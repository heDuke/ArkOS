/* .arco module loader: ARCO1 header validation, manifest verification, the
 * RX / RW+NX mapping split, per-slot stack, INIT + poll + device lifecycle and
 * the ARK_SYS_DRIVER permission surface. No fabricated install result: every
 * assertion below is about kernel policy, bounds and refusal paths.
 * gcc -std=c11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
 *   -DARK_MODULE_HOST_TEST -DARK_DEVICE_HOST_TEST -DARK_PCI_HOST_TEST \
 *   -DARK_BLOB_HOST_TEST -Iinclude tests/module_host_test.c kernel/module.c \
 *   kernel/device.c kernel/pci.c kernel/blob.c kernel/sha256.c kernel/lib.c \
 *   kernel/alloc.c -o build/module-host-test
 * ASAN_OPTIONS=detect_leaks=0 ./build/module-host-test
 */
#define _POSIX_C_SOURCE 200809L
#define ARK_KERNEL
#include "ark.h"
#include "ark_driver.h"
#include "ark_api.h"
#include "device.h"
#include "accounts.h"
#include "sha256.h"
#include "blob.h"
#include "block.h"
#include "storage.h"
#include "pci.h"
#include "module.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

/* ---- kernel seams ------------------------------------------------------- */

uint64_t platform_ticks(void) {
    return 1234567;
}
uint64_t platform_millis(void) {
    return 424242;
}
static char serial_log[8192];
void serial_write(const char *s) {
    size_t at = strlen(serial_log);
    while (*s && at + 1 < sizeof serial_log)
        serial_log[at++] = *s++;
    serial_log[at] = 0;
}

static AccountState stub_account = ACCOUNT_ACTIVE;
static bool stub_admin = true;
static uint32_t stub_uid = 1000;
static uint64_t stub_caps = ARK_CAP_SYSTEM;
AccountState accounts_state(void) {
    return stub_account;
}
bool accounts_current_admin(void) {
    return stub_admin;
}
uint32_t accounts_current_uid(void) {
    return stub_account == ACCOUNT_ACTIVE ? stub_uid : ACCOUNTS_UID_NONE;
}
bool process_has_cap(uint64_t mask) {
    return (stub_caps & mask) == mask;
}
bool process_user_range(uint64_t address, size_t bytes, bool write) {
    (void)write;
    return address >= 0x40000000ull && address + bytes <= 0x80000000ull;
}
void *process_kernel_alloc(size_t bytes) {
    return calloc(1, bytes);
}
void process_kernel_free(void *pointer, size_t bytes) {
    (void)bytes;
    free(pointer);
}

static VFile stub_file;
int vfs_find(const char *name) {
    return stub_file.size && !strcmp(stub_file.name, name) ? 0 : -1;
}
VFile *vfs_entry(int index) {
    return index == 0 && stub_file.size ? &stub_file : 0;
}
bool vfs_path_canonical(char out[128], const char *path) {
    snprintf(out, 128, "%s", path && path[0] == '/' ? path : "/");
    return path && path[0] == '/';
}
bool accounts_path_allowed(uint32_t uid, const char *absolute_path, bool write) {
    (void)uid;
    (void)write;
    return absolute_path && absolute_path[0] == '/';
}
BlockDevice *block_device(unsigned unit) {
    (void)unit;
    return 0;
}
bool block_read(BlockDevice *device, uint64_t lba, uint32_t sectors, void *buffer) {
    (void)device;
    (void)lba;
    (void)sectors;
    (void)buffer;
    return false;
}
bool block_flush(BlockDevice *device) {
    (void)device;
    return false;
}
const char *block_error(void) {
    return "no block device";
}
void process_wake(uint32_t pid) {
    (void)pid;
}
bool process_copy_from_user(void *dst, uint64_t src, size_t bytes) {
    (void)dst;
    (void)src;
    (void)bytes;
    return false;
}
bool process_copy_to_user(uint64_t dst, const void *src, size_t bytes) {
    (void)dst;
    (void)src;
    (void)bytes;
    return false;
}
uint32_t process_current_uid(void) {
    return stub_uid;
}
uint32_t process_current_pid(void) {
    return 1;
}

/* ArkFS system volume backing the blob store: an in-memory region so the
 * manifest/image round trip is exercised without any real disk. */
static uint8_t volume[8u * 1024 * 1024];
static bool volume_ready;
bool storage_mounted(void) {
    return volume_ready;
}
uint32_t storage_volume_sectors(void) {
    return (uint32_t)(sizeof volume / 512);
}
bool storage_volume_io(uint32_t lba, uint32_t n, void *buf, bool write) {
    if (!volume_ready)
        return false;
    if ((uint64_t)lba * 512 + (uint64_t)n * 512 > sizeof volume)
        return false;
    if (write)
        memcpy(volume + lba * 512, buf, n * 512);
    else
        memcpy(buf, volume + lba * 512, n * 512);
    return true;
}
bool storage_volume_flush(void) {
    return volume_ready;
}

/* Synthetic PCI configuration space: a driver reaches hardware through the
 * host table, so the harness needs a real decoded bus behind it. */
