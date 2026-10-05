/* .arco loadable kernel modules.
 *
 * A module is one contiguous position-independent image behind an ARCO1
 * header (include/ark_driver.h). Installed bytes live in the @drv.* blob
 * namespace of the ArkFS system volume; the protected manifest @drv.manifest
 * stores the SHA-256 of every file and is the only trust reference — a
 * changed byte anywhere rejects the whole module.
 *
 * Execution isolation: module images are mapped in a dedicated kernel-only
 * window at PML4 index 1 (VA 0x80_0000_0000). The [0, data_off) prefix is
 * read+execute; data and BSS are read+write+NX; each module calls its entry
 * and poll callback on a private 32 KiB stack below a guard page. The
 * identity map still aliases the same physical pages — the window split
 * enforces W^X on the module's own view and keeps accidental writes out of
 * its code pages; it is not a boundary against kernel-mode attacks.
 *
 * Module removal is logical: the slot stops running, its device nodes go
 * ABSENT and its manifest entry is dropped, but mapped pages stay reserved
 * for the rest of the boot (device.c has no unregister). Eight slots and an
 * 8 MiB image budget bound the total footprint. */
#include "ark.h"
#include "ark_driver.h"
#ifndef ARK_KERNEL
#define ARK_KERNEL
#endif
#include "ark_api.h"
#include "device.h"
#include "pci.h"
#include "sha256.h"
#include "process.h"
#include "accounts.h"
#include "extfs.h"
#include "module.h"
#include "alloc.h"
#include "blob.h"
#include "net.h"
#include "block.h"
extern bool vfs_path_canonical(char out[128], const char *path);

#define MODULE_PML4_INDEX 1u
#define MODULE_VMA_BASE ((uint64_t)MODULE_PML4_INDEX << 39)
#define MODULE_SLOT_STRIDE (2u * 1024u * 1024u)
#define MODULE_MAX_DEVICES 16u
/* Kernel-reserved blob owner uid, same convention as @swap.* / @pkg.*: the
 * records are invisible to non-kernel LIST and every user op on @drv.* names
 * is rejected in blob_request. uid 1000 is the bootstrap admin; its blobs
 * already carry the protected system namespaces. */
#define MANIFEST_UID 1000u
#define MANIFEST_BLOB "@drv.manifest"
#define MODULE_BLOB_PREFIX "@drv."
#define PAGE_BYTES 4096ull

typedef struct {
    char name[32];
    uint8_t sha256[32];       /* over the whole .arco file, header included */
    uint64_t bytes;
    uint32_t version, api, reserved;
} ManifestEntry;

typedef struct {
    uint8_t used, state, has_poll, embedded;
    char name[ARCO_NAME_MAX];
    uint32_t version;
    uint8_t sha256[32];
    uint64_t image_bytes;     /* image bytes only, header excluded */
    uint64_t code_va;         /* window VA of the image's first page */
    void *entry;              /* arco_entry inside the window */
    uint64_t stack_top;       /* highest usable module-stack address */
    void *image_phys;         /* kernel pool pages, identity alias */
    size_t image_pages, rx_pages;
    uint64_t data_off;
    void *stack_phys;
    void (*poll_fn)(void);
    int devices[MODULE_MAX_DEVICES];
    unsigned device_count;
    char detail[64];           /* why this slot is FAILED; empty when healthy */
} Module;

static Module modules[ARCO_MODULE_MAX];
static bool initialized, boot_loaded;
#ifndef ARK_MODULE_HOST_TEST
static bool window_ok;
#endif
static int loading_slot = -1; /* slot currently inside arco_entry(INIT/DEINIT) */
/* device_register_poll has no unregister: each slot's stub is registered at
 * most once per boot and stays armed; the module's poll_fn pointer inside it
 * is swapped on reload, and the dispatch gate skips non-LOADED slots. */
static bool poll_slot_registered[ARCO_MODULE_MAX];
/* ISA lines (0..15) owned by modules: -1 = free. Lines used by the platform
 * itself (PIT 0, PS/2 1 and 12, cascade 2) read as busy to irq_attach. The
 * platform delivers a bound line through an IOAPIC RTE when the MADT lists
 * one, else through the legacy 8259. */
#define MODULE_IRQ_LINES 16u
#define KERNEL_IRQ_MASK ((1u << 0) | (1u << 1) | (1u << 2) | (1u << 12))
/* Static -1 fill is load-bearing: interrupts run before module_init, and a
 * zeroed owner table would look like slot-0 bindings to the dispatcher. */
static int8_t irq_owner[MODULE_IRQ_LINES] = {
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1};
static void (*irq_fn[MODULE_IRQ_LINES])(void);
/* ISR invocations per bound line since its last irq_attach. Exposed in the
 * driver detail ("irq11 ... n=42") so a poll-only driver cannot pass for an
 * interrupt-driven one. */
static uint64_t irq_count[MODULE_IRQ_LINES];
static char last_error[128];
/* Why an installed module did not activate during this boot. */
static char boot_error[128];

/* ---- checksums ---------------------------------------------------------- */

static uint32_t module_crc32(const void *data, size_t bytes) {
    static uint32_t table[256];
    static bool built;
    if (!built) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (unsigned j = 0; j < 8; j++)
                c = (c & 1) ? (c >> 1) ^ 0xEDB88320u : c >> 1;
            table[i] = c;
        }
        built = true;
    }
    uint32_t c = ~0u;
    const uint8_t *p = data;
    while (bytes--)
        c = (c >> 8) ^ table[(c ^ *p++) & 255];
    return ~c;
}

static bool name_ok(const char *name) {
    if (!name)
        return false;
    for (unsigned i = 0; i < ARCO_NAME_MAX; i++) {
        char c = name[i];
        if (!c)
            return i > 0;
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' ||
              c == '-' || c == '.'))
            return false;
    }
    return false; /* unterminated inside the field */
}

static void fail(const char *text) {
    strcopy(last_error, text ? text : "module operation failed", sizeof last_error);
}
const char *module_error(void) {
    return last_error;
}

/* ---- mapping layer ------------------------------------------------------- */

#ifdef ARK_MODULE_HOST_TEST
/* Host harness: no real paging. The seam records the mapping decisions so
 * tests can check the RX / RW+NX split and the stack layout. */
#define MODULE_MAP_MAX 32u
static ModuleMap test_maps[MODULE_MAP_MAX];
static unsigned test_map_count;
unsigned module_test_maps(ModuleMap *out) {
    if (out)
        memcpy(out, test_maps, test_map_count * sizeof(ModuleMap));
    return test_map_count;
}
void module_test_reset_maps(void) {
    test_map_count = 0;
}
static bool mod_map_range(unsigned slot, uint64_t va_off, const void *phys,
                          uint64_t bytes, bool writable, bool nx) {
    (void)phys;
    if (va_off >= MODULE_SLOT_STRIDE || bytes > MODULE_SLOT_STRIDE - va_off ||
        bytes % PAGE_BYTES || test_map_count >= MODULE_MAP_MAX)
        return false;
    ModuleMap record;
    record.va = MODULE_VMA_BASE + slot * (uint64_t)MODULE_SLOT_STRIDE + va_off;
    record.bytes = bytes;
    record.writable = writable;
    record.nx = nx;
    test_maps[test_map_count++] = record;
    return true;
}
/* Host harness links libc; page-granular fake backing for the loader. */
extern void *calloc(size_t n, size_t size);
extern void free(void *p);
static void *mod_alloc_pages(size_t pages) {
    return calloc(pages, PAGE_BYTES);
}
static void mod_free_pages(void *p, size_t pages) {
    (void)pages;
    free(p);
}
int64_t module_stack_call(uint64_t stack_top, void *fn, uint64_t a1, uint64_t a2) {
    (void)stack_top;
    return ((int64_t (*)(uint64_t, uint64_t))fn)(a1, a2);
}
/* A mapped window VA is not callable in the harness, so the test supplies the
 * entry function it wants the loader to invoke. */
