/* ACPI MADT IOAPIC discovery and ISA IRQ routing (see include/ioapic.h).
 *
 * Scope: ISA lines 0..15 (PIT, PS/2 and the PCI Interrupt Line values module
 * drivers pass to irq_attach) land on IOAPIC redirection entries with vector
 * 32 + line, fixed delivery, physical destination = BSP LAPIC. Overrides from
 * MADT type 2 supply GSI, polarity and trigger. Each RTE is allocated when a
 * line is routed and cleared (masked, vector 0) when it is released, so an
 * unload/reload cycle never leaves a stale or duplicate entry behind; the
 * dump after every change proves that on the serial log. */
#ifdef ARK_IOAPIC_HOST_TEST
#include <string.h>
#include "ioapic.h"
#else
#include "ark.h"
#include "mmio.h"
#include "acpi.h"
#include "ioapic.h"
#endif

/* ---- pure MADT parsing (host-testable) ----------------------------------- */

static uint32_t rd32(const uint8_t *p) {
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}

unsigned ioapic_parse_madt(const uint8_t *m, uint32_t length, IoApicTopology *out) {
    if (!out)
        return 0;
    memset(out, 0, sizeof *out);
    for (unsigned i = 0; i < IOAPIC_ISA_IRQS; i++)
        out->isa_gsi[i] = i;
    if (!m || length < 44 || memcmp(m, "APIC", 4) || rd32(m + 4) != length)
        return 0;
    out->madt_flags = rd32(m + 40);
    for (uint32_t at = 44; at + 2 <= length;) {
        unsigned type = m[at], size = m[at + 1];
        if (size < 2 || size > length - at) {
            out->rejected++;
            break; /* a malformed entry poisons everything after it */
        }
        const uint8_t *e = m + at;
        if (type == 1) {
            uint32_t address = size >= 12 ? rd32(e + 4) : 0;
            uint32_t base = size >= 12 ? rd32(e + 8) : 0;
            bool overlap = false;
            for (unsigned i = 0; i < out->count; i++)
                if (out->ioapic[i].address == address || out->ioapic[i].gsi_base == base)
                    overlap = true;
            if (size < 12 || !address || (address & 15) || address < 0x100000u || overlap ||
                out->count >= IOAPIC_MAX)
                out->rejected++;
            else
                out->ioapic[out->count++] = (IoApicDesc){e[2], address, base};
        } else if (type == 2) {
            unsigned bus = size >= 10 ? e[2] : 1, source = size >= 10 ? e[3] : 255;
            uint32_t gsi = size >= 10 ? rd32(e + 4) : 0;
            uint16_t flags = size >= 10 ? (uint16_t)(e[8] | (e[9] << 8)) : 0;
            if (size < 10 || bus != 0 || source >= IOAPIC_ISA_IRQS ||
                out->isa_override[source] || gsi == IOAPIC_GSI_NONE)
                out->rejected++;
            else {
                out->isa_override[source] = 1;
                out->isa_gsi[source] = gsi;
                out->isa_flags[source] = flags;
                out->overrides++;
            }
        }
        at += size;
    }
    /* An overridden GSI belongs to its override source: the identity line of
     * that number (e.g. ISA 2 once ISA 0 -> GSI 2) has no pin of its own, and
     * two sources can never share one GSI. */
    for (unsigned i = 0; i < IOAPIC_ISA_IRQS; i++) {
        if (out->isa_gsi[i] == IOAPIC_GSI_NONE)
            continue;
        for (unsigned j = 0; j < IOAPIC_ISA_IRQS; j++) {
            if (j == i || out->isa_gsi[j] != out->isa_gsi[i])
                continue;
            /* Keep the explicit override; drop the identity default. When both
             * are overrides, the lower source wins deterministically. */
            if (!out->isa_override[i] || (out->isa_override[j] && j < i)) {
                out->isa_gsi[i] = IOAPIC_GSI_NONE;
                break;
            }
        }
    }
    return out->count;
}