#define BUSES 2
#define DEVS 32
#define FUNCS 4
#define WORDS 64
static uint32_t config[BUSES][DEVS][FUNCS][WORDS];
static uint32_t synthetic_read(uint32_t bdf, unsigned offset) {
    unsigned bus = (bdf >> 16) & 0xff, dev = (bdf >> 11) & 0x1f, function = (bdf >> 8) & 0x7;
    if (bus >= BUSES || dev >= DEVS || function >= FUNCS || offset >= WORDS * 4)
        return 0xffffffffu;
    return config[bus][dev][function][offset / 4];
}
static void bus_install(unsigned bus, unsigned dev, unsigned function, uint32_t vendor,
                        uint32_t device, uint32_t class_code) {
    config[bus][dev][function][0] = (device << 16) | vendor;
    config[bus][dev][function][1] = 0x00000006;
    config[bus][dev][function][2] = class_code;
}
static void bus_reset(void) {
    memset(config, 0xff, sizeof config);
    bus_install(0, 3, 0, 0x8086, 0x2922, 0x01060100); /* synthetic AHCI */
    pci_test_set_config(synthetic_read);
    pci_init(); /* idempotent scan over the synthetic bus */
}

/* ---- harness ------------------------------------------------------------ */

static unsigned checks, failures;
#define CHECK(test)                                                                                \
    do {                                                                                           \
        ++checks;                                                                                  \
        if (!(test)) {                                                                             \
            ++failures;                                                                            \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #test);                        \
        }                                                                                          \
    } while (0)

int64_t blob_request(ArkBlobRequest *request);
const ArkDriverHost *module_test_host(void);

/* Driver body used by the harness: one PCI-class node, one poll callback, and
 * a second poll registration that the host table must refuse. */
static unsigned stub_entry_calls, stub_poll_calls;
static uint64_t stub_seen_millis;
static bool stub_poll_notifies;
static int stub_device_index = -1;

static void stub_poll(void) {
    const ArkDriverHost *host = module_test_host();
    ++stub_poll_calls;
    if (stub_poll_notifies && stub_device_index >= 0) {
        host->device_add_counters((uint32_t)stub_device_index, 512, 128, 3, 0);
        host->device_notify((uint32_t)stub_device_index);
    }
}
static int64_t stub_entry(const ArkDriverHost *host, uint32_t op) {
    ++stub_entry_calls;
    stub_seen_millis = host->millis();
    if (op != ARCO_OP_INIT)
        return 0;
    ArkDeviceInfo info = {0};
    info.class_id = ARK_DEV_CLASS_PCI;
    info.bus = ARK_BUS_PCI;
    info.flags = ARK_DEV_PRESENT | ARK_DEV_MODULE;
    info.state = ARK_DEV_STATE_OK;
    snprintf(info.name, sizeof info.name, "arkos-module");
    snprintf(info.driver, sizeof info.driver, "demo");
    snprintf(info.detail, sizeof info.detail, "host-test driver");
    stub_device_index = host->device_register(&info);
    if (host->device_register_poll(stub_poll) < 0)
        return -16;
    if (host->device_register_poll(stub_poll) >= 0)
        return -5; /* the host table must refuse a second callback */
    return stub_device_index >= 0 ? 0 : -12;
}
static int64_t refuse_entry(const ArkDriverHost *host, uint32_t op) {
    (void)host;
    (void)op;
    return -5;
}

/* ---- .arco builder (independent of the loader's own checksums) ---------- */