static int64_t (*test_entry)(const ArkDriverHost *, uint32_t);
void module_test_set_entry(int64_t (*entry)(const ArkDriverHost *, uint32_t)) {
    test_entry = entry;
}
#else
/* Kernel build: one PDPT at kernel PML4[1] shared by every address space
 * (process roots copy all 4096 bytes of the kernel PML4), one PD covering the
 * 1 GiB window and one 4 KiB PT per active 2 MiB slot. */
static uint64_t *mod_pdpt, *mod_pd, *mod_pts[ARCO_MODULE_MAX];
static void invlpg(uint64_t va) {
    __asm__ volatile("invlpg (%0)" ::"r"(va) : "memory");
}
static void *mod_alloc_pages(size_t pages) {
    return process_kernel_alloc(pages * PAGE_BYTES);
}
static void mod_free_pages(void *p, size_t pages) {
    if (p)
        process_kernel_free(p, pages * PAGE_BYTES);
}
void module_window_init(uint64_t kernel_cr3) {
    mod_pdpt = mod_alloc_pages(1);
    mod_pd = mod_alloc_pages(1);
    if (!mod_pdpt || !mod_pd) {
        serial_write("[module] window page-table allocation failed\n");
        return;
    }
    uint64_t *root = (uint64_t *)(uintptr_t)kernel_cr3;
    uint64_t existing = root[MODULE_PML4_INDEX];
    if (existing & 1) {
        /* platform_map_mmio may already have installed a pool PDPT here when a
         * firmware-assigned BAR landed in the [512 GiB, 1 TiB) physical window
         * (UEFI commonly places virtio BARs near 768 GiB). Replacing the entry
         * would orphan those mappings and fault the next device access, so the
         * window must adopt the existing PDPT and share it. */
        uint64_t *shared =
            (uint64_t *)(uintptr_t)(existing & 0x000ffffffffff000ull);
        if (shared[0] & 1) {
            serial_write("[module] window VA collides with an existing mapping\n");
            return;
        }
        mod_free_pages(mod_pdpt, 1);
        mod_pdpt = shared;
    } else {
        root[MODULE_PML4_INDEX] = (uint64_t)(uintptr_t)mod_pdpt | 3;
    }
    mod_pdpt[0] = (uint64_t)(uintptr_t)mod_pd | 3;
    window_ok = true;
}
int64_t module_stack_call(uint64_t stack_top, void *fn, uint64_t a1, uint64_t a2);
static bool mod_map_range(unsigned slot, uint64_t va_off, const void *phys,
                          uint64_t bytes, bool writable, bool nx) {
    if (!window_ok || va_off >= MODULE_SLOT_STRIDE ||
        bytes > MODULE_SLOT_STRIDE - va_off || bytes % PAGE_BYTES)
        return false;
    if (!mod_pts[slot]) {
        mod_pts[slot] = mod_alloc_pages(1);
        if (!mod_pts[slot])
            return false;
        mod_pd[slot] = (uint64_t)(uintptr_t)mod_pts[slot] | 3;
        invlpg(MODULE_VMA_BASE + slot * (uint64_t)MODULE_SLOT_STRIDE);
    }
    uint64_t *pt = mod_pts[slot];
    uint64_t pa = (uint64_t)(uintptr_t)phys;
    for (uint64_t at = 0; at < bytes; at += PAGE_BYTES) {
        unsigned i = (unsigned)((va_off + at) / PAGE_BYTES);
        if (pt[i] & 1)
            return false;
        pt[i] = (pa + at) | 1 | (writable ? 2 : 0) | (nx ? (1ull << 63) : 0);
        invlpg(MODULE_VMA_BASE + slot * (uint64_t)MODULE_SLOT_STRIDE + va_off + at);
    }
    return true;
}
#endif

/* ---- manifest ------------------------------------------------------------ */

/* ARK_BLOB_READ demands capacity == the stored length, so the size is taken
 * from LIST first and the read is issued for exactly that many bytes. */
#define MODULE_BLOB_SCAN 128u
static bool blob_measure(const char *name, uint32_t *size) {
    for (unsigned i = 0; i < MODULE_BLOB_SCAN; i++) {
        ArkBlobRequest q = {0};
        q.op = ARK_BLOB_LIST;
        q.index = i;
        if (blob_kernel_request(&q, MANIFEST_UID) < 0)
            return false;
        if (!strcmp(q.name, name)) {
            *size = q.size;
            return true;
        }
    }
    return false;
}

static bool blob_read_whole(const char *name, void *buffer, uint32_t capacity,
                            uint32_t *size) {
    uint32_t length = 0;
    if (!blob_measure(name, &length) || length > capacity)
        return false;
    ArkBlobRequest q = {0};
    q.op = ARK_BLOB_READ;
    q.capacity = length;
    q.buffer = (uint64_t)(uintptr_t)buffer;
    strcopy(q.name, name, sizeof q.name);
    if (blob_kernel_request(&q, MANIFEST_UID) < 0)
        return false;
    if (size)
        *size = q.size;
    return true;
}

static bool manifest_blob_read(void *buffer, uint32_t capacity, uint32_t *size) {
    return blob_read_whole(MANIFEST_BLOB, buffer, capacity, size);
}

static bool manifest_blob_write(const void *buffer, uint32_t bytes) {
    ArkBlobRequest q = {0};
    q.op = ARK_BLOB_WRITE;
    q.capacity = bytes;
    q.buffer = (uint64_t)(uintptr_t)buffer;
    strcopy(q.name, MANIFEST_BLOB, sizeof q.name);
    return blob_kernel_request(&q, MANIFEST_UID) >= 0;
}

typedef struct {
    uint8_t magic[8];  /* "ARKD1\0\0\0" */
    uint32_t count;
    uint32_t crc32;    /* over the whole record with this field zero */
    uint32_t reserved[2];
    ManifestEntry entry[ARCO_MODULE_MAX];
} Manifest;

#define MANIFEST_HEADER_BYTES 24u

static bool manifest_load(Manifest *m) {
    static uint8_t buffer[1024 + sizeof(Manifest)];
    uint32_t size = 0;
    memset(m, 0, sizeof *m);
    if (!manifest_blob_read(buffer, sizeof buffer, &size))
        return true; /* absent manifest = empty installed set */
    if (size < MANIFEST_HEADER_BYTES || size > sizeof buffer)
        return false;
    uint32_t count;
    memcpy(&count, buffer + 8, 4);
    if (count > ARCO_MODULE_MAX ||
        size != MANIFEST_HEADER_BYTES + count * (uint32_t)sizeof(ManifestEntry))
        return false;
    uint32_t crc;
    memcpy(&crc, buffer + 12, 4);
    memset(buffer + 12, 0, 4);
    if (module_crc32(buffer, size) != crc)
        return false;
    memcpy(m, buffer, size);
    if (memcmp(m->magic, "ARKD1\0\0\0", 8))
        return false;
    return true;
}

