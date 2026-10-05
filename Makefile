CC := gcc
LD := ld
CFLAGS := -std=c11 -O2 -Wall -Wextra -Werror -ffreestanding -fno-builtin -fno-stack-protector -fno-pie -fno-asynchronous-unwind-tables -m64 -mno-red-zone -mgeneral-regs-only -mcmodel=small -Iinclude
LDFLAGS := -nostdlib -z noexecstack -z max-page-size=0x1000 -T boot/linker.ld
C_SOURCES := $(wildcard kernel/*.c)
OBJECTS := $(patsubst kernel/%.c,build/%.o,$(C_SOURCES)) build/entry.o build/ap.o build/interrupts.o build/user_entry.o build/module_asm.o build/drivers_embed.o build/programs_embed.o
.PHONY: all clean run iso check check-vm check-external
all: iso disk
build:
	mkdir -p build
build/%.o: kernel/%.c $(wildcard include/*.h) | build
	$(CC) $(CFLAGS) -c $< -o $@
build/arkfs2_seal.o: kernel/arkfs2_seal.c $(wildcard include/*.h) | build
	$(CC) $(CFLAGS) -Ithird_party/bearssl/inc -c $< -o $@
build/process.o: kernel/process_vm.inc
build/virtio_gpu.o: kernel/glass_shaders.inc $(wildcard third_party/virgl-protocol/*.h)
kernel/glass_shaders.inc: scripts/build-glass-shaders.py
	python3 $<
build/entry.o: boot/entry.S | build
	$(CC) -m64 -ffreestanding -fno-pie -c $< -o $@
build/interrupts.o: kernel/interrupts.S | build
	$(CC) -m64 -ffreestanding -fno-pie -c $< -o $@
build/module_asm.o: kernel/module_asm.S | build
	$(CC) -m64 -ffreestanding -fno-pie -c $< -o $@
# Inbox NIC driver: a real .arco image linked into kernel.elf so every boot —
# bare ISO, foreign disk, fresh install — has networking before any manifest.
build/e1000.arco: sdk/driver_e1000.c scripts/arco.py scripts/arco.ld $(wildcard include/*.h) | build
	python3 scripts/arco.py sdk/driver_e1000.c -o $@ --name e1000 --version 1.0.0
build/drivers_embed.o: kernel/drivers_embed.S build/e1000.arco | build
	$(CC) -m64 -ffreestanding -fno-pie -c $< -o $@
build/kernel.elf: $(OBJECTS) boot/linker.ld
	$(LD) $(LDFLAGS) $(OBJECTS) $(BEARSSL_OBJECTS) -o $@
	grub-file --is-x86-multiboot2 $@
iso: build/arkos-0.13.0.iso
build/arkos-0.13.0.iso: build/kernel.elf boot/grub.cfg scripts/mkiso.py
	mkdir -p build/iso/boot/grub
	python3 -c 'from pathlib import Path; [Path("build/iso/boot",n).unlink(missing_ok=True) for n in ("kernel.elf.gz","kernel.elf.xz")]'
	cp build/kernel.elf build/iso/boot/kernel.elf
	cp boot/grub.cfg build/iso/boot/grub/grub.cfg
	python3 scripts/mkiso.py build/iso $@ ARKOS0130
run: iso disk
	qemu-system-x86_64 -machine q35 -cpu max -smp 4 -m 512M -cdrom build/arkos-0.13.0.iso -boot d -vga vmware -global vmware-svga.vgamem_mb=64 -serial stdio -netdev user,id=net0 -device e1000,netdev=net0,romfile= -drive file=build/arkos-data.img,format=raw,if=ide,index=0 -device virtio-multitouch-pci -device virtio-tablet-pci
check:
	sh tests/test-host.sh
check-vm: iso
	python3 tests/v8_desktop_test.py bios
	python3 tests/v8_desktop_test.py uefi
	python3 tests/v7_install_test.py bios
	python3 tests/v7_install_test.py uefi
	python3 tests/panic_test.py
# This production-ISO test includes FAT32/NTFS readback and a local HTTP server.
check-external: check-runtime
.PHONY: check-runtime
check-runtime: iso
	python3 tests/v8_runtime_test.py
clean:
	python3 -c 'from pathlib import Path; import shutil; p=Path("build"); [f.unlink() for f in list(p.glob("*.o"))+[p/"kernel.elf",p/"arkos-0.13.0.iso"] if f.is_file()]; [shutil.rmtree(p/n,ignore_errors=True) for n in ("user","apps","sdk","iso")]'

.PHONY: disk
disk: | build
	@test -f build/arkos-data.img || python3 scripts/create-disk.py build/arkos-data.img

.PHONY: check-motion check-motion-host
check-motion-host: | build
	$(CC) -std=c11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -Iinclude tests/motion_host_test.c user/motion.c user/raster.c -lm -o build/motion-host-test
	ASAN_OPTIONS=detect_leaks=0 ./build/motion-host-test
	$(CC) -std=c11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -Iinclude tests/ribbon_host_test.c tests/ribbon_reference.c user/ribbon.c user/raster.c user/motion.c -o build/ribbon-host-test
	ASAN_OPTIONS=detect_leaks=0 ./build/ribbon-host-test
check-motion: iso check-motion-host
	python3 tests/motion_vm_test.py --scenario reference

include user.mk
build/user_entry.o: kernel/user_entry.S | build
	$(CC) $(CFLAGS) -c $< -o $@

include net.mk

.PHONY: check-security-host check-protection
check-security-host: | build
	$(CC) -std=c11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -DARK_STORAGE_HOST_TEST -Iinclude tests/accounts_test.c kernel/vfs.c -o build/accounts-test
	ASAN_OPTIONS=detect_leaks=0 ./build/accounts-test
	$(CC) -std=c11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -DARK_STORAGE_HOST_TEST -DARK_ACCOUNTS_HOST_TEST -Iinclude tests/service_host_test.c kernel/services.c kernel/vfs.c kernel/accounts.c kernel/permissions.c kernel/device.c -o build/service-host-test
	ASAN_OPTIONS=detect_leaks=0 ./build/service-host-test
.PHONY: check-device-host
check-device-host: | build
	$(CC) -std=c11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -DARK_DEVICE_HOST_TEST -DARK_PCI_HOST_TEST -Iinclude tests/device_host_test.c kernel/device.c kernel/pci.c kernel/lib.c -o build/device-host-test
	ASAN_OPTIONS=detect_leaks=0 ./build/device-host-test
.PHONY: check-module-host
check-module-host: | build
	$(CC) -std=c11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -DARK_MODULE_HOST_TEST -DARK_DEVICE_HOST_TEST -DARK_PCI_HOST_TEST -DARK_BLOB_HOST_TEST -Iinclude tests/module_host_test.c kernel/module.c kernel/device.c kernel/pci.c kernel/blob.c kernel/sha256.c kernel/lib.c kernel/alloc.c -o build/module-host-test
	ASAN_OPTIONS=detect_leaks=0 ./build/module-host-test
.PHONY: check-ioapic-host
check-ioapic-host: | build
	$(CC) -std=c11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -Iinclude tests/ioapic_host_test.c -o build/ioapic-host-test
	./build/ioapic-host-test
check-protection: iso
	ARK_SMP_TEST=1 python3 tests/process_test.py
	python3 tests/process_api_test.py

.PHONY: check-gpu
check-gpu: iso
	ARKOS_CC="$(CC)" ARKOS_LD="$(LD)" python3 tests/gpu_glass_test.py

build/ap.o: boot/ap.S | build
	$(CC) -m64 -ffreestanding -fno-pie -c $< -o $@

include arm64.mk

.PHONY: check-v6-host
check-v6-host: | build
	$(CC) -std=c11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -DARK_MICROCODE_HOST_TEST -Iinclude tests/v6_host_test.c user/html.c kernel/microcode.c -o build/v6-host-test
	ASAN_OPTIONS=detect_leaks=0 ./build/v6-host-test

.PHONY: check-v7-host
check-v7-host: | build
	$(CC) -std=c11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -DARK_BLOB_HOST_TEST -Iinclude tests/blob_host_test.c kernel/blob.c kernel/alloc.c -o build/blob-host-test
	ASAN_OPTIONS=detect_leaks=0 ./build/blob-host-test
	$(CC) -std=c11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -Iinclude tests/pinyin_test.c user/pinyin.c -o build/pinyin-host-test
	ASAN_OPTIONS=detect_leaks=0 ./build/pinyin-host-test

.PHONY: check-package-host
check-package-host: | build
	$(CC) -std=c11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -DARK_BLOB_HOST_TEST -DARK_PACKAGE_HOST_TEST -Iinclude tests/package_host_test.c kernel/package.c kernel/blob.c kernel/alloc.c kernel/elf.c kernel/sha256.c -o build/package-host-test
	ASAN_OPTIONS=detect_leaks=0 ./build/package-host-test

.PHONY: check-v8-host
check-v8-host: | build
	$(CC) -std=c11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -ffunction-sections -fdata-sections -Wl,--gc-sections -DARK_API_HOST_TEST -Iinclude -Isdk tests/v8_app_core_test.c user/unicode.c -lm -o build/v8-app-core-test
	ASAN_OPTIONS=detect_leaks=0 ./build/v8-app-core-test

# Native images retain private runtime mappings; only identical stored font
# payloads are shared in this immutable build-time container.
build/programs_embed.o: $(USER_PROGRAMS) scripts/pack-user-images.py scripts/build-system-packages.py assets/fonts/wqy-microhei.ttc
	python3 scripts/pack-user-images.py $(USER_PROGRAMS)
	python3 scripts/build-system-packages.py
	$(CC) -m64 -ffreestanding -fno-pie -c build/programs.S -o $@

include wasm.mk

.PHONY: check-arkfs2
ARKFS2_SEAL_HOST = third_party/bearssl/src/aead/gcm.c third_party/bearssl/src/hash/ghash_ctmul64.c third_party/bearssl/src/symcipher/aes_ct.c third_party/bearssl/src/symcipher/aes_ct_ctr.c third_party/bearssl/src/symcipher/aes_ct_enc.c
check-arkfs2: | build
	$(CC) -std=c11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -DARK_STORAGE_HOST_TEST -Iinclude -Ithird_party/bearssl/inc -Ithird_party/bearssl/src -DBR_AES_X86NI=0 -DBR_SSE2=0 tests/arkfs2_host.c kernel/arkfs2.c kernel/arkfs2_lz.c kernel/arkfs2_seal.c kernel/sha256.c $(ARKFS2_SEAL_HOST) -o build/arkfs2-host-test
	ASAN_OPTIONS=detect_leaks=0 ./build/arkfs2-host-test
.PHONY: check-arkfs2-seal
check-arkfs2-seal: | build
	$(CC) -std=c11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -DARK_STORAGE_HOST_TEST -Iinclude -Ithird_party/bearssl/inc -Ithird_party/bearssl/src -DBR_AES_X86NI=0 -DBR_SSE2=0 tests/arkfs2_seal_host.c kernel/arkfs2_seal.c kernel/arkfs2_lz.c kernel/sha256.c $(ARKFS2_SEAL_HOST) -o build/arkfs2-seal-host-test
	ASAN_OPTIONS=detect_leaks=0 ./build/arkfs2-seal-host-test

.PHONY: check-registry-host
check-registry-host: | build
	$(CC) -std=c11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -DARK_BLOB_HOST_TEST -DARK_REGISTRY_HOST_TEST -Iinclude tests/registry_host_test.c kernel/registry.c kernel/blob.c kernel/sha256.c kernel/alloc.c -o build/registry-host-test
	ASAN_OPTIONS=detect_leaks=0 ./build/registry-host-test
