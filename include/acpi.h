#ifndef ARK_ACPI_H
#define ARK_ACPI_H
/* Minimal static-table discovery shared by SMP and IOAPIC setup.
 * Header-only (static) so every fixture that links smp.c or ioapic.c keeps
 * linking without a new object. Tables must sit inside the boot identity map
 * (1 MiB .. 4 GiB) and carry a valid checksum; no AML is interpreted. */
#include "ark.h"
extern const uint8_t *platform_acpi_rsdp(void);
static inline bool acpi_checksum(const uint8_t *p, unsigned n) {
    uint8_t sum = 0;
    for (unsigned i = 0; i < n; i++)
        sum += p[i];
    return !sum;
}
static inline const uint8_t *acpi_table_at(uint64_t address) {
    if (address < 0x100000 || address > 0xffff0000)
        return 0;
    const uint8_t *p = (const uint8_t *)(uintptr_t)address;
    uint32_t size;
    memcpy(&size, p + 4, 4);
    if (size < 36 || size > 65536 || address + size > 0x100000000ull)
        return 0;
    return acpi_checksum(p, size) ? p : 0;
}
/* First checksum-valid table whose signature matches sig (4 chars). */
static inline const uint8_t *acpi_find_table(const char *sig) {
    const uint8_t *r = platform_acpi_rsdp();
    if (!r || strncmp((const char *)r, "RSD PTR ", 8) || !acpi_checksum(r, 20))
        return 0;
    uint64_t addr = 0;
    unsigned stride = 4;
    if (r[15] >= 2 && acpi_checksum(r, 36)) {
        memcpy(&addr, r + 24, 8);
        stride = 8;
    }
    if (!addr) {
        uint32_t low;
        memcpy(&low, r + 16, 4);
        addr = low;
        stride = 4;
    }
    const uint8_t *root = acpi_table_at(addr);
    if (!root)
        return 0;
    uint32_t len;
    memcpy(&len, root + 4, 4);
    if (strncmp((const char *)root, stride == 8 ? "XSDT" : "RSDT", 4) || (len - 36) % stride)
        return 0;
    for (unsigned i = 36; i + stride <= len; i += stride) {
        addr = 0;
        memcpy(&addr, root + i, stride);
        const uint8_t *t = acpi_table_at(addr);
        if (t && !strncmp((const char *)t, sig, 4))
            return t;
    }
    return 0;
}
#endif