static bool manifest_store(const Manifest *m) {
    static uint8_t buffer[1024 + sizeof(Manifest)];
    uint32_t size = MANIFEST_HEADER_BYTES + m->count * (uint32_t)sizeof(ManifestEntry);
    memcpy(buffer, m, size);
    memset(buffer + 12, 0, 4);
    uint32_t crc = module_crc32(buffer, size);
    memcpy(buffer + 12, &crc, 4);
    return manifest_blob_write(buffer, size);
}

static int manifest_find(const Manifest *m, const char *name) {
    for (unsigned i = 0; i < m->count; i++)
        if (!strcmp(m->entry[i].name, name))
            return (int)i;
    return -1;
}

static void module_blob_name(char out[64], const char *name) {
    strcopy(out, MODULE_BLOB_PREFIX, 64);
    strcopy(out + strlen(MODULE_BLOB_PREFIX), name, 64 - strlen(MODULE_BLOB_PREFIX));
}

static bool module_blob_read(const char *name, void *buffer, uint32_t capacity,
                             uint32_t *size) {
    char blob[64];
    module_blob_name(blob, name);
    return blob_read_whole(blob, buffer, capacity, size);
}

static bool module_blob_write(const char *name, const void *buffer,
                              uint32_t bytes) {
    ArkBlobRequest q = {0};
    char blob[64];
    module_blob_name(blob, name);
    q.op = ARK_BLOB_WRITE;
    q.capacity = bytes;
    q.buffer = (uint64_t)(uintptr_t)buffer;
    strcopy(q.name, blob, sizeof q.name);
    return blob_kernel_request(&q, MANIFEST_UID) >= 0;
}

static bool module_blob_remove(const char *name) {
    ArkBlobRequest q = {0};
    char blob[64];
    module_blob_name(blob, name);
    q.op = ARK_BLOB_REMOVE;
    strcopy(q.name, blob, sizeof q.name);
    return blob_kernel_request(&q, MANIFEST_UID) >= 0;
}

/* ---- host table handed to drivers ---------------------------------------- */

static void host_log(const char *text) {
    char line[128];
    strcopy(line, "[drv] ", sizeof line);
    size_t at = strlen(line);
    if (text)
        for (unsigned i = 0; text[i] && at + 2 < sizeof line; i++)
            line[at++] = text[i] < 32 ? '?' : text[i];
    line[at++] = '\n';
    line[at] = 0;
    serial_write(line);
}
static void *host_map_mmio(uint64_t physical, uint64_t bytes) {
    /* MMIO apertures below 4 GiB sit inside the kernel identity map already;
     * the returned pointer is a supervisor-only alias. */
    if (!physical || physical < 0x10000 || !bytes ||
        physical + bytes > 0x100000000ull || bytes > 64u * 1024 * 1024)
        return 0;
    return (void *)(uintptr_t)physical;
}
static int host_pci_device(unsigned index, uint32_t out[10]) {
    const PciDevice *d = pci_device(index);
    if (!d || !out)
        return 0;
    uint32_t packed[10] = {d->bdf,      d->vendor,           d->device,
                           d->class_code, d->subclass,       d->revision,
                           d->header_type, d->subsystem_vendor,
                           d->subsystem_device, d->irq_pin};
    memcpy(out, packed, sizeof packed);
    return 1;
}
static int host_device_register(const void *info) {
    if (loading_slot < 0 || !info)
        return -22;
    int index = device_register((const ArkDeviceInfo *)info);
    Module *m = &modules[loading_slot];
    if (index >= 0 && m->device_count < MODULE_MAX_DEVICES)
        m->devices[m->device_count++] = index;
    return index;
}
static int host_device_set_state(uint32_t index, uint32_t flags, uint32_t state) {
    return device_set_state(index, flags, state) ? 0 : -22;
}
static int host_device_counters(uint32_t index, uint64_t rx, uint64_t tx,
                                uint64_t ops, uint64_t errors) {
    return device_add_counters(index, rx, tx, ops, errors) ? 0 : -22;
}
static int host_device_notify(uint32_t index) {
    return device_notify(index) ? 0 : -22;
}
static void module_poll_dispatch(unsigned slot);
static void poll_stub_0(void) { module_poll_dispatch(0); }
static void poll_stub_1(void) { module_poll_dispatch(1); }
static void poll_stub_2(void) { module_poll_dispatch(2); }
static void poll_stub_3(void) { module_poll_dispatch(3); }
static void poll_stub_4(void) { module_poll_dispatch(4); }
static void poll_stub_5(void) { module_poll_dispatch(5); }
static void poll_stub_6(void) { module_poll_dispatch(6); }
static void poll_stub_7(void) { module_poll_dispatch(7); }
static void (*const poll_stubs[ARCO_MODULE_MAX])(void) = {
    poll_stub_0, poll_stub_1, poll_stub_2, poll_stub_3,
    poll_stub_4, poll_stub_5, poll_stub_6, poll_stub_7};
static int host_pci_find(uint16_t vendor, uint16_t device, uint32_t *bdf) {
    if (!bdf)
        return -22;
    return pci_find(vendor, device, bdf) ? 0 : -2;
}
static int host_pci_bar(uint32_t bdf, unsigned bar, uint64_t *base) {
    if (!base)
        return -22;
    return pci_bar_base(bdf, bar, base) ? 0 : -2;
}
static int host_register_poll(void (*poll)(void)) {
    if (loading_slot < 0 || !poll)
        return -22;
    Module *m = &modules[loading_slot];
    if (m->has_poll)
        return -16; /* one poll callback per module */
    if (!poll_slot_registered[loading_slot]) {
        if (!device_register_poll(poll_stubs[loading_slot]))
            return -16; /* global poll budget exhausted */
        poll_slot_registered[loading_slot] = true;
    }
    m->poll_fn = poll;
    m->has_poll = 1;
    return 0;
}
/* Platform hooks (kernel/platform.c; stateful stubs in the host test).
 * route allocates delivery for one ISA line (IOAPIC RTE or PIC IMR bit) and
 * fails visibly; release is its exact inverse. */
int platform_irq_route(unsigned irq);
void platform_irq_release(unsigned irq);
void platform_irq_describe(unsigned irq, char *out, size_t cap);
/* Drop every line bound to slot: release its RTE / mask the PIC line first
 * so no ISR can run while the module is being torn down. */