uint32_t ioapic_rte_mode(uint16_t flags) {
    uint32_t mode = 0;
    if ((flags & 3u) == IOAPIC_FLAG_POLARITY_LOW)
        mode |= 1u << 13; /* active low; 0/1 (conforming ISA / high) = high */
    if ((flags & (3u << 2)) == IOAPIC_FLAG_TRIGGER_LEVEL)
        mode |= 1u << 15; /* level; 0/1 (conforming ISA / edge) = edge */
    return mode;
}

#ifndef ARK_IOAPIC_HOST_TEST
/* ---- hardware ------------------------------------------------------------ */

#define RTE_MASKED (1u << 16)
#define VECTOR_BASE 32u
typedef struct {
    volatile uint32_t *regs;
    uint32_t gsi_base;
    unsigned pins;
    uint8_t id, version;
} Chip;
static Chip chips[IOAPIC_MAX];
static unsigned chip_count;
static IoApicTopology topo;
static bool active, x2apic;
static volatile uint32_t *lapic;
static uint32_t bsp_id;
static uint8_t routed[IOAPIC_ISA_IRQS];
static volatile uint32_t lock_word;

static uint64_t lock(void) {
    uint64_t flags;
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(flags)::"memory");
    while (__atomic_exchange_n(&lock_word, 1, __ATOMIC_ACQUIRE))
        __asm__ volatile("pause");
    return flags;
}
static void unlock(uint64_t flags) {
    __atomic_store_n(&lock_word, 0, __ATOMIC_RELEASE);
    if (flags & (1u << 9))
        __asm__ volatile("sti" ::: "memory");
}
static uint32_t reg_read(const Chip *c, unsigned reg) {
    c->regs[0] = reg;
    return c->regs[4];
}
static void reg_write(const Chip *c, unsigned reg, uint32_t value) {
    c->regs[0] = reg;
    c->regs[4] = value;
}
static uint64_t rdmsr(uint32_t msr) {
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}
static void wrmsr(uint32_t msr, uint64_t value) {
    __asm__ volatile("wrmsr" ::"a"((uint32_t)value), "d"((uint32_t)(value >> 32)), "c"(msr));
}
static void lapic_write(unsigned offset, uint32_t value) {
    if (x2apic)
        wrmsr(0x800 + (offset >> 4), value);
    else if (lapic) {
        lapic[offset / 4] = value;
        (void)lapic[0x20 / 4];
    }
}
/* Resolve a GSI to (chip, pin); false when no IOAPIC owns it. */
static bool locate(uint32_t gsi, unsigned *chip, unsigned *pin) {
    for (unsigned i = 0; i < chip_count; i++)
        if (gsi >= chips[i].gsi_base && gsi - chips[i].gsi_base < chips[i].pins) {
            *chip = i;
            *pin = gsi - chips[i].gsi_base;
            return true;
        }
    return false;
}

/* Bounded text building for one serial line at a time. */
typedef struct {
    char *buf;
    size_t cap, at;
} Text;
static void put(Text *t, const char *s) {
    while (*s && t->at + 1 < t->cap)
        t->buf[t->at++] = *s++;
    t->buf[t->at] = 0;
}
static void num(Text *t, uint64_t v) {
    char n[24];
    uint_to_str(v, n);
    put(t, n);
}
static void hex(Text *t, uint64_t v) {
    char n[19] = "0x";
    unsigned digits = 1;
    for (uint64_t x = v >> 4; x; x >>= 4)
        digits++;
    for (unsigned i = 0; i < digits; i++)
        n[2 + i] = "0123456789abcdef"[(v >> ((digits - 1 - i) * 4)) & 15];
    n[2 + digits] = 0;
    put(t, n);
}
static const char *mode_text(uint32_t low) {
    static const char *names[4] = {"edge/high", "edge/low", "level/high", "level/low"};
    return names[((low >> 13) & 1) | (((low >> 15) & 1) << 1)];
}

