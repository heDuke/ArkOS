/* Host unit test for MADT IOAPIC / ISA-override parsing (kernel/ioapic.c). */
#define ARK_IOAPIC_HOST_TEST 1
#include "ioapic.h"
#include "../kernel/ioapic.c"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); return 1; } } while (0)

static void put_u32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}
static void put_u16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

/* Build a MADT: header + Local APIC + IOAPIC + overrides. */
static unsigned build_madt(uint8_t *buf, size_t cap, int with_ioapic, int with_irq0_override) {
    memset(buf, 0, cap);
    memcpy(buf, "APIC", 4);
    buf[8] = 1; /* revision */
    /* Local APIC entry: type 0, size 8, id 0, flags 1 */
    unsigned at = 44;
    buf[at] = 0;
    buf[at + 1] = 8;
    buf[at + 2] = 0;
    buf[at + 3] = 0;
    put_u32(buf + at + 4, 1);
    at += 8;
    if (with_ioapic) {
        buf[at] = 1;
        buf[at + 1] = 12;
        buf[at + 2] = 0; /* id */
        buf[at + 3] = 0;
        put_u32(buf + at + 4, 0xfec00000u);
        put_u32(buf + at + 8, 0);
        at += 12;
    }
    if (with_irq0_override) {
        buf[at] = 2;
        buf[at + 1] = 10;
        buf[at + 2] = 0; /* bus ISA */
        buf[at + 3] = 0; /* source irq 0 */
        put_u32(buf + at + 4, 2); /* gsi 2 */
        put_u16(buf + at + 8, 0); /* edge/high */
        at += 10;
        /* irq 11 level/high SCI-style */
        buf[at] = 2;
        buf[at + 1] = 10;
        buf[at + 2] = 0;
        buf[at + 3] = 11;
        put_u32(buf + at + 4, 11);
        put_u16(buf + at + 8, 0xd);
        at += 10;
    }
    put_u32(buf + 4, at);
    /* checksum so host parsers that re-check do not reject (ioapic_parse_madt
     * itself only looks at the length field). */
    uint8_t sum = 0;
    for (unsigned i = 0; i < at; i++)
        sum += buf[i];
    buf[9] = (uint8_t)(256 - sum);
    return at;
}

int main(void) {
    uint8_t madt[256];
    IoApicTopology t;
    unsigned n;

    printf("ioapic_parse: empty\n");
    CHECK(ioapic_parse_madt(0, 0, &t) == 0);
    CHECK(ioapic_parse_madt(madt, 20, &t) == 0);

    printf("ioapic_parse: no IOAPIC\n");
    n = build_madt(madt, sizeof madt, 0, 0);
    CHECK(ioapic_parse_madt(madt, n, &t) == 0);
    CHECK(t.count == 0);

    printf("ioapic_parse: QEMU-like (IOAPIC + irq0->gsi2 + irq11)\n");
    n = build_madt(madt, sizeof madt, 1, 1);
    CHECK(ioapic_parse_madt(madt, n, &t) == 1);
    CHECK(t.count == 1);
    CHECK(t.ioapic[0].address == 0xfec00000u);
    CHECK(t.ioapic[0].gsi_base == 0);
    CHECK(t.overrides == 2);
    CHECK(t.isa_gsi[0] == 2);
    CHECK(t.isa_override[0] == 1);
    CHECK(t.isa_gsi[2] == IOAPIC_GSI_NONE); /* identity 2 dropped: owned by irq0 */
    CHECK(t.isa_gsi[11] == 11);
    CHECK(t.isa_flags[11] == 0xd);
    CHECK(t.isa_gsi[1] == 1); /* default identity */

    printf("ioapic_rte_mode\n");
    CHECK(ioapic_rte_mode(0) == 0); /* edge/high */
    CHECK(ioapic_rte_mode(0xd) == ((1u << 15))); /* level, polarity conforming (= high) */
    CHECK(ioapic_rte_mode(3) == (1u << 13)); /* edge/low */
    CHECK(ioapic_rte_mode(0xf) == ((1u << 13) | (1u << 15))); /* level/low */

    printf("ioapic_parse: duplicate IOAPIC address rejected\n");
    n = build_madt(madt, sizeof madt, 1, 0);
    /* Append a second identical IOAPIC after the body. */
    unsigned at = n;
    madt[at] = 1;
    madt[at + 1] = 12;
    madt[at + 2] = 1;
    put_u32(madt + at + 4, 0xfec00000u);
    put_u32(madt + at + 8, 24);
    at += 12;
    put_u32(madt + 4, at);
    uint8_t sum = 0;
    for (unsigned i = 0; i < at; i++)
        sum += madt[i];
    madt[9] = (uint8_t)(256 - sum);
    CHECK(ioapic_parse_madt(madt, at, &t) == 1);
    CHECK(t.rejected >= 1);

    printf("PASS\n");
    return 0;
}
