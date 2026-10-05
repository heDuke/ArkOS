#ifndef ARK_MODULE_H
#define ARK_MODULE_H
/* .arco loadable kernel driver modules: window setup at kernel PML4[1],
 * manifest-verified boot load and the ARK_SYS_DRIVER request handler. */
#include <stdint.h>
#include "ark_api.h"

void module_init(uint64_t kernel_cr3);
void module_boot_load(void);
int64_t module_request(ArkDriverRequest *request);
const char *module_error(void);
/* ISA line dispatch (IOAPIC or legacy PIC delivery): runs the module ISR
 * bound to irq (0..15) on the module stack and counts it; 1 when a handler
 * ran, 0 when the line has no module owner. */
int module_irq_dispatch(unsigned irq);

/* Host-test view of one slot. The loader's internal state stays private; the
 * harness asserts on decisions (state, mapped bytes, registered devices). */
typedef struct {
    uint32_t used, state, version, devices, has_poll;
    char name[32];
    uint64_t image_bytes;
    uint8_t sha256[32];
    uint32_t irq_lines;      /* bitmask of ISA lines bound by this slot */
    uint64_t irq_count;      /* ISR invocations over those lines */
} ModuleSlotView;

/* Copies the slot decision record out; returns 0 on success. */
int module_slot_view(unsigned slot, ModuleSlotView *out);

#ifdef ARK_MODULE_HOST_TEST
/* One recorded mapping decision, mirroring the PTE flags the kernel would set
 * for the same window range. */
typedef struct {
    uint64_t va, bytes;
    int writable, nx;
} ModuleMap;
/* Host harness: no real paging and no real window VA, so the caller supplies
 * the callable entry and the harness observes mapping decisions instead. */
void module_test_reset(void);
int module_test_load(const void *file, size_t bytes);
void module_test_set_entry(int64_t (*entry)(const ArkDriverHost *, uint32_t));
unsigned module_test_maps(ModuleMap *out);
void module_test_reset_maps(void);
void module_test_set_loading(int slot);
/* Stateful platform route stub: live routed lines, plus cumulative route /
 * release events; route_fail forces platform_irq_route to return rc. */
unsigned module_test_routed(unsigned *routes, unsigned *releases);
void module_test_route_fail(int rc);
#endif

#endif