static uint32_t crc32_of(const void *data, size_t bytes) {
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

static uint8_t image[16 * 1024];
static uint8_t file_buf[ARCO_HEADER_BYTES + sizeof image];

static size_t build_arco(uint8_t *out, const char *name, uint32_t version,
                         size_t image_bytes, size_t data_off, size_t bss_size) {
    for (size_t i = 0; i < image_bytes; i++)
        image[i] = (uint8_t)(i * 7 + 3);
    memcpy(out + ARCO_HEADER_BYTES, image, image_bytes);
    ArcoHeader h;
    memset(&h, 0, sizeof h);
    memcpy(h.magic, "ARCO1\0\0\0", 8);
    snprintf(h.name, sizeof h.name, "%s", name);
    h.api = ARK_DRIVER_API;
    h.version = version;
    h.image_size = image_bytes;
    h.entry_off = 0x40;
    h.data_off = data_off ? data_off : image_bytes;
    h.bss_size = bss_size;
    h.image_crc32 = crc32_of(image, image_bytes);
    ark_sha256(image, image_bytes, h.image_sha256);
    h.header_crc32 = crc32_of(&h, sizeof h);
    memcpy(out, &h, sizeof h);
    return ARCO_HEADER_BYTES + image_bytes;
}

static void park_file(const char *path, const uint8_t *bytes, size_t n) {
    snprintf(stub_file.name, sizeof stub_file.name, "%s", path);
    memcpy(stub_file.data, bytes, n);
    stub_file.size = n;
}

static void reset_world(void) {
    volume_ready = true;
    memset(volume, 0, sizeof volume);
    module_test_reset();
    device_init();
    blob_test_reset();
    module_test_reset_maps();
    module_test_set_entry(stub_entry);
    bus_reset();
    stub_account = ACCOUNT_ACTIVE;
    stub_admin = true;
    stub_uid = 1000;
    stub_caps = ARK_CAP_SYSTEM;
    stub_entry_calls = stub_poll_calls = 0;
    stub_poll_notifies = false;
    stub_device_index = -1;
    memset(&stub_file, 0, sizeof stub_file);
    serial_log[0] = 0;
}

/* A reboot keeps the system volume: the blob records are re-read from storage
 * and the driver manifest is verified again exactly as at real boot. */
static void reboot_world(void) {
    module_test_reset();
    device_init();
    blob_test_reset();
    module_test_reset_maps();
    module_test_set_entry(stub_entry);
    bus_reset();
    stub_account = ACCOUNT_ACTIVE;
    stub_admin = true;
    stub_uid = 1000;
    stub_caps = ARK_CAP_SYSTEM;
    stub_entry_calls = stub_poll_calls = 0;
    stub_poll_notifies = false;
    stub_device_index = -1;
    memset(&stub_file, 0, sizeof stub_file);
    serial_log[0] = 0;
}

/* ---- header validation -------------------------------------------------- */

static void damage_magic(uint8_t *f, ArcoHeader *h, size_t n) {
    (void)h;
    (void)n;
    f[1] = 'X';
}
static void damage_api(uint8_t *f, ArcoHeader *h, size_t n) {
    (void)f;
    (void)n;
    h->api = ARK_DRIVER_API + 1;
}
static void damage_name_space(uint8_t *f, ArcoHeader *h, size_t n) {
    (void)f;
    (void)n;
    snprintf(h->name, sizeof h->name, "Bad Name");
}
static void damage_name_empty(uint8_t *f, ArcoHeader *h, size_t n) {
    (void)f;
    (void)n;
    h->name[0] = 0;
}
static void damage_name_unterminated(uint8_t *f, ArcoHeader *h, size_t n) {
    (void)f;
    (void)n;
    memset(h->name, 'a', sizeof h->name);
}
static void damage_image_size(uint8_t *f, ArcoHeader *h, size_t n) {
    (void)f;
    h->image_size = n - ARCO_HEADER_BYTES + 1;
}
static void damage_entry_off(uint8_t *f, ArcoHeader *h, size_t n) {
    (void)f;
    (void)n;
    h->entry_off = 0x100000;
}
static void damage_data_unaligned(uint8_t *f, ArcoHeader *h, size_t n) {
    (void)f;
    (void)n;
    h->data_off = 0x801;
}
static void damage_bss_unaligned(uint8_t *f, ArcoHeader *h, size_t n) {
    (void)f;
    (void)n;
    h->bss_size = 0x1234;
}
static void damage_reserved(uint8_t *f, ArcoHeader *h, size_t n) {
    (void)f;
    (void)n;
    h->reserved[3] = 9;
}
static void damage_image_crc(uint8_t *f, ArcoHeader *h, size_t n) {
    (void)f;
    (void)n;
    h->image_crc32 ^= 1u;
}
static void damage_image_sha(uint8_t *f, ArcoHeader *h, size_t n) {
    (void)f;
    (void)n;
    h->image_sha256[5] ^= 0x40;
}
static void damage_payload(uint8_t *f, ArcoHeader *h, size_t n) {
    (void)h;
    (void)n;
    f[ARCO_HEADER_BYTES + 64] ^= 0xff;
}
static void damage_header_crc(uint8_t *f, ArcoHeader *h, size_t n) {
    (void)f;
    (void)n;
    h->version += 1; /* CRC not restamped by the case */
}

static void test_header_validation(void) {
    printf("header validation\n");
    reset_world();
    size_t n = build_arco(file_buf, "demo", 0x00010000, 8192, 4096, 4096);
    int slot = module_test_load(file_buf, n);
    CHECK(slot == 0);
    ModuleSlotView view;
    CHECK(module_slot_view(0, &view) == 0);
    CHECK(view.state == ARK_DRV_STATE_LOADED);
    CHECK(!strcmp(view.name, "demo"));
    CHECK(view.version == 0x00010000);
    CHECK(view.image_bytes == 8192);
    CHECK(view.devices == 1);
    CHECK(view.has_poll == 1);
    CHECK(stub_entry_calls == 1);
    CHECK(stub_seen_millis == 424242);
    /* the PCI scan's own inventory node coexists with the module's node */
    CHECK(device_count() == 2);
    CHECK(stub_device_index == 1);
    CHECK(device_info(0) && !strcmp(device_info(0)->driver, "pci"));

    struct {
        const char *what;
        void (*damage)(uint8_t *file, ArcoHeader *header, size_t bytes);
        bool restamp;
    } cases[] = {
        {"magic", damage_magic, true},
        {"api", damage_api, true},
        {"name space", damage_name_space, true},
        {"name empty", damage_name_empty, true},
        {"name unterminated", damage_name_unterminated, true},
        {"image_size", damage_image_size, true},
        {"entry_off", damage_entry_off, true},
        {"data_off unaligned", damage_data_unaligned, true},
        {"bss unaligned", damage_bss_unaligned, true},
        {"reserved nonzero", damage_reserved, true},
        {"image crc", damage_image_crc, true},
        {"image sha", damage_image_sha, true},
        {"payload byte", damage_payload, true},
        {"header crc", damage_header_crc, false},
    };
    for (unsigned i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        uint8_t copy[sizeof file_buf];
        memcpy(copy, file_buf, n);
        ArcoHeader damaged;
        memcpy(&damaged, copy, sizeof damaged);
        cases[i].damage(copy, &damaged, n);
        if (cases[i].restamp) {
            damaged.header_crc32 = crc32_of(&damaged, sizeof damaged);
            memcpy(copy, &damaged, sizeof damaged);
        }
        int s = module_test_load(copy, n);
        CHECK(s < 0);
        CHECK(s < 0 || !strcmp(module_slot_view(0, &view) ? view.name : "", "demo"));
        if (s < 0)
            ++checks;
    }
    /* refusal left the healthy slot untouched and reported a reason */
    CHECK(module_slot_view(0, &view) == 0);
    CHECK(view.state == ARK_DRV_STATE_LOADED);
    CHECK(module_error()[0] != 0);
    CHECK(stub_entry_calls == 1); /* no refused image reached an entry point */

    /* structural sizes */
    CHECK(module_test_load(file_buf, ARCO_HEADER_BYTES) < 0);
    CHECK(module_test_load(file_buf, 4) < 0);
    CHECK(module_slot_view(ARCO_MODULE_MAX, &view) == -22);
    CHECK(module_slot_view(0, 0) == -22);
}

/* ---- mapping split ------------------------------------------------------ */

static void test_mapping(void) {
    printf("mapping split\n");
    reset_world();
    size_t n = build_arco(file_buf, "demo", 1, 8192, 4096, 4096);
    CHECK(module_test_load(file_buf, n) == 0);
    ModuleMap maps[32];
    unsigned count = module_test_maps(maps);
    const uint64_t base = (uint64_t)1 << 39;
    const uint64_t stride = 2ull * 1024 * 1024;
    CHECK(count == 3);
    CHECK(count > 0 && maps[0].va == base && maps[0].bytes == 4096);
    CHECK(count > 0 && !maps[0].writable && !maps[0].nx);
    CHECK(count > 1 && maps[1].va == base + 4096 && maps[1].bytes == 8192);
    CHECK(count > 1 && maps[1].writable && maps[1].nx);
    CHECK(count > 2 && maps[2].va == base + stride - 32768 && maps[2].bytes == 32768);
    CHECK(count > 2 && maps[2].writable && maps[2].nx);
    /* the page below the stack guard stays unmapped, so a stack overflow
     * faults instead of running into module data */
    uint64_t guard = base + stride - 32769;
    bool hole = true;
    for (unsigned i = 0; i < count; i++)
        if (maps[i].va <= guard && guard < maps[i].va + maps[i].bytes)
            hole = false;
    CHECK(hole);
    /* nothing is mapped user-accessible: window pages are supervisor only */

    /* a second slot lands on its own 2 MiB stride */
    reset_world();
    n = build_arco(file_buf, "first", 1, 4096, 4096, 0);
    CHECK(module_test_load(file_buf, n) == 0);
    uint8_t second[sizeof file_buf];
    size_t m = build_arco(second, "second", 1, 4096, 4096, 0);
    CHECK(module_test_load(second, m) == 1);
    count = module_test_maps(maps);
    bool slot1 = false, overlap = false;
    for (unsigned i = 0; i < count; i++)
        if (maps[i].va >= base + stride)
            slot1 = true;
    for (unsigned i = 0; i < count; i++)
        for (unsigned j = i + 1; j < count; j++)
            if (maps[i].va < maps[j].va + maps[j].bytes &&
                maps[j].va < maps[i].va + maps[i].bytes)
                overlap = true;
    CHECK(slot1);
    CHECK(!overlap);

    /* read-only image: no data pages at all */
    reset_world();
    n = build_arco(file_buf, "ro", 1, 8192, 0, 0);
    CHECK(module_test_load(file_buf, n) == 0);
    count = module_test_maps(maps);
    CHECK(count == 2);
    CHECK(count > 0 && maps[0].bytes == 8192 && !maps[0].writable && !maps[0].nx);

    /* duplicate name, then slot exhaustion */
    reset_world();
    n = build_arco(file_buf, "first", 1, 4096, 4096, 0);
    CHECK(module_test_load(file_buf, n) == 0);
    m = build_arco(second, "first", 1, 4096, 4096, 0);
    CHECK(module_test_load(second, m) < 0);
    CHECK(strstr(module_error(), "already loaded") != 0);
    for (unsigned s = 1; s < ARCO_MODULE_MAX; s++) {
        char name[32];
        snprintf(name, sizeof name, "mod%u", s);
        uint8_t buf[sizeof file_buf];
        size_t k = build_arco(buf, name, 1, 4096, 4096, 0);
        CHECK(module_test_load(buf, k) == (int)s);
    }
    uint8_t extra[sizeof file_buf];
    size_t e = build_arco(extra, "overflow", 1, 4096, 4096, 0);
    CHECK(module_test_load(extra, e) < 0);
    CHECK(strstr(module_error(), "slots") != 0);
}

/* ---- INIT refusal ------------------------------------------------------- */

static void test_init_rejection(void) {
    printf("init refusal\n");
    reset_world();
    size_t n = build_arco(file_buf, "demo", 1, 4096, 4096, 0);
    module_test_set_entry(refuse_entry);
    int slot = module_test_load(file_buf, n);
    CHECK(slot == 0);
    ModuleSlotView view;
    CHECK(module_slot_view(0, &view) == 0);
    CHECK(view.state == ARK_DRV_STATE_FAILED);
    CHECK(strstr(module_error(), "init") != 0);
    /* a FAILED slot is reusable: the replacement takes the same name */
    module_test_set_entry(stub_entry);
    uint8_t other[sizeof file_buf];
    size_t m = build_arco(other, "demo", 2, 4096, 4096, 0);
    CHECK(module_test_load(other, m) == 0);
    CHECK(module_slot_view(0, &view) == 0);
    CHECK(view.state == ARK_DRV_STATE_LOADED && view.version == 2);
    /* an image with no harness entry installed cannot execute */
    reset_world();
    n = build_arco(file_buf, "demo", 1, 4096, 4096, 0);
    module_test_set_entry(0);
    CHECK(module_test_load(file_buf, n) < 0);
    CHECK(device_count() == 1); /* only the PCI inventory node, no driver ran */
}

/* ---- syscall surface ---------------------------------------------------- */

static void test_service(void) {
    printf("syscall surface\n");
    reset_world();
    ArkDriverRequest q;
    memset(&q, 0, sizeof q);

    /* LIST needs an active session and reports the manifest, not slots */
    q.op = ARK_DRV_LIST;
    CHECK(module_request(&q) == -2);
    CHECK(q.count == 0); /* the total is reported even on an empty end-of-list */
    stub_account = ACCOUNT_LOGGED_OUT;
    CHECK(module_request(&q) == -1);
    stub_account = ACCOUNT_ACTIVE;

    /* INSTALL from a local path */
    size_t n = build_arco(file_buf, "demo", 0x00020000, 8192, 4096, 4096);
    park_file("/demo.arco", file_buf, n);
    q.op = ARK_DRV_INSTALL;
    snprintf(q.path, sizeof q.path, "/demo.arco");
    CHECK(module_request(&q) == 0);
    CHECK(q.count == 1);
    CHECK(!strcmp(q.info.name, "demo"));
    CHECK(q.info.state == ARK_DRV_STATE_LOADED);
    CHECK(q.info.version == 0x00020000);
    CHECK(q.info.image_bytes == n);
    { ArkBlobRequest mb = {0};
      mb.op = ARK_BLOB_LIST;
      mb.index = 0;
      bool saw_image = false, saw_manifest = false;
      while (blob_kernel_request(&mb, 1000) == 0) {
          saw_image |= !strcmp(mb.name, "@drv.demo");
          saw_manifest |= !strcmp(mb.name, "@drv.manifest");
          ++mb.index;
      }
      CHECK(saw_image && saw_manifest); } /* image and manifest both stored */
    CHECK(device_count() == 2);
    ModuleSlotView view;
    CHECK(module_slot_view(0, &view) == 0);
    CHECK(view.state == ARK_DRV_STATE_LOADED && view.devices == 1);

    /* LIST / QUERY now see it */
    q.op = ARK_DRV_LIST;
    q.index = 0;
    CHECK(module_request(&q) == 0);
    CHECK(q.count == 1);
    CHECK(!strcmp(q.info.name, "demo"));
    CHECK(strstr(q.info.detail, "devices:") != 0);
    q.index = 1;
    CHECK(module_request(&q) == -2);
    q.op = ARK_DRV_QUERY;
    snprintf(q.name, sizeof q.name, "demo");
    CHECK(module_request(&q) == 0);
    CHECK(q.info.version == 0x00020000);
    CHECK(memcmp(q.info.sha256, view.sha256, 32) == 0);
    snprintf(q.name, sizeof q.name, "absent");
    CHECK(module_request(&q) == -2);
    q.op = 77;
    CHECK(module_request(&q) == -22);

    /* permission cases: SYSTEM + active + admin */
    q.op = ARK_DRV_INSTALL;
    snprintf(q.path, sizeof q.path, "/demo.arco");
    stub_admin = false;
    CHECK(module_request(&q) == -1);
    stub_admin = true;
    stub_caps = ARK_CAP_FILES;
    CHECK(module_request(&q) == -1);
    stub_caps = ARK_CAP_SYSTEM;
    stub_account = ACCOUNT_LOCKED;
    CHECK(module_request(&q) == -1);
    stub_account = ACCOUNT_ACTIVE;
    CHECK(device_count() == 2); /* no refusal installed anything */

    /* REMOVE needs the same authority and clears visibility */
    q.op = ARK_DRV_REMOVE;
    snprintf(q.name, sizeof q.name, "demo");
    stub_admin = false;
    CHECK(module_request(&q) == -1);
    stub_admin = true;
    CHECK(module_request(&q) == 0);
    CHECK(q.count == 0);
    CHECK(q.info.state == ARK_DRV_STATE_DISABLED);
    ModuleSlotView gone;
    CHECK(module_slot_view(0, &gone) == 0);
    CHECK(gone.used == 0); /* slot and its pages returned to the pool */
    const ArkDeviceInfo *node = device_info((uint32_t)stub_device_index);
    CHECK(node && node->state == ARK_DEV_STATE_ABSENT && node->flags == 0);
    CHECK(device_info(0) && !strcmp(device_info(0)->driver, "pci"));
    CHECK(module_request(&q) == -2);
    q.op = ARK_DRV_QUERY;
    CHECK(module_request(&q) == -2);
    snprintf(q.name, sizeof q.name, "bad name");
    CHECK(module_request(&q) == -22);

    /* refused sources never change stored state */
    q.op = ARK_DRV_INSTALL;
    uint8_t junk[512];
    memset(junk, 0xcc, sizeof junk);
    park_file("/junk.arco", junk, sizeof junk);
    snprintf(q.path, sizeof q.path, "/junk.arco");
    CHECK(module_request(&q) == -22);
    CHECK(strstr(q.error, "ARCO1") != 0);
    q.op = ARK_DRV_LIST;
    q.index = 0;
    CHECK(module_request(&q) == -2);
    CHECK(q.count == 0);
    memset(&stub_file, 0, sizeof stub_file);
    snprintf(q.path, sizeof q.path, "/missing.arco");
    q.op = ARK_DRV_INSTALL;
    CHECK(module_request(&q) == -2);
    q.path[0] = 0;
    CHECK(module_request(&q) == -22);
}

/* ---- install rollback --------------------------------------------------- */

static void test_install_refused_init(void) {
    printf("install rollback\n");
    reset_world();
    size_t n = build_arco(file_buf, "demo", 1, 4096, 4096, 0);
    park_file("/demo.arco", file_buf, n);
    module_test_set_entry(refuse_entry);
    ArkDriverRequest q = {0};
    q.op = ARK_DRV_INSTALL;
    snprintf(q.path, sizeof q.path, "/demo.arco");
    CHECK(module_request(&q) < 0);
    CHECK(q.error[0] != 0);
    q.op = ARK_DRV_LIST;
    CHECK(module_request(&q) == -2);
    CHECK(q.count == 0); /* nothing persisted after a refused INIT */
    ModuleSlotView view;
    CHECK(module_slot_view(0, &view) == 0);
    CHECK(view.state == ARK_DRV_STATE_FAILED);
}

/* ---- manifest tampering ------------------------------------------------- */

static void test_manifest_tamper(void) {
    printf("manifest integrity\n");
    reset_world();
    size_t n = build_arco(file_buf, "demo", 1, 4096, 4096, 0);
    park_file("/demo.arco", file_buf, n);
    ArkDriverRequest q = {0};
    q.op = ARK_DRV_INSTALL;
    snprintf(q.path, sizeof q.path, "/demo.arco");
    CHECK(module_request(&q) == 0);

    /* the stored image is a blob the kernel owns; corrupt its payload bytes
     * and the next boot-time verification must refuse it */
    ArkBlobRequest b = {0};
    b.op = ARK_BLOB_READ;
    snprintf(b.name, sizeof b.name, "@drv.demo");
    b.capacity = n; /* READ requires the exact stored length */
    b.buffer = (uint64_t)(uintptr_t)file_buf;
    CHECK(blob_kernel_request(&b, 1000) == 0);
    CHECK(b.size == n);
    CHECK(memcmp(file_buf + ARCO_HEADER_BYTES, image, 16) == 0); /* image bytes came back */
    file_buf[ARCO_HEADER_BYTES + 10] ^= 0xff;
    ArkBlobRequest w = {0};
    w.op = ARK_BLOB_WRITE;
    snprintf(w.name, sizeof w.name, "@drv.demo");
    w.capacity = n;
    w.buffer = (uint64_t)(uintptr_t)file_buf;
    CHECK(blob_kernel_request(&w, 1000) == 0);

    /* boot load verifies the manifest hash before mapping anything */
    reboot_world();
    module_boot_load();
    CHECK(device_count() == 1); /* only the PCI inventory node; no driver ran */
    ModuleSlotView none;
    CHECK(module_slot_view(0, &none) == 0);
    CHECK(none.used == 0);
    CHECK(strstr(serial_log, "load failed") != 0);
    CHECK(strstr(serial_log, "demo") != 0);
    CHECK(serial_log[0] != 0);
}

/* ---- blob namespace isolation ------------------------------------------- */

static void test_blob_namespace(void) {
    printf("blob namespace\n");
    reset_world();
    ArkBlobRequest b = {0};
    b.op = ARK_BLOB_WRITE;
    snprintf(b.name, sizeof b.name, "@drv.manifest");
    b.capacity = 4;
    b.buffer = (uint64_t)(uintptr_t)"ARKD1";
    CHECK(blob_kernel_request(&b, 1000) == 0);
    /* The kernel path can see its own namespace; the user syscall cannot. */
    stub_caps = ARK_CAP_FILES;
    b.op = ARK_BLOB_READ;
    snprintf(b.name, sizeof b.name, "@drv.manifest");
    CHECK(blob_request(&b) == -1);
    b.op = ARK_BLOB_WRITE;
    CHECK(blob_request(&b) == -1);
    b.op = ARK_BLOB_REMOVE;
    CHECK(blob_request(&b) == -1);
    b.op = ARK_BLOB_LIST;
    b.index = 0;
    CHECK(blob_request(&b) == -2); /* only entry hidden: empty user list */
    stub_caps = ARK_CAP_SYSTEM;
    /* module.c never addresses a @drv record below uid 1000 */
    b.op = ARK_BLOB_READ;
    snprintf(b.name, sizeof b.name, "@drv.demo");
    b.capacity = sizeof file_buf;
    b.buffer = (uint64_t)(uintptr_t)file_buf;
    CHECK(blob_kernel_request(&b, 999) == -1);
    CHECK(blob_kernel_request(&b, 1000) < 0); /* absent */
}

/* ---- host table policy -------------------------------------------------- */

static void test_host_table(void) {
    printf("host table\n");
    reset_world();
    const ArkDriverHost *host = module_test_host();
    CHECK(host != 0);
    CHECK(host->millis() == 424242 && host->ticks() == 1234567);
    CHECK(host->pci_count() == 1);
    uint32_t record[10];
    CHECK(host->pci_device(0, record) == 1);
    CHECK(record[0] == 0x1800u); /* bus 0, dev 3, function 0 */
    CHECK(record[1] == 0x8086 && record[2] == 0x2922);
    CHECK(record[3] == 0x01u); /* class */
    CHECK(host->pci_device(1, record) == 0);
    CHECK(host->pci_device(9999, record) == 0);
    CHECK(host->pci_device(0, 0) == 0);
    uint32_t bdf = 0;
    CHECK(host->pci_find(0x8086, 0x2922, &bdf) == 0);
    CHECK(bdf == 0x1800u);
    CHECK(host->pci_find(0xdead, 0xbeef, &bdf) == -2);
    CHECK(host->pci_find(0x8086, 0x2922, 0) == -22);
    CHECK(host->pci_read(0x1800, 0) == 0x29228086u);
    CHECK(host->pci_read8(0x1800, 0) == 0x86);
    CHECK(host->pci_read(0xffff, 0) == 0xffffffffu);
    uint64_t bar_base;
    CHECK(host->pci_bar_base(0x1800, 0, &bar_base) == -2); /* device has no BAR */
    CHECK(host->pci_bar_base(0x1800, 9, &bar_base) == -2);
    /* MMIO alias guard: no zero, no zero length, nothing outside 4 GiB */
    CHECK(host->map_mmio(0, 4096) == 0);
    CHECK(host->map_mmio(0x1000, 0) == 0);
    CHECK(host->map_mmio(0x1000, 1ull << 32) == 0);
    CHECK(host->map_mmio(0x1000, 128ull * 1024 * 1024) == 0);
    CHECK(host->map_mmio(0xfffff000ull, 0x4000) == 0);
    CHECK(host->map_mmio(0xf0000000ull, 4096) != 0);
    /* log must stay bounded and prefix every line */
    char big[512];
    memset(big, 'x', sizeof big - 1);
    big[sizeof big - 1] = 0;
    host->log(big);
    host->log(0);
    CHECK(strstr(serial_log, "[drv] xxx") != 0);
    /* device registration outside a load window is refused */
    ArkDeviceInfo info = {0};
    info.class_id = ARK_DEV_CLASS_PCI;
    info.flags = ARK_DEV_PRESENT;
    info.state = ARK_DEV_STATE_OK;
    module_test_set_loading(-1);
    CHECK(host->device_register(&info) == -22);
    CHECK(host->device_register(0) == -22);
    /* one poll callback per module, only during INIT */
    module_test_set_loading(0);
    CHECK(host->device_register_poll(stub_poll) == 0);
    CHECK(host->device_register_poll(stub_poll) == -16);
    module_test_set_loading(-1);
    CHECK(host->device_register_poll(stub_poll) == -22);
    CHECK(host->device_register_poll(0) == -22);
    /* one ISA line per module, only during INIT; platform lines stay busy.
     * route allocates delivery; a second owner of a live line fails. */
    unsigned routes0 = 0, releases0 = 0;
    CHECK(module_test_routed(&routes0, &releases0) == 0);
    module_test_set_loading(0);
    CHECK(host->irq_attach(9, stub_poll) == 0);
    CHECK(module_test_routed(0, 0) == 1);
    CHECK(host->irq_attach(9, stub_poll) == -16);
    CHECK(host->irq_attach(0, stub_poll) == -16);
    CHECK(host->irq_attach(12, stub_poll) == -16);
    CHECK(host->irq_attach(16, stub_poll) == -22);
    CHECK(host->irq_attach(5, 0) == -22);
    module_test_set_loading(1);
    CHECK(host->irq_attach(9, stub_poll) == -16);
    module_test_set_loading(-1);
    CHECK(host->irq_attach(5, stub_poll) == -22);
    /* a line bound to a slot that never finished loading is stale: dispatch
     * releases the RTE instead of reaching dead code */
    CHECK(module_irq_dispatch(9) == 0);
    CHECK(module_test_routed(0, 0) == 0); /* released */
    unsigned routes1 = 0, releases1 = 0;
    module_test_routed(&routes1, &releases1);
    CHECK(releases1 > releases0);
    /* reload: the same line reallocates exactly once, no leftover */
    module_test_set_loading(1);
    CHECK(host->irq_attach(9, stub_poll) == 0);
    CHECK(module_test_routed(0, 0) == 1);
    module_test_set_loading(-1);
    /* a platform that cannot allocate an RTE fails visibly (-19); the
     * binding is not left half-installed. */
    module_test_route_fail(-19);
    module_test_set_loading(0);
    CHECK(host->irq_attach(5, stub_poll) == -19);
    module_test_route_fail(0);
    CHECK(module_test_routed(0, 0) == 1); /* only irq9 from slot 1 */
    /* counters and state against a missing node */
    CHECK(host->device_add_counters(9999, 1, 1, 1, 1) == -22);
    CHECK(host->device_set_state(9999, ARK_DEV_PRESENT, ARK_DEV_STATE_OK) == -22);
    CHECK(host->device_set_state(0, 1u << 30, 1) == -22);
    CHECK(host->device_notify(9999) == -22);
    /* allocation seam is the kernel heap, not a raw page allocator */
    void *p = host->alloc(64);
    CHECK(p != 0);
    host->free(p);
}

/* ---- device lifecycle through a real driver entry ----------------------- */

static void test_device_lifecycle(void) {
    printf("device lifecycle\n");
    reset_world();
    module_test_set_loading(0);
    const ArkDriverHost *host = module_test_host();
    stub_poll_notifies = true;
    CHECK(stub_entry(host, ARCO_OP_INIT) == 0);
    CHECK(stub_entry_calls == 1);
    CHECK(device_count() == 2);
    CHECK(stub_device_index == 1);
    const ArkDeviceInfo *node = device_info((uint32_t)stub_device_index);
    CHECK(node && (node->flags & ARK_DEV_MODULE) != 0);
    CHECK(node && !strcmp(node->driver, "demo"));
    CHECK(node && !strcmp(node->name, "arkos-module"));
    CHECK(node && node->state == ARK_DEV_STATE_OK);

    uint64_t ops_before = node->ops;
    stub_poll();
    node = device_info((uint32_t)stub_device_index);
    CHECK(node && node->ops == ops_before + 3);
    CHECK(node && node->rx_bytes == 512 && node->tx_bytes == 128);
    CHECK(stub_poll_calls == 1);
    CHECK(stub_entry(host, ARCO_OP_DEINIT) == 0);
    CHECK(stub_entry_calls == 2);
}

/* ---- the real packer's output ------------------------------------------- */

/* scripts/arco.py writes the ARCO1 header this very loader parses. Running its
 * real output through the loader keeps the tool and the kernel from drifting
 * apart on field offsets or checksum coverage. */
static void test_packer_output(void) {
    printf("packer output\n");
    FILE *pipe = popen("python3 scripts/arco.py sdk/driver_demo.c "
                       "-o build/module_test.arco --name packtest --version 2.3.4 "
                       "2>&1", "r");
    CHECK(pipe != 0);
    if (!pipe)
        return;
    char line[512];
    bool failed = false;
    while (fgets(line, sizeof line, pipe)) {
        /* the packer prints "<output>: ..." on success; only a leading
         * "arco:" prefix is a diagnostic */
        if (strstr(line, "Traceback") || !strncmp(line, "arco:", 5))
            failed = true;
    }
    CHECK(pclose(pipe) == 0);
    CHECK(!failed);

    FILE *handle = fopen("build/module_test.arco", "rb");
    CHECK(handle != 0);
    if (!handle)
        return;
    static uint8_t packed[ARCO_HEADER_BYTES + 65536];
    size_t bytes = fread(packed, 1, sizeof packed, handle);
    fclose(handle);
    CHECK(bytes > ARCO_HEADER_BYTES);

    reset_world();
    module_test_set_entry(stub_entry);
    CHECK(module_test_load(packed, bytes) == 0);
    ModuleSlotView view;
    CHECK(module_slot_view(0, &view) == 0);
    CHECK(view.state == ARK_DRV_STATE_LOADED);
    CHECK(view.version == 0x00020304u); /* 2.3.4 -> major<<16|minor<<8|patch */
    CHECK(!strcmp(view.name, "packtest"));
    CHECK(view.has_poll == 1);
    CHECK(view.devices == 1);
    CHECK(stub_entry_calls == 1);

    /* One flipped payload byte must fail every check the loader makes. */
    uint8_t damaged[ARCO_HEADER_BYTES + 65536];
    memcpy(damaged, packed, bytes);
    damaged[ARCO_HEADER_BYTES + 32] ^= 0x01;
    reset_world();
    CHECK(module_test_load(damaged, bytes) < 0);
    /* A flipped header byte must fail the header CRC. */
    memcpy(damaged, packed, bytes);
    damaged[44] ^= 0x01; /* version */
    CHECK(module_test_load(damaged, bytes) < 0);
    /* A reserved byte must be rejected outright. */
    memcpy(damaged, packed, bytes);
    damaged[200] = 1;
    CHECK(module_test_load(damaged, bytes) < 0);
    /* An image for a newer driver API must not load into this kernel. */
    memcpy(damaged, packed, bytes);
    ArcoHeader future;
    memcpy(&future, damaged, sizeof future);
    future.api = ARK_DRIVER_API + 1;
    future.header_crc32 = 0;
    future.header_crc32 = crc32_of(&future, sizeof future);
    memcpy(damaged, &future, sizeof future);
    CHECK(module_test_load(damaged, bytes) < 0);
    unlink("build/module_test.arco");
}

int main(void) {
    test_packer_output();
    test_header_validation();
    test_mapping();
    test_init_rejection();
    test_service();
    test_install_refused_init();
    test_manifest_tamper();
    test_blob_namespace();
    test_host_table();
    test_device_lifecycle();
    printf("%u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}