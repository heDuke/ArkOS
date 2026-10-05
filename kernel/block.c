/* Native polling ATA transport for primary master/slave. MIT license. */
#include "block.h"
#ifndef ARK_BLOCK_HOST_TEST
#include "ahci.h"
#include "ark_driver.h"
static bool native_ahci, initialized;
static const ArkBlockOps *module_ops;
static unsigned module_owner;
static int module_disk = -1;
#endif
static BlockDevice devices[2];
static const char *error_text = "";
static BlockStats io_statistics;
extern void process_record_io(bool, uint64_t) __attribute__((weak));
static bool account_transfer(bool ok, bool write, uint32_t sectors) {
    if (ok) {
        uint64_t bytes = (uint64_t)sectors * 512;
        if (write) {
            io_statistics.write_bytes += bytes;
            io_statistics.write_operations++;
        } else {
            io_statistics.read_bytes += bytes;
            io_statistics.read_operations++;
        }
        if (process_record_io)
            process_record_io(write, bytes);
    }
    return ok;
}
void block_statistics(BlockStats *out) {
    if (out)
        *out = io_statistics;
}
#ifndef ARK_BLOCK_HOST_TEST
static inline uint8_t in8(uint16_t p) {
    uint8_t v;
    __asm__ volatile("inb %1,%0" : "=a"(v) : "Nd"(p));
    return v;
}
static inline uint16_t in16(uint16_t p) {
    uint16_t v;
    __asm__ volatile("inw %1,%0" : "=a"(v) : "Nd"(p));
    return v;
}
static inline void out8(uint16_t p, uint8_t v) {
    __asm__ volatile("outb %0,%1" ::"a"(v), "Nd"(p));
}
static inline void out16(uint16_t p, uint16_t v) {
    __asm__ volatile("outw %0,%1" ::"a"(v), "Nd"(p));
}
static void delay(void) {
    for (unsigned i = 0; i < 4; i++)
        (void)in8(0x3f6);
}
static bool wait_ata(bool data, bool check) {
    uint64_t start = platform_ticks();
    for (unsigned i = 0; i < 10000000; i++) {
        uint8_t s = in8(0x3f6);
        if (!s || s == 255) {
            error_text = "ATA device absent";
            return false;
        }
        if (!(s & 0x80)) {
            if (check && (s & 0x21)) {
                error_text = "ATA I/O error";
                return false;
            }
            if (!data || (s & 8))
                return true;
        }
        if (platform_ticks() - start > 1000)
            break;
        __asm__ volatile("pause");
    }
    error_text = "ATA timeout";
    return false;
}
static bool identify(BlockDevice *d) {
    out8(0x3f6, 2);
    out8(0x1f6, (uint8_t)(0xe0 | (d->id << 4)));
    delay();
    uint8_t s = in8(0x1f7);
    if (!s || s == 255)
        return false;
    if (!wait_ata(false, false))
        return false;
    for (int p = 0x1f2; p <= 0x1f5; p++)
        out8((uint16_t)p, 0);
    out8(0x1f7, 0xec);
    delay();
    if (!wait_ata(true, true))
        return false;
    uint16_t id[256];
    for (unsigned i = 0; i < 256; i++)
        id[i] = in16(0x1f0);
    delay();
    if (!wait_ata(false, true) || !(id[49] & 512))
        return false;
    if ((id[106] & 0xc000) == 0x4000 && (id[106] & 0x1000) &&
        (((uint32_t)id[118] << 16) | id[117]) != 256)
        return false;
    if ((id[83] & 0xc000) != 0x4000 || !(id[83] & 0x1000))
        return false;
    d->sectors = ((uint32_t)id[61] << 16) | id[60];
    if (d->sectors > 0x10000000)
        d->sectors = 0x10000000;
    return d->sectors > 0;
}
static bool transfer(BlockDevice *d, uint64_t lba, uint32_t count, void *data, bool write) {
    uint8_t *b = data;
    if (!d || !d->present || !count || lba >= d->sectors || count > d->sectors - lba) {
        error_text = "Block request out of range";
        return false;
    }
    while (count) {
        unsigned n = count > 255 ? 255 : count; /* Select first: the other device may be absent. */
        out8(0x1f6, (uint8_t)(0xe0 | (d->id << 4) | ((lba >> 24) & 15)));
        delay();
        if (!wait_ata(false, false))
            return false;
        out8(0x1f1, 0);
        out8(0x1f2, (uint8_t)n);
        out8(0x1f3, (uint8_t)lba);
        out8(0x1f4, (uint8_t)(lba >> 8));
        out8(0x1f5, (uint8_t)(lba >> 16));
        out8(0x1f7, write ? 0x30 : 0x20);
        delay();
        for (unsigned s = 0; s < n; s++) {
            if (!wait_ata(true, true))
                return false;
            for (unsigned w = 0; w < 256; w++) {
                if (write)
                    out16(0x1f0, (uint16_t)(b[w * 2] | ((uint16_t)b[w * 2 + 1] << 8)));
                else {
                    uint16_t v = in16(0x1f0);
                    b[w * 2] = (uint8_t)v;
                    b[w * 2 + 1] = (uint8_t)(v >> 8);
                }
            }
            b += 512;
            delay();
        }
        if (!wait_ata(false, true))
            return false;
        lba += n;
        count -= n;
    }
    return true;
}
void block_init(void) {
    if (initialized)
        return;
    initialized = true;
    memset(devices, 0, sizeof(devices));
    unsigned found = ahci_init();
    if (found) {
        native_ahci = true;
        for (unsigned i = 0; i < 2; i++) {
            devices[i].id = i;
            devices[i].sectors = ahci_sectors(i);
            devices[i].present = devices[i].sectors != 0;
        }
        return;
    }
    for (unsigned i = 0; i < 2; i++) {
        devices[i].id = i;
        devices[i].present = identify(&devices[i]);
    } /* Leave master selected for legacy ArkFS transport. */
    out8(0x1f6, 0xe0);
    delay();
}
bool block_read(BlockDevice *d, uint64_t lba, uint32_t count, void *b) {
    if (module_ops && d && module_disk >= 0 && (int)d->id == module_disk)
        return account_transfer(d->present && module_ops->transfer(lba, count, b, 0) == 0, false,
                                count);
    return account_transfer(native_ahci
                                ? (d && d->present && ahci_transfer(d->id, lba, count, b, false))
                                : transfer(d, lba, count, b, false),
                            false, count);
}
bool block_write(BlockDevice *d, uint64_t lba, uint32_t count, const void *b) {
    if (module_ops && d && module_disk >= 0 && (int)d->id == module_disk)
        return account_transfer(
            d->present && module_ops->transfer(lba, count, (void *)b, 1) == 0, true, count);
    return account_transfer(
        native_ahci ? (d && d->present && ahci_transfer(d->id, lba, count, (void *)b, true))
                    : transfer(d, lba, count, (void *)b, true),
        true, count);
}
bool block_flush(BlockDevice *d) {
    if (module_ops && d && module_disk >= 0 && (int)d->id == module_disk)
        return d && d->present && module_ops->flush() == 0;
    if (native_ahci)
        return d && d->present && ahci_flush(d->id);
    if (!d || !d->present) {
        error_text = "ATA device absent";
        return false;
    }
    out8(0x1f6, (uint8_t)(0xe0 | (d->id << 4)));
    delay();
    if (!wait_ata(false, false))
        return false;
    out8(0x1f7, 0xe7);
    delay();
    return wait_ata(false, true);
}
#else
extern bool test_block_identify(unsigned, uint64_t *);
extern bool test_block_io(unsigned, uint64_t, uint32_t, void *, bool);
extern bool test_block_flush(unsigned);
void block_init(void) {
    for (unsigned i = 0; i < 2; i++) {
        devices[i].id = i;
        devices[i].present = test_block_identify(i, &devices[i].sectors);
    }
}
bool block_read(BlockDevice *d, uint64_t l, uint32_t n, void *b) {
    return account_transfer(d && d->present && test_block_io(d->id, l, n, b, false), false, n);
}
bool block_write(BlockDevice *d, uint64_t l, uint32_t n, const void *b) {
    return account_transfer(d && d->present && test_block_io(d->id, l, n, (void *)b, true), true,
                            n);
}
bool block_flush(BlockDevice *d) {
    return d && d->present && test_block_flush(d->id);
}
#endif
#ifndef ARK_BLOCK_HOST_TEST
int block_bind_ops(const void *ops, unsigned owner) {
    const ArkBlockOps *table = (const ArkBlockOps *)ops;
    if (!table || !table->sectors || !table->transfer || !table->flush)
        return -22;
    if (module_ops)
        return -16;
    if (!initialized)
        block_init();
    int slot = -1;
    for (unsigned i = 0; i < 2; i++)
        if (!devices[i].present) {
            slot = (int)i;
            break;
        }
    if (slot < 0) {
        error_text = "No free block slot for module disk";
        return -28;
    }
    uint64_t sec = table->sectors();
    if (!sec) {
        error_text = "Module disk reported zero capacity";
        return -19;
    }
    module_ops = table;
    module_owner = owner;
    module_disk = slot;
    devices[slot].id = (unsigned)slot;
    devices[slot].present = true;
    devices[slot].sectors = sec;
    error_text = "";
    return 0;
}
void block_unbind_ops(unsigned owner) {
    if (!module_ops || module_owner != owner)
        return;
    if (module_disk >= 0 && module_disk < 2) {
        devices[module_disk].present = false;
        devices[module_disk].sectors = 0;
    }
    module_ops = 0;
    module_disk = -1;
}
#else
int block_bind_ops(const void *ops, unsigned owner) {
    (void)ops;
    (void)owner;
    return -38;
}
void block_unbind_ops(unsigned owner) {
    (void)owner;
}
#endif
BlockDevice *block_device(unsigned id) {
    return id < 2 ? &devices[id] : 0;
}
const char *block_error(void) {
    return error_text;
}