bool ioapic_init(void) {
    uint32_t a = 1, b, c, d;
    __asm__ volatile("cpuid" : "+a"(a), "=b"(b), "=c"(c), "=d"(d));
    if (!(d & (1u << 9))) {
        serial_write("[irq] legacy PIC only (CPU reports no local APIC)\n");
        return false;
    }
    const uint8_t *m = acpi_find_table("APIC");
    if (!m) {
        serial_write("[irq] legacy PIC only (no valid ACPI MADT)\n");
        return false;
    }
    if (!ioapic_parse_madt(m, rd32(m + 4), &topo)) {
        serial_write("[irq] legacy PIC only (MADT lists no usable IOAPIC)\n");
        return false;
    }
    /* BSP LAPIC: IOAPIC-delivered IRQs need its EOI. Enable it here, before
     * smp_init, so the very first PIT interrupt can be acknowledged. */
    uint64_t base = rdmsr(0x1b);
    if (!(base & (1u << 11)))
        wrmsr(0x1b, base |= 1u << 11);
    x2apic = (base & (1u << 10)) != 0;
    if (x2apic)
        bsp_id = (uint32_t)rdmsr(0x802);
    else {
        uint64_t physical = base & 0x000ffffffffff000ull;
        if (!physical || !platform_map_mmio(physical, 4096)) {
            serial_write("[irq] legacy PIC only (BSP LAPIC MMIO unavailable)\n");
            return false;
        }
        lapic = (volatile uint32_t *)(uintptr_t)physical;
        bsp_id = lapic[0x20 / 4] >> 24;
    }
    if (bsp_id > 255) {
        /* Compatibility-format RTEs carry an 8-bit destination. */
        serial_write("[irq] legacy PIC only (BSP APIC id > 255 needs interrupt remapping)\n");
        lapic = 0;
        x2apic = false;
        return false;
    }
    char line[160];
    for (unsigned i = 0; i < topo.count; i++) {
        const IoApicDesc *desc = &topo.ioapic[i];
        Text t = {line, sizeof line, 0};
        if (!platform_map_mmio(desc->address, 4096)) {
            put(&t, "[irq] IOAPIC id=");
            num(&t, desc->id);
            put(&t, " at ");
            hex(&t, desc->address);
            put(&t, " skipped: MMIO map failed\n");
            serial_write(line);
            continue;
        }
        Chip *chip = &chips[chip_count];
        chip->regs = (volatile uint32_t *)(uintptr_t)desc->address;
        chip->gsi_base = desc->gsi_base;
        chip->id = desc->id;
        uint32_t version = reg_read(chip, 1);
        chip->version = (uint8_t)version;
        chip->pins = ((version >> 16) & 0xff) + 1;
        if (version == 0xffffffffu || chip->pins > 240) {
            put(&t, "[irq] IOAPIC id=");
            num(&t, desc->id);
            put(&t, " at ");
            hex(&t, desc->address);
            put(&t, " skipped: no device behind window\n");
            serial_write(line);
            continue;
        }
        /* Start from a clean table: firmware may leave ExtINT/virtual-wire
         * or stale entries; every pin is masked with vector 0. */
        for (unsigned pin = 0; pin < chip->pins; pin++) {
            reg_write(chip, 0x10 + pin * 2, RTE_MASKED);
            reg_write(chip, 0x11 + pin * 2, 0);
        }
        chip_count++;
        put(&t, "[irq] IOAPIC id=");
        num(&t, chip->id);
        put(&t, " at ");
        hex(&t, desc->address);
        put(&t, " gsi_base=");
        num(&t, chip->gsi_base);
        put(&t, " gsi ");
        num(&t, chip->gsi_base);
        put(&t, "..");
        num(&t, chip->gsi_base + chip->pins - 1);
        put(&t, " pins=");
        num(&t, chip->pins);
        put(&t, " ver=");
        hex(&t, chip->version);
        put(&t, "\n");
        serial_write(line);
    }
    if (!chip_count) {
        serial_write("[irq] legacy PIC only (no IOAPIC window responded)\n");
        lapic = 0;
        x2apic = false;
        return false;
    }
    for (unsigned irq = 0; irq < IOAPIC_ISA_IRQS; irq++) {
        Text t = {line, sizeof line, 0};
        unsigned chip, pin;
        if (topo.isa_override[irq]) {
            put(&t, "[irq] ISA override irq");
            num(&t, irq);
            put(&t, " -> gsi");
            num(&t, topo.isa_gsi[irq]);
            put(&t, " flags=");
            hex(&t, topo.isa_flags[irq]);
            put(&t, " (");
            put(&t, mode_text(ioapic_rte_mode(topo.isa_flags[irq])));
            put(&t, ")\n");
            serial_write(line);
        } else if (topo.isa_gsi[irq] == IOAPIC_GSI_NONE ||
                   !locate(topo.isa_gsi[irq], &chip, &pin)) {
            put(&t, "[irq] ISA irq");
            num(&t, irq);
            put(&t, " has no IOAPIC pin\n");
            serial_write(line);
        }
    }
    /* Software-enable the LAPIC (spurious vector 255), accept every priority
     * and mask LINT0: the 8259 virtual-wire ExtINT path is retired. */
    lapic_write(0xf0, 0x1ff);
    lapic_write(0x80, 0);
    lapic_write(0x350, 0x10700);
    Text t = {line, sizeof line, 0};
    put(&t, "[irq] IOAPIC mode: ");
    num(&t, chip_count);
    put(&t, " IOAPIC(s), ");
    num(&t, topo.overrides);
    put(&t, " ISA override(s), dest ");
    put(&t, x2apic ? "x2APIC" : "xAPIC");
    put(&t, " id ");
    num(&t, bsp_id);
    put(&t, " physical; VT-d interrupt remapping not used (compat RTEs); MSI/MSI-X TODO\n");
    serial_write(line);
    active = true;
    return true;
}

