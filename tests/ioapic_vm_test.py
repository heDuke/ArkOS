#!/usr/bin/env python3
"""Guest smoke for IOAPIC interrupt routing (Tester 13700H / QEMU q35 gates).

  1. Boot log shows MADT-parsed IOAPIC address, GSI range, RTE table; 8259 IMR
     is fully masked when the IOAPIC path is active.
  2. Inbox e1000 attaches through irq_attach; after DHCP its module detail
     reports a rising ISR count (n=). A poll-only driver would show
     "irq:none (poll-only)" and fail this gate.
  3. Unload then reinstall e1000: the ISA-11 RTE is released then reallocated
     with leftovers=0 and duplicates=0.

    ARKOS_ISO=... python3 tests/ioapic_vm_test.py
"""
import os
import re
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from vm import VM, ROOT  # noqa: E402

os.environ["ARKOS_MACHINE"] = "q35"
os.environ["ARKOS_SMP"] = "4"
ADMIN = "IoapicGate42!"
out = ROOT / "build" / "test-ioapic-vm"
out.mkdir(parents=True, exist_ok=True)


def seed_source_blob(disk):
    """Park build/e1000.arco as an ordinary admin-owned blob (uid 1000) so the
    guest can reinstall it with `dev install blob:e1000src` after removal.
    Uses the blob-store layout helpers of scripts/seed-drivers.py; no manifest
    is written, so boot still loads the inbox copy."""
    import importlib.util
    import zlib
    spec = importlib.util.spec_from_file_location("seed", ROOT / "scripts" / "seed-drivers.py")
    seed = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(seed)
    arco = (ROOT / "build" / "e1000.arco").read_bytes()
    assert arco[:8] == b"ARCO1\0\0\0"
    offset = seed.find_arkfs_offset(disk)
    with disk.open("r+b") as handle:
        lba = seed.ARENA_LBA + 64
        seed.write_blob(handle, offset, lba, arco)
        root = seed.bank_root([(seed.KERNEL_UID, lba, len(arco),
                                zlib.crc32(arco) & 0xFFFFFFFF, "e1000src")])
        handle.seek((offset + seed.INDEX_LBA + seed.BANK_STRIDE) * 512)
        handle.write(root)
        handle.write(bytes(seed.BANK_STRIDE * 512 - seed.ROOT_BYTES))
        handle.seek((offset + seed.INDEX_LBA) * 512)
        handle.write(bytes(seed.BANK_STRIDE * 512))


def fresh_disk():
    disk = out / "data.img"
    disk.unlink(missing_ok=True)
    subprocess.run([sys.executable, "scripts/create-disk.py", str(disk)],
                   check=True, cwd=ROOT)
    return disk


def sign_in(vm):
    vm.wait("[session] setup ready")
    vm.type(ADMIN)
    vm.key("tab")
    vm.type(ADMIN)
    vm.key("ret")
    vm.wait("[session] desktop unlocked", 60)


def command(vm, text, expect=None, timeout=30):
    start = len(vm.log.read_text())
    vm.command(text)
    if expect:
        vm.wait(expect, timeout, after=start)


disk = fresh_disk()
seed_source_blob(disk)
vm = VM("ioapic-smoke", disk=str(disk), firmware="bios", device=None)
try:
    # ---- gate 1: boot path -------------------------------------------------
    log = vm.log.read_text()
    assert re.search(r"\[irq\] IOAPIC id=\d+ at 0x[0-9a-f]+ gsi_base=\d+ gsi \d+\.\.\d+", log), log[-3000:]
    assert "[irq] IOAPIC mode:" in log, log[-3000:]
    assert "8259 PIC fully masked" in log, log[-3000:]
    assert "IMR master=" in log and "ff" in log.lower(), log[-3000:]
    assert "[irq] RTE table (boot):" in log, log[-3000:]
    assert "leftovers=0" in log and "duplicates=0" in log, log[-3000:]
    assert "[irq] legacy PIC only" not in log, "fell back to PIC on q35"
    assert "[exception]" not in log and "[panic]" not in log, log[-2000:]
    print("PASS gate1: MADT IOAPIC address/GSI + RTE dump; 8259 fully masked", flush=True)

    # ---- gate 2: e1000 interrupts, not poll-only ---------------------------
    assert "[irq] e1000 irq_attach(11) bound -> ioapic" in log, log[-3000:]
    assert "[drv] e1000 irq 11 bound" in log, log[-3000:]
    vm.wait("[net] DHCP ready", 60)
    time.sleep(2)  # let a few more RX IRQs land
    sign_in(vm)
    vm.terminal()
    before = len(vm.log.read_text())
    command(vm, "dev drivers", "e1000")
    after = vm.log.read_text()[before:]
    assert "irq:none" not in after, "e1000 reported poll-only: " + after
    m = re.search(r"irq11 ioapic\d+ gsi\d+ n=(\d+)", after)
    assert m, "missing irq11 ISR counter in drivers detail: " + after
    assert int(m.group(1)) > 0, "ISR count stayed 0 (poll-only would look the same): " + after
    command(vm, "net", "link up")
    print(f"PASS gate2: e1000 irq11 ISR count n={m.group(1)} (not poll-only)", flush=True)

    # ---- gate 3: unload then reinstall ------------------------------------
    before = len(vm.log.read_text())
    command(vm, "dev remove e1000", "Removed e1000")
    log = vm.log.read_text()[before:]
    assert "[irq] route- isa11" in log, log
    assert "leftovers=0" in log and "duplicates=0" in log, log
    assert "gsi11=v43" not in log.split("RTE table (after route-)")[-1][:200] if "RTE table (after route-)" in log else True
    command(vm, "blobs")
    time.sleep(1)
    # Reinstall from the admin-owned source blob seeded before boot.
    before = len(vm.log.read_text())
    command(vm, "dev install blob:e1000src", "Installed e1000", timeout=60)
    log = vm.log.read_text()[before:]
    assert "[irq] route+ isa11" in log, log
    assert "[irq] e1000 irq_attach(11) bound -> ioapic" in log, log
    assert "leftovers=0" in log and "duplicates=0" in log, log
    # Count the active gsi11 entries in the last RTE dump: exactly one.
    dumps = [line for line in log.splitlines() if "RTE table (after route+)" in line]
    assert dumps, log
    assert dumps[-1].count("gsi11=") == 1, dumps[-1]
    before = len(vm.log.read_text())
    time.sleep(3)
    command(vm, "dev drivers", "e1000")
    after = vm.log.read_text()[before:]
    m2 = re.search(r"irq11 ioapic\d+ gsi\d+ n=(\d+)", after)
    assert m2, "reinstalled e1000 lost its IRQ binding: " + after
    print(f"PASS gate3: RTE released on unload and reallocated on reinstall (n={m2.group(1)} since reattach)", flush=True)
    print("PASS IOAPIC smoke gates (q35)", flush=True)
finally:
    vm.close()