static void module_irq_release(unsigned slot) {
    for (unsigned i = 0; i < MODULE_IRQ_LINES; i++)
        if (irq_owner[i] == (int)slot) {
            platform_irq_release(i);
            irq_owner[i] = -1;
            irq_fn[i] = 0;
        }
}
static void irq_attach_log(uint32_t irq, int rc, const char *why) {
    char text[160], n[24];
    strcopy(text, "[irq] ", sizeof text);
    size_t at = strlen(text);
    strcopy(text + at, loading_slot >= 0 ? modules[loading_slot].name : "?", sizeof text - at);
    at = strlen(text);
    strcopy(text + at, " irq_attach(", sizeof text - at);
    at = strlen(text);
    uint_to_str(irq, n);
    strcopy(text + at, n, sizeof text - at);
    at = strlen(text);
    if (rc) {
        strcopy(text + at, ") FAILED rc=-", sizeof text - at);
        at = strlen(text);
        uint_to_str((uint64_t)-(int64_t)rc, n);
        strcopy(text + at, n, sizeof text - at);
        at = strlen(text);
        strcopy(text + at, " (", sizeof text - at);
        at = strlen(text);
        strcopy(text + at, why, sizeof text - at);
        at = strlen(text);
        strcopy(text + at, "); driver is poll-only unless it retries\n", sizeof text - at);
    } else {
        char route[64];
        platform_irq_describe(irq, route, sizeof route);
        strcopy(text + at, ") bound -> ", sizeof text - at);
        at = strlen(text);
        strcopy(text + at, route[0] ? route : "unrouted", sizeof text - at);
        at = strlen(text);
        strcopy(text + at, "\n", sizeof text - at);
    }
    serial_write(text);
}
static int host_irq_attach(uint32_t irq, void (*isr)(void)) {
    if (loading_slot < 0 || !isr || irq >= MODULE_IRQ_LINES) {
        if (loading_slot >= 0)
            irq_attach_log(irq, -22, !isr ? "null isr" : "line outside 0..15");
        return -22;
    }
#ifndef ARK_MODULE_HOST_TEST
    /* Same contract as net ops: the ISR must live in this module's own RX
     * code window, never in foreign or host code. */
    Module *m = &modules[loading_slot];
    uint64_t va = (uint64_t)(uintptr_t)isr;
    if (va < m->code_va || va >= m->code_va + m->rx_pages * PAGE_BYTES) {
        irq_attach_log(irq, -22, "isr outside module code");
        return -22;
    }
#endif
    if ((KERNEL_IRQ_MASK & (1u << irq)) || irq_owner[irq] >= 0) {
        irq_attach_log(irq, -16, "line owned by kernel or another module");
        return -16; /* platform-owned or already bound */
    }
    irq_owner[irq] = (int8_t)loading_slot;
    irq_fn[irq] = isr;
    irq_count[irq] = 0;
    int rc = platform_irq_route(irq);
    if (rc < 0) {
        /* No IOAPIC pin / RTE busy: the binding never existed. */
        irq_owner[irq] = -1;
        irq_fn[irq] = 0;
        irq_attach_log(irq, rc, rc == -19 ? "no IOAPIC pin for line" : "RTE busy");
        return rc;
    }
    irq_attach_log(irq, 0, "");
    return 0;
}
/* Called from interrupt_dispatch before the PIC EOI. A stale binding (module
 * mid-teardown) is masked off instead of reaching released code. */
int module_irq_dispatch(unsigned irq) {
    if (irq >= MODULE_IRQ_LINES)
        return 0;
    int slot = irq_owner[irq];
    if (slot < 0)
        return 0;
    Module *m = &modules[slot];
    /* Pin the handler locally: a concurrent module_remove may clear the slot
     * between the gate and the call; the pinned pointer stays mapped until
     * removal finishes tearing down the slot's pages. */
    void (*fn)(void) = irq_fn[irq];
    if (!m->used || m->state != ARK_DRV_STATE_LOADED || !fn) {
        module_irq_release((unsigned)slot);
        return 0;
    }
    irq_count[irq]++;
    module_stack_call(m->stack_top, (void *)fn, 0, 0);
    return 1;
}
/* Module window VAs are backed by identity-pool kernel pages, so a window VA
 * plus the run base yields the DMA physical address. 0 = VA not owned by any
 * module image or stack. */
static uint64_t host_phys_of(const void *va_ptr) {
    uint64_t va = (uint64_t)(uintptr_t)va_ptr;
    for (unsigned s = 0; s < ARCO_MODULE_MAX; s++) {
        Module *m = &modules[s];
        if (!m->used)
            continue;
        if (va >= m->code_va && va < m->code_va + m->image_pages * PAGE_BYTES)
            return (uint64_t)(uintptr_t)m->image_phys + (va - m->code_va);
        uint64_t stack_base = m->stack_top - ARCO_STACK_BYTES;
        if (va >= stack_base && va < m->stack_top)
            return (uint64_t)(uintptr_t)m->stack_phys + (va - stack_base);
    }
    return 0;
}
#ifdef ARK_MODULE_HOST_TEST
/* No PIC/IOAPIC in the harness: a stateful route table stands in for the
 * RTEs so the allocate/release contract is observable (a second route of a
 * live line fails, release frees it, reload reallocates it exactly once). */
static uint8_t test_routed[MODULE_IRQ_LINES];
static unsigned test_route_events, test_release_events;
static int test_route_fail = 0;
int platform_irq_route(unsigned irq) {
    if (irq >= MODULE_IRQ_LINES)
        return -22;
    if (test_route_fail)
        return test_route_fail;
    if (test_routed[irq])
        return -16;
    test_routed[irq] = 1;
    test_route_events++;
    return 0;
}
void platform_irq_release(unsigned irq) {
    if (irq < MODULE_IRQ_LINES && test_routed[irq]) {
        test_routed[irq] = 0;
        test_release_events++;
    }
}
void platform_irq_describe(unsigned irq, char *out, size_t cap) {
    (void)irq;
    strcopy(out, "test route", cap);
}
unsigned module_test_routed(unsigned *routes, unsigned *releases) {
    unsigned live = 0;
    for (unsigned i = 0; i < MODULE_IRQ_LINES; i++)
        live += test_routed[i];
    if (routes)
        *routes = test_route_events;
    if (releases)
        *releases = test_release_events;
    return live;
}
void module_test_route_fail(int rc) {
    test_route_fail = rc;
}
/* No network stack in the harness; a stateful stub keeps the bind contract
 * exercisable (second bind fails until the owner's unbind). */
static bool test_nic_bound;
static unsigned test_nic_owner;
int net_bind_nic(const void *ops, unsigned owner) {
    if (!ops)
        return -22;
    if (test_nic_bound)
        return -16;
    test_nic_bound = true;
    test_nic_owner = owner;
    return 0;
}
void net_unbind_nic(unsigned owner) {
    if (test_nic_bound && test_nic_owner == owner)
        test_nic_bound = false;
}
#endif
static int host_net_attach(const void *ops) {
    if (loading_slot < 0)
        return -22;
    const ArkNetOps *table = (const ArkNetOps *)ops;
    if (!table || !table->send || !table->link || !table->receive ||
        !table->read_mac)
        return -22;
#ifndef ARK_MODULE_HOST_TEST
    /* Ops callbacks must execute inside this module's own RX window; a driver
     * may not hand the network stack pointers into foreign code. */
    Module *m = &modules[loading_slot];
    uint64_t begin = m->code_va, end = m->code_va + m->rx_pages * PAGE_BYTES;
    const void *fns[] = {(const void *)table->send, (const void *)table->link,
                         (const void *)table->receive,
                         (const void *)table->read_mac};
    for (unsigned i = 0; i < sizeof fns / sizeof fns[0]; i++) {
        uint64_t va = (uint64_t)(uintptr_t)fns[i];
        if (va < begin || va >= end)
            return -22;
    }
#endif
    return net_bind_nic(ops, (unsigned)loading_slot);
}
static void host_net_detach(void) {
    if (loading_slot >= 0)
        net_unbind_nic((unsigned)loading_slot);
}

