#ifndef ARK_IOAPIC_H
#define ARK_IOAPIC_H
/* ACPI MADT IOAPIC discovery and ISA IRQ routing.
 *
 * When the MADT lists at least one usable IOAPIC the kernel routes ISA lines
 * 0..15 through it (honoring Interrupt Source Overrides), keeps both 8259s
 * fully masked and completes every IOAPIC-delivered IRQ with a LAPIC EOI.
 * IDT vectors stay 32 + line, the same as the legacy PIC remap. Without an
 * IOAPIC (or without a usable BSP LAPIC) platform.c keeps the 8259 path.
 * VT-d interrupt remapping is not used: RTEs are compatibility format with a
 * physical 8-bit destination (the BSP). MSI/MSI-X and ACPI _PRT are TODO. */
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define IOAPIC_MAX 8u
#define IOAPIC_ISA_IRQS 16u
#define IOAPIC_GSI_NONE 0xffffffffu
/* MPS INTI flags (MADT type 2): polarity bits 0..1, trigger bits 2..3. */
#define IOAPIC_FLAG_POLARITY_LOW 3u
#define IOAPIC_FLAG_TRIGGER_LEVEL (3u << 2)

typedef struct {
    uint8_t id;
    uint32_t address, gsi_base;
} IoApicDesc;

typedef struct {
    unsigned count;          /* IOAPIC entries accepted (<= IOAPIC_MAX) */
    unsigned overrides;      /* ISA overrides accepted */
    unsigned rejected;       /* malformed / out-of-range entries skipped */
    uint32_t madt_flags;     /* bit 0 = PCAT_COMPAT (dual 8259 present) */
    IoApicDesc ioapic[IOAPIC_MAX];
    /* Resolved ISA mapping: GSI per ISA line (IOAPIC_GSI_NONE = unroutable)
     * and the MPS flags that apply (0 = ISA default: edge, active high). */
    uint32_t isa_gsi[IOAPIC_ISA_IRQS];
    uint16_t isa_flags[IOAPIC_ISA_IRQS];
    uint8_t isa_override[IOAPIC_ISA_IRQS];
} IoApicTopology;

/* Pure parser over a checksum-validated MADT ("APIC"). Returns the number of
 * IOAPICs found (0 = none / malformed table). Never reads past length. */
unsigned ioapic_parse_madt(const uint8_t *madt, uint32_t length, IoApicTopology *out);
/* RTE low-dword trigger/polarity bits for MPS flags (ISA bus defaults). */
uint32_t ioapic_rte_mode(uint16_t mps_flags);

#ifndef ARK_IOAPIC_HOST_TEST
/* Boot: parse MADT, enable the BSP LAPIC, map + mask every IOAPIC pin.
 * true = IOAPIC mode active; the caller must keep the 8259s fully masked. */
bool ioapic_init(void);
/* Allocate the RTE for ISA line irq (vector 32 + irq, unmasked).
 * 0, -16 already routed / GSI owned, -19 no IOAPIC pin, -22 bad line. */
int ioapic_route(unsigned irq);
/* Mask and clear the RTE for irq; a no-op when it was not routed. */
void ioapic_release(unsigned irq);
void ioapic_mask(unsigned irq);
void ioapic_unmask(unsigned irq);
void ioapic_eoi(void);
/* Serial dump of every unmasked RTE plus leftover / duplicate checks.
 * Returns the number of active RTEs. */
unsigned ioapic_dump(const char *reason);
/* "ioapic0 gsi11 pin11 vec43 level/high" for a routed line, else "". */
void ioapic_describe(unsigned irq, char *out, size_t cap);
#endif
#endif