static int route_locked(unsigned irq, char *log, size_t cap) {
    Text t = {log, cap, 0};
    if (irq >= IOAPIC_ISA_IRQS)
        return -22;
    uint32_t gsi = topo.isa_gsi[irq];
    unsigned chip, pin;
    if (routed[irq]) {
        put(&t, "[irq] route isa");
        num(&t, irq);
        put(&t, " rejected: RTE already allocated\n");
        return -16;
    }
    if (gsi == IOAPIC_GSI_NONE || !locate(gsi, &chip, &pin)) {
        put(&t, "[irq] route isa");
        num(&t, irq);
        put(&t, " rejected: no IOAPIC pin\n");
        return -19;
    }
    uint32_t low = reg_read(&chips[chip], 0x10 + pin * 2);
    if (!(low & RTE_MASKED) || (low & 0xff)) {
        /* Never stack a second owner onto a live or stale entry. */
        put(&t, "[irq] route isa");
        num(&t, irq);
        put(&t, " rejected: gsi");
        num(&t, gsi);
        put(&t, " RTE busy\n");
        return -16;
    }
    uint32_t vector = VECTOR_BASE + irq;
    uint32_t mode = ioapic_rte_mode(topo.isa_flags[irq]);
    reg_write(&chips[chip], 0x11 + pin * 2, bsp_id << 24);
    reg_write(&chips[chip], 0x10 + pin * 2, RTE_MASKED | mode | vector);
    reg_write(&chips[chip], 0x10 + pin * 2, mode | vector);
    routed[irq] = 1;
    put(&t, "[irq] route+ isa");
    num(&t, irq);
    put(&t, " -> ioapic");
    num(&t, chips[chip].id);
    put(&t, " gsi");
    num(&t, gsi);
    put(&t, " pin");
    num(&t, pin);
    put(&t, " vec");
    num(&t, vector);
    put(&t, " ");
    put(&t, mode_text(mode));
    put(&t, " dest apic");
    num(&t, bsp_id);
    put(&t, "\n");
    return 0;
}
int ioapic_route(unsigned irq) {
    if (!active)
        return -19;
    char log[160];
    uint64_t flags = lock();
    int rc = route_locked(irq, log, sizeof log);
    unlock(flags);
    if (rc != -22)
        serial_write(log);
    return rc;
}
void ioapic_release(unsigned irq) {
    if (!active || irq >= IOAPIC_ISA_IRQS)
        return;
    unsigned chip, pin;
    char log[96];
    Text t = {log, sizeof log, 0};
    uint64_t flags = lock();
    bool was = routed[irq];
    if (was && locate(topo.isa_gsi[irq], &chip, &pin)) {
        reg_write(&chips[chip], 0x10 + pin * 2, RTE_MASKED);
        reg_write(&chips[chip], 0x11 + pin * 2, 0);
    }
    routed[irq] = 0;
    unlock(flags);
    if (was) {
        put(&t, "[irq] route- isa");
        num(&t, irq);
        put(&t, " gsi");
        num(&t, topo.isa_gsi[irq]);
        put(&t, " released (masked, vector cleared)\n");
        serial_write(log);
    }
}
static void set_mask(unsigned irq, bool masked) {
    if (!active || irq >= IOAPIC_ISA_IRQS)
        return;
    unsigned chip, pin;
    uint64_t flags = lock();
    if (routed[irq] && locate(topo.isa_gsi[irq], &chip, &pin)) {
        uint32_t low = reg_read(&chips[chip], 0x10 + pin * 2);
        reg_write(&chips[chip], 0x10 + pin * 2,
                  masked ? (low | RTE_MASKED) : (low & ~RTE_MASKED));
    }
    unlock(flags);
}
void ioapic_mask(unsigned irq) {
    set_mask(irq, true);
}
void ioapic_unmask(unsigned irq) {
    set_mask(irq, false);
}
void ioapic_eoi(void) {
    lapic_write(0xb0, 0);
}
unsigned ioapic_dump(const char *reason) {
    if (!active)
        return 0;
    char line[512];
    Text t = {line, sizeof line, 0};
    unsigned live = 0, leftovers = 0, duplicates = 0;
    uint8_t seen[256] = {0};
    put(&t, "[irq] RTE table");
    if (reason) {
        put(&t, " (");
        put(&t, reason);
        put(&t, ")");
    }
    put(&t, ":");
    uint64_t flags = lock();
    for (unsigned i = 0; i < chip_count; i++)
        for (unsigned pin = 0; pin < chips[i].pins; pin++) {
            uint32_t low = reg_read(&chips[i], 0x10 + pin * 2);
            if ((low & RTE_MASKED) && !(low & 0xff))
                continue;
            uint32_t gsi = chips[i].gsi_base + pin, vector = low & 0xff;
            /* Live entries must belong to the routed ISA line that owns this
             * GSI with the matching vector; anything else is a leftover. */
            bool owned = false;
            for (unsigned irq = 0; irq < IOAPIC_ISA_IRQS; irq++)
                if (routed[irq] && topo.isa_gsi[irq] == gsi && vector == VECTOR_BASE + irq)
                    owned = true;
            if (!owned)
                leftovers++;
            if (seen[vector]++)
                duplicates++;
            live++;
            put(&t, " gsi");
            num(&t, gsi);
            put(&t, "=v");
            num(&t, vector);
            if (low & RTE_MASKED)
                put(&t, "(masked)");
        }
    unlock(flags);
    if (!live)
        put(&t, " none");
    put(&t, " | active=");
    num(&t, live);
    put(&t, " leftovers=");
    num(&t, leftovers);
    put(&t, " duplicates=");
    num(&t, duplicates);
    put(&t, "\n");
    serial_write(line);
    return live;
}
void ioapic_describe(unsigned irq, char *out, size_t cap) {
    if (!cap)
        return;
    out[0] = 0;
    unsigned chip, pin;
    if (!active || irq >= IOAPIC_ISA_IRQS || !routed[irq] ||
        !locate(topo.isa_gsi[irq], &chip, &pin))
        return;
    Text t = {out, cap, 0};
    put(&t, "ioapic");
    num(&t, chips[chip].id);
    put(&t, " gsi");
    num(&t, topo.isa_gsi[irq]);
    put(&t, " vec");
    num(&t, VECTOR_BASE + irq);
    put(&t, " ");
    put(&t, mode_text(ioapic_rte_mode(topo.isa_flags[irq])));
}
#endif