static int host_block_attach(const void *ops) {
    if (loading_slot < 0)
        return -22;
    const ArkBlockOps *table = (const ArkBlockOps *)ops;
    if (!table || !table->sectors || !table->transfer || !table->flush)
        return -22;
#ifndef ARK_MODULE_HOST_TEST
    Module *m = &modules[loading_slot];
    uint64_t begin = m->code_va, end = m->code_va + m->rx_pages * PAGE_BYTES;
    const void *fns[] = {(const void *)table->sectors, (const void *)table->transfer,
                         (const void *)table->flush};
    for (unsigned i = 0; i < sizeof fns / sizeof fns[0]; i++) {
        uint64_t va = (uint64_t)(uintptr_t)fns[i];
        if (va < begin || va >= end)
            return -22;
    }
#endif
    return block_bind_ops(ops, (unsigned)loading_slot);
}
static void host_block_detach(void) {
    if (loading_slot >= 0)
        block_unbind_ops((unsigned)loading_slot);
}

static const ArkDriverHost host_table = {
    .millis = platform_millis,
    .ticks = platform_ticks,
    .log = host_log,
    .alloc = ark_alloc,
    .free = ark_free,
    .map_mmio = host_map_mmio,
    .pci_count = pci_count,
    .pci_device = host_pci_device,
    .pci_read = pci_read,
    .pci_read8 = pci_read8,
    .pci_write16 = pci_write16,
    .pci_command = pci_command,
    .pci_find = host_pci_find,
    .pci_bar_base = host_pci_bar,
    .device_register = host_device_register,
    .device_set_state = host_device_set_state,
    .device_add_counters = host_device_counters,
    .device_notify = host_device_notify,
    .device_register_poll = host_register_poll,
    .phys_of = host_phys_of,
    .net_attach = host_net_attach,
    .net_detach = host_net_detach,
    .irq_attach = host_irq_attach,
    .block_attach = host_block_attach,
    .block_detach = host_block_detach,
};
static void module_poll_dispatch(unsigned slot) {
    if (slot >= ARCO_MODULE_MAX)
        return;
    Module *m = &modules[slot];
    if (!m->used || m->state != ARK_DRV_STATE_LOADED || !m->poll_fn)
        return;
    module_stack_call(m->stack_top, (void *)m->poll_fn, 0, 0);
}

/* ---- image validation and load ------------------------------------------- */

static uint8_t file_digest[32];

#ifdef ARK_MODULE_HOST_TEST
static void window_clear(unsigned slot, uint64_t va_off, uint64_t bytes) {
    (void)slot;
    (void)va_off;
    (void)bytes;
}
#else
/* Unmap one slot range and hand the pages back to the kernel pool. */
static void window_clear(unsigned slot, uint64_t va_off, uint64_t bytes) {
    uint64_t *pt = mod_pts[slot];
    if (!pt)
        return;
    for (uint64_t at = 0; at < bytes; at += PAGE_BYTES) {
        unsigned i = (unsigned)((va_off + at) / PAGE_BYTES);
        if (!(pt[i] & 1))
            continue;
        pt[i] = 0;
        invlpg(MODULE_VMA_BASE + slot * (uint64_t)MODULE_SLOT_STRIDE + va_off + at);
    }
}
#endif

/* Free a slot's mapping and backing pages so the name can be installed again.
 * Called only for DISABLED or FAILED slots: a LOADED module keeps running. */
static void module_release(unsigned slot) {
    Module *m = &modules[slot];
    if (!m->used)
        return;
    window_clear(slot, 0, m->rx_pages * PAGE_BYTES);
    window_clear(slot, m->data_off, (m->image_pages - m->rx_pages) * PAGE_BYTES);
    window_clear(slot, MODULE_SLOT_STRIDE - ARCO_STACK_BYTES, ARCO_STACK_BYTES);
    mod_free_pages(m->stack_phys, ARCO_STACK_BYTES / PAGE_BYTES);
    mod_free_pages(m->image_phys, m->image_pages);
    memset(m, 0, sizeof *m);
}

static int64_t module_call(Module *m, void *fn, uint32_t op) {
    return module_stack_call(m->stack_top, fn, (uint64_t)(uintptr_t)&host_table,
                             op);
}

static int find_slot(void) {
    for (unsigned i = 0; i < ARCO_MODULE_MAX; i++)
        if (!modules[i].used)
            return (int)i;
    return -1;
}
static int slot_by_name(const char *name) {
    for (unsigned i = 0; i < ARCO_MODULE_MAX; i++)
        if (modules[i].used && !strcmp(modules[i].name, name))
            return (int)i;
    return -1;
}

/* file = complete .arco bytes (header + image). Returns slot index or <0. */
static int module_load(const uint8_t *file, size_t bytes) {
    if (!initialized || bytes <= ARCO_HEADER_BYTES ||
        bytes > ARCO_HEADER_BYTES + ARCO_IMAGE_MAX)
        return fail("module image size out of range"), -22;
    ArcoHeader h;
    memcpy(&h, file, sizeof h);
    if (memcmp(h.magic, "ARCO1\0\0\0", 8) || h.api != ARK_DRIVER_API ||
        !name_ok(h.name))
        return fail("not an ARCO1 module for this API"), -22;
    uint64_t image = bytes - ARCO_HEADER_BYTES;
    if (h.image_size != image || image > ARCO_IMAGE_MAX || !image ||
        h.entry_off >= image || h.bss_size > ARCO_BSS_MAX ||
        h.bss_size % PAGE_BYTES)
        return fail("module header fields inconsistent"), -22;
    uint64_t data_off = h.data_off ? h.data_off : image;
    if (data_off > image || data_off % PAGE_BYTES || data_off < PAGE_BYTES)
        return fail("module data boundary misaligned"), -22;
    if (h.entry_off >= data_off)
        return fail("module entry point outside code region"), -22;
    for (unsigned i = 0; i < sizeof h.reserved; i++)
        if (h.reserved[i])
            return fail("module header reserved bytes nonzero"), -22;
    uint32_t hcrc = h.header_crc32;
    ArcoHeader hc = h;
    hc.header_crc32 = 0;
    if (module_crc32(&hc, sizeof hc) != hcrc ||
        module_crc32(file + ARCO_HEADER_BYTES, image) != h.image_crc32)
        return fail("module CRC mismatch"), -22;
    uint8_t digest[32];
    ark_sha256(file + ARCO_HEADER_BYTES, image, digest);
    if (memcmp(digest, h.image_sha256, 32))
        return fail("module image hash mismatch"), -22;
    ark_sha256(file, bytes, file_digest);
    int slot = slot_by_name(h.name);
    if (slot >= 0) {
        /* reinstalling over a removed or failed module reuses its slot */
        if (modules[slot].state == ARK_DRV_STATE_LOADED)
            return fail("module name already loaded"), -17;
        module_release((unsigned)slot);
    }
    if ((slot = find_slot()) < 0)
        return fail("module slots exhausted"), -28;

    /* One contiguous physical run holds [0,image+bss): the RX prefix covers
     * exactly data_off bytes (page-aligned by construction) and everything
     * after it — .data, file tail padding and zeroed .bss — is RW+NX. */
    size_t total_pages = (size_t)((image + h.bss_size + PAGE_BYTES - 1) / PAGE_BYTES);
    size_t rx_pages = (size_t)(data_off / PAGE_BYTES);
    if ((uint64_t)total_pages * PAGE_BYTES > ARCO_POOL_BYTES)
        return fail("module pool exhausted"), -28;
    void *phys = mod_alloc_pages(total_pages);
    if (!phys)
        return fail("kernel pool exhausted"), -12;
    memcpy(phys, file + ARCO_HEADER_BYTES, image);
    /* BSS and tail padding are already zeroed by the pool allocator. */
    void *stack = mod_alloc_pages(ARCO_STACK_BYTES / PAGE_BYTES);
    if (!stack) {
        mod_free_pages(phys, total_pages);
        return fail("kernel pool exhausted"), -12;
    }
    uint64_t code_va = MODULE_VMA_BASE + (uint64_t)slot * MODULE_SLOT_STRIDE;
    uint64_t stack_off = MODULE_SLOT_STRIDE - ARCO_STACK_BYTES;
    if (!mod_map_range(slot, 0, phys, rx_pages * PAGE_BYTES, false, false) ||
        (total_pages > rx_pages &&
         !mod_map_range(slot, data_off, (uint8_t *)phys + data_off,
                        (total_pages - rx_pages) * PAGE_BYTES, true, true)) ||
        !mod_map_range(slot, stack_off, stack, ARCO_STACK_BYTES, true, true)) {
        mod_free_pages(stack, ARCO_STACK_BYTES / PAGE_BYTES);
        mod_free_pages(phys, total_pages);
        return fail("module window mapping failed"), -12;
    }

    Module *m = &modules[slot];
    memset(m, 0, sizeof *m);
    m->used = 1;
    m->state = ARK_DRV_STATE_LOADED;
    m->version = h.version;
    m->image_bytes = image;
    memcpy(m->sha256, file_digest, 32);
    strcopy(m->name, h.name, sizeof m->name);
    m->code_va = code_va;
    m->stack_top = code_va + stack_off + ARCO_STACK_BYTES;
    m->image_phys = phys;
    m->image_pages = total_pages;
    m->rx_pages = rx_pages;
    m->data_off = data_off;
    m->stack_phys = stack;
    for (unsigned i = 0; i < MODULE_MAX_DEVICES; i++)
        m->devices[i] = -1;

#ifdef ARK_MODULE_HOST_TEST
    if (!test_entry)
        return fail("no host entry installed"), -5;
    m->entry = (void *)(uintptr_t)test_entry;
#else
    m->entry = (void *)(uintptr_t)(code_va + h.entry_off);
#endif
    loading_slot = slot;
    int64_t rc = module_call(m, m->entry, ARCO_OP_INIT);
    loading_slot = -1;
    if (rc < 0) {
        /* INIT may have bound IRQ lines before failing; release them. */
        module_irq_release((unsigned)slot);
        m->state = ARK_DRV_STATE_FAILED;
        strcopy(m->detail, "module init rejected the load", sizeof m->detail);
        fail("module init rejected the load");
        return slot; /* slot stays visible as FAILED */
    }
    return slot;
}

/* ---- init, boot load, install/remove ------------------------------------- */

void module_init(uint64_t kernel_cr3) {
    memset(irq_owner, 0xff, sizeof irq_owner);
    memset(irq_fn, 0, sizeof irq_fn);
#ifdef ARK_MODULE_HOST_TEST
    (void)kernel_cr3;
#else
    module_window_init(kernel_cr3);
    if (!window_ok)
        return;
#endif
    initialized = true;
}

/* Verify file sha256 against the manifest entry, parse, map, INIT. */
static int module_activate(const ManifestEntry *e, const uint8_t *file,
                           size_t bytes) {
    uint8_t digest[32];
    ark_sha256(file, bytes, digest);
    if (memcmp(digest, e->sha256, 32))
        return fail("module bytes differ from manifest"), -22;
    return module_load(file, bytes);
}

/* Inbox drivers: .arco images linked into kernel.elf by kernel/drivers_embed.S.
 * They are kernel build artifacts — the same trust level as kernel text — so
 * they skip the manifest hash but still pass every structural check inside
 * module_load. Manifest-installed drivers take precedence: the manifest pass
 * runs first and a claimed name (any state) is never overridden here, so a
 * disk update shadows the inbox copy. Embedded drivers keep no manifest
 * entry; removing one lasts until the next boot. */
#ifndef ARK_MODULE_HOST_TEST
extern const uint8_t _arkos_driver_e1000_start[], _arkos_driver_e1000_end[];
static const struct {
    const uint8_t *start, *end;
} embedded_drivers[] = {
    {_arkos_driver_e1000_start, _arkos_driver_e1000_end},
};
#endif
static void module_load_embedded(void) {
#ifndef ARK_MODULE_HOST_TEST
    for (unsigned i = 0; i < sizeof embedded_drivers / sizeof embedded_drivers[0];
         i++) {
        const uint8_t *file = embedded_drivers[i].start;
        size_t bytes = (size_t)(embedded_drivers[i].end - embedded_drivers[i].start);
        if (bytes <= ARCO_HEADER_BYTES)
            continue;
        ArcoHeader h;
        memcpy(&h, file, sizeof h);
        if (!name_ok(h.name))
            continue;
        /* Any claimed slot wins — including a FAILED manifest driver, which
         * must stay visible instead of being silently shadowed. `dev remove`
         * frees the name and the inbox copy returns on the next boot. */
        if (slot_by_name(h.name) >= 0)
            continue;
        int slot = module_load(file, bytes);
        if (slot < 0 || modules[slot].state != ARK_DRV_STATE_LOADED) {
            strcopy(boot_error, last_error, sizeof boot_error);
            serial_write("[module] inbox load failed: ");
            serial_write(h.name);
            serial_write(" -> ");
            serial_write(last_error);
            serial_write("\n");
            continue;
        }
        modules[slot].embedded = 1;
        serial_write("[module] loaded ");
        serial_write(h.name);
        serial_write(" (inbox)\n");
    }
#endif
}
void module_boot_load(void) {
    if (!initialized || boot_loaded)
        return;
    boot_loaded = true;
    Manifest m;
    if (manifest_load(&m)) {
        static uint8_t file[ARCO_HEADER_BYTES + ARCO_IMAGE_MAX];
        for (unsigned i = 0; i < m.count; i++) {
            uint32_t size = 0;
            if (!module_blob_read(m.entry[i].name, file, sizeof file, &size) ||
                size != m.entry[i].bytes) {
                strcopy(boot_error, "stored image is unreadable",
                        sizeof boot_error);
                serial_write("[module] blob unreadable: ");
                serial_write(m.entry[i].name);
                serial_write("\n");
                continue;
            }
            int slot = module_activate(&m.entry[i], file, size);
            if (slot < 0 || modules[slot].state != ARK_DRV_STATE_LOADED) {
                strcopy(boot_error, last_error, sizeof boot_error);
                serial_write("[module] load failed: ");
                serial_write(m.entry[i].name);
                serial_write(" -> ");
                serial_write(last_error);
                serial_write("\n");
            } else {
                serial_write("[module] loaded ");
                serial_write(m.entry[i].name);
                serial_write("\n");
            }
        }
    } else {
        serial_write("[module] no driver manifest; inbox drivers only\n");
    }
    module_load_embedded();
}

/* INSTALL: image bytes come from blob:NAME (user blob, kernel-uid namespace
 * is also reachable) or a canonical local path. Manifest first, then load;
 * a failed activation rolls the install back completely. */
static int64_t module_install(ArkDriverRequest *q) {
    static uint8_t file[ARCO_HEADER_BYTES + ARCO_IMAGE_MAX];
    uint32_t size = 0;
    if (!strncmp(q->path, "blob:", 5)) {
        const char *name = q->path + 5;
        if (!name[0] || strlen(name) >= sizeof q->name)
            return -22;
        /* ARK_BLOB_READ requires capacity == stored length (see blob_measure). */
        uint32_t uid = accounts_current_uid(), length = 0;
        bool measured = false;
        for (unsigned i = 0; i < MODULE_BLOB_SCAN; i++) {
            ArkBlobRequest qlist = {0};
            qlist.op = ARK_BLOB_LIST;
            qlist.index = i;
            if (blob_kernel_request(&qlist, uid) < 0)
                break;
            if (!strcmp(qlist.name, name)) {
                length = qlist.size;
                measured = true;
                break;
            }
        }
        if (!measured || !length || length > sizeof file)
            return strcopy(q->error, "source blob unreadable", sizeof q->error), -2;
        ArkBlobRequest b = {0};
        b.op = ARK_BLOB_READ;
        b.capacity = length;
        b.buffer = (uint64_t)(uintptr_t)file;
        strcopy(b.name, name, sizeof b.name);
        if (blob_kernel_request(&b, uid) < 0)
            return strcopy(q->error, "source blob unreadable", sizeof q->error), -2;
        size = b.size;
    } else {
        char canon[128];
        if (!vfs_path_canonical(canon, q->path) ||
            !accounts_path_allowed(accounts_current_uid(), canon, false))
            return strcopy(q->error, "path is outside your own files", sizeof q->error), -22;
        int index = vfs_find(canon);
        if (index < 0)
            return strcopy(q->error, "no such driver file", sizeof q->error), -2;
        VFile *f = vfs_entry(index);
        if (!f || !f->size)
            return strcopy(q->error, "driver file is empty", sizeof q->error), -22;
        if (f->size > sizeof file)
            return strcopy(q->error, "driver image is over the 1 MiB limit",
                           sizeof q->error), -22;
        memcpy(file, f->data, f->size);
        size = f->size;
    }
    if (size <= ARCO_HEADER_BYTES)
        return strcopy(q->error, "file is too small to be an ARCO1 module",
                       sizeof q->error), -22;
    /* Peek at the header for the manifest name/hash before touching state. */
    const ArcoHeader *h = (const ArcoHeader *)file;
    if (memcmp(h->magic, "ARCO1\0\0\0", 8) || !name_ok(h->name))
        return strcopy(q->error, "not an ARCO1 module", sizeof q->error), -22;
    Manifest m;
    if (!manifest_load(&m))
        return strcopy(q->error, "driver manifest corrupt", sizeof q->error), -5;
    memcpy(m.magic, "ARKD1\0\0\0", 8);
    if (m.count >= ARCO_MODULE_MAX)
        return strcopy(q->error, "driver slots exhausted", sizeof q->error), -16;
    int at = manifest_find(&m, h->name);
    ManifestEntry e;
    memset(&e, 0, sizeof e);
    strcopy(e.name, h->name, sizeof e.name);
    ark_sha256(file, size, e.sha256);
    e.bytes = size;
    e.version = h->version;
    e.api = h->api;
    if (at >= 0)
        m.entry[at] = e;
    else
        m.entry[m.count++] = e;
    int slot = module_activate(&e, file, size);
    if (slot < 0 || modules[slot].state != ARK_DRV_STATE_LOADED) {
        strcopy(q->error, last_error, sizeof q->error);
        return slot >= 0 ? -5 : -22;
    }
    /* Persist only after the module accepted itself. */
    if (!module_blob_write(e.name, file, size) || !manifest_store(&m)) {
        modules[slot].state = ARK_DRV_STATE_FAILED;
        return strcopy(q->error, "driver store write failed", sizeof q->error), -5;
    }
    q->info.index = (uint32_t)slot;
    q->info.state = ARK_DRV_STATE_LOADED;
    q->info.version = e.version;
    q->info.image_bytes = e.bytes;
    strcopy(q->info.name, e.name, sizeof q->info.name);
    memcpy(q->info.sha256, e.sha256, 32);
    q->count = m.count;
    return 0;
}

static int64_t module_remove(ArkDriverRequest *q) {
    if (!name_ok(q->name))
        return -22;
    int slot = slot_by_name(q->name);
    if (slot < 0)
        return -2;
    Module *m = &modules[slot];
    m->state = ARK_DRV_STATE_DISABLED;
    /* Mask the module's PIC lines before DEINIT runs: no ISR may execute on
     * the module stack while the driver is tearing its device down. */
    module_irq_release((unsigned)slot);
    /* Best-effort teardown on the module stack; a driver that refuses still
     * loses its device visibility below. Mapped pages stay reserved. */
    loading_slot = slot;
    module_call(m, m->entry, ARCO_OP_DEINIT);
    loading_slot = -1;
    /* A NIC driver should detach in DEINIT; force it so the network stack
     * can never call ops of a removed module. */
    net_unbind_nic((unsigned)slot);
    block_unbind_ops((unsigned)slot);
    for (unsigned i = 0; i < m->device_count; i++)
        if (m->devices[i] >= 0)
            device_set_state((uint32_t)m->devices[i], 0, ARK_DEV_STATE_ABSENT);
    Manifest mf;
    if (manifest_load(&mf)) {
        int at = manifest_find(&mf, q->name);
        if (at >= 0) {
            mf.entry[at] = mf.entry[mf.count - 1];
            mf.count--;
            manifest_store(&mf);
        }
    }
    module_blob_remove(q->name);
    q->count = mf.count ? mf.count : 0;
    strcopy(q->info.name, q->name, sizeof q->info.name);
    q->info.state = ARK_DRV_STATE_DISABLED;
    module_release((unsigned)slot);
    return 0;
}

/* ---- syscall face --------------------------------------------------------- */

static bool module_active(void) {
    return accounts_state() == ACCOUNT_ACTIVE;
}
static bool module_may_change(void) {
    return process_has_cap(ARK_CAP_SYSTEM) && module_active() &&
           accounts_current_admin();
}

static void module_detail(const Module *m, char out[96]) {
    strcopy(out, "devices:", 96);
    size_t at = strlen(out);
    for (unsigned i = 0; i < m->device_count && at + 6 < 96; i++) {
        char num[8];
        uint_to_str((unsigned)m->devices[i], num);
        strcopy(out + at, num, 96 - at);
        at = strlen(out);
        if (i + 1 < m->device_count && at + 2 < 96)
            out[at++] = ',';
    }
    out[at] = 0;
    /* Interrupt evidence: every bound line with its route and ISR count, or
     * an explicit poll-only marker. Smoke gates read this, not the driver's
     * own claims. */
    int slot = (int)(m - modules);
    bool any = false;
    for (unsigned i = 0; i < MODULE_IRQ_LINES; i++) {
        if (irq_owner[i] != slot)
            continue;
        char num[24], route[64];
        platform_irq_describe(i, route, sizeof route);
        strcopy(out + at, " irq", 96 - at);
        at = strlen(out);
        uint_to_str(i, num);
        strcopy(out + at, num, 96 - at);
        at = strlen(out);
        strcopy(out + at, " ", 96 - at);
        at = strlen(out);
        /* "ioapic0 gsi11 vec43 level/high" is long; keep route kind + gsi. */
        if (!strncmp(route, "ioapic", 6))
            for (char *c = route; *c; c++)
                if (!strncmp(c, " vec", 4)) {
                    *c = 0;
                    break;
                }
        strcopy(out + at, route[0] ? route : "unrouted", 96 - at);
        at = strlen(out);
        strcopy(out + at, " n=", 96 - at);
        at = strlen(out);
        uint_to_str(irq_count[i], num);
        strcopy(out + at, num, 96 - at);
        at = strlen(out);
        any = true;
    }
    if (!any && m->used && m->state == ARK_DRV_STATE_LOADED)
        strcopy(out + at, m->has_poll ? " irq:none (poll-only)" : " irq:none", 96 - at);
}
/* Embedded (inbox) modules carry no manifest entry; LIST appends them after
 * manifest entries so dev drivers shows the NIC driver too. */
static unsigned embedded_count(const Manifest *mf, bool have) {
    unsigned n = 0;
    for (unsigned s = 0; s < ARCO_MODULE_MAX; s++)
        if (modules[s].used && modules[s].embedded &&
            (!have || manifest_find(mf, modules[s].name) < 0))
            n++;
    return n;
}
static int embedded_at(const Manifest *mf, bool have, unsigned index) {
    for (unsigned s = 0; s < ARCO_MODULE_MAX; s++) {
        if (!modules[s].used || !modules[s].embedded ||
            (have && manifest_find(mf, modules[s].name) >= 0))
            continue;
        if (!index--)
            return (int)s;
    }
    return -1;
}

int64_t module_request(ArkDriverRequest *q) {
    if (!initialized)
        return -38;
    q->error[0] = 0;
    if (q->op == ARK_DRV_LIST || q->op == ARK_DRV_QUERY) {
        if (!module_active())
            return -1;
        Manifest mf;
        bool have = manifest_load(&mf);
        unsigned count = have ? mf.count : 0;
        unsigned total = count + embedded_count(&mf, have);
        q->count = total;
        if (q->op == ARK_DRV_LIST) {
            if (q->index >= total)
                return -2;
            memset(&q->info, 0, sizeof q->info);
            q->info.index = q->index;
            if (q->index >= count) {
                int slot = embedded_at(&mf, have, q->index - count);
                if (slot < 0)
                    return -2;
                const Module *m = &modules[slot];
                q->info.state = m->state;
                q->info.version = m->version;
                q->info.api = ARK_DRIVER_API;
                q->info.image_bytes = m->image_bytes;
                strcopy(q->info.name, m->name, sizeof q->info.name);
                memcpy(q->info.sha256, m->sha256, 32);
                module_detail(m, q->info.detail);
                return 0;
            }
            const ManifestEntry *e = &mf.entry[q->index];
            int slot = slot_by_name(e->name);
            q->info.state = slot >= 0 ? modules[slot].state : ARK_DRV_STATE_FAILED;
            q->info.version = e->version;
            q->info.api = e->api;
            q->info.image_bytes = e->bytes;
            strcopy(q->info.name, e->name, sizeof q->info.name);
            memcpy(q->info.sha256, e->sha256, 32);
            if (slot >= 0) {
                q->info.image_bytes = modules[slot].image_bytes;
                module_detail(&modules[slot], q->info.detail);
            }
            return 0;
        }
        /* QUERY by name */
        if (!name_ok(q->name))
            return -22;
        int at = have ? manifest_find(&mf, q->name) : -1;
        int slot = slot_by_name(q->name);
        if (at < 0 && slot < 0)
            return -2;
        memset(&q->info, 0, sizeof q->info);
        strcopy(q->info.name, q->name, sizeof q->info.name);
        if (at >= 0) {
            q->info.version = mf.entry[at].version;
            q->info.api = mf.entry[at].api;
            q->info.image_bytes = mf.entry[at].bytes;
            memcpy(q->info.sha256, mf.entry[at].sha256, 32);
        } else if (slot >= 0) {
            /* Embedded-only module: the slot is the authoritative record. */
            q->info.version = modules[slot].version;
            q->info.api = ARK_DRIVER_API;
            q->info.image_bytes = modules[slot].image_bytes;
            memcpy(q->info.sha256, modules[slot].sha256, 32);
        }
        q->info.state = slot >= 0 ? modules[slot].state : ARK_DRV_STATE_FAILED;
        if (slot >= 0)
            module_detail(&modules[slot], q->info.detail);
        return 0;
    }
    if (q->op == ARK_DRV_INSTALL) {
        if (!module_may_change())
            return -1;
        if (!q->path[0])
            return -22;
        return module_install(q);
    }
    if (q->op == ARK_DRV_REMOVE) {
        if (!module_may_change())
            return -1;
        return module_remove(q);
    }
    return -22;
}

/* ---- host-test seams ------------------------------------------------------ */

#ifdef ARK_MODULE_HOST_TEST
void module_test_reset(void) {
    memset(modules, 0, sizeof modules);
    memset(poll_slot_registered, 0, sizeof poll_slot_registered);
    memset(irq_owner, 0xff, sizeof irq_owner);
    memset(irq_fn, 0, sizeof irq_fn);
    memset(irq_count, 0, sizeof irq_count);
    memset(test_routed, 0, sizeof test_routed);
    test_route_events = test_release_events = 0;
    test_route_fail = 0;
    test_map_count = 0;
    test_entry = 0;
    initialized = true;
    boot_loaded = false;
    loading_slot = -1;
    last_error[0] = 0;
}
const ArkDriverHost *module_test_host(void) {
    return &host_table;
}
int module_test_load(const void *file, size_t bytes) {
    return module_load(file, bytes);
}
void module_test_set_loading(int slot) {
    loading_slot = slot;
}
#endif

/* ---- read-only slot view (host tests and diagnostics) -------------------- */

int module_slot_view(unsigned slot, ModuleSlotView *out) {
    if (slot >= ARCO_MODULE_MAX || !out)
        return -22;
    const Module *m = &modules[slot];
    memset(out, 0, sizeof *out);
    out->used = m->used;
    out->state = m->state;
    out->version = m->version;
    out->devices = m->device_count;
    out->has_poll = m->has_poll;
    for (unsigned i = 0; i < MODULE_IRQ_LINES; i++)
        if (m->used && irq_owner[i] == (int)slot) {
            out->irq_lines |= 1u << i;
            out->irq_count += irq_count[i];
        }
    out->image_bytes = m->image_bytes;
    strcopy(out->name, m->name, sizeof out->name);
    memcpy(out->sha256, m->sha256, 32);
    return 0;
}
