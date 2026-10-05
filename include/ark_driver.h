#ifndef ARK_DRIVER_H
#define ARK_DRIVER_H
/* .arco loadable kernel driver ABI.
 *
 * A .arco file is a 256-byte ARCO1 header followed by one contiguous
 * position-independent image produced by scripts/arco.py. The image carries
 * no relocations: every reference is RIP-relative, so the loader never
 * patches code. The header splits the image at data_off (4 KiB aligned);
 * [0, data_off) is mapped read+execute, [data_off, image_end + bss_size) is
 * mapped read+write+NX inside the kernel-only module window at PML4 index 1.
 * Drivers run in kernel mode on a dedicated 32 KiB stack per module.
 *
 * Trust model: the protected manifest blob @drv.manifest (ARKD1) stores the
 * SHA-256 of every installed file. The kernel hashes the complete file —
 * header included — and refuses anything whose bytes changed. The header's
 * own image_sha256 is payload integrity, not a publisher signature. */
#include <stdint.h>
#include <stddef.h>

#define ARCO_HEADER_BYTES 256u
#define ARCO_IMAGE_MAX (1024u * 1024u)
#define ARCO_BSS_MAX (256u * 1024u)
#define ARCO_STACK_BYTES (32u * 1024u)
#define ARCO_NAME_MAX 32u
#define ARCO_MODULE_MAX 8u
#define ARCO_POOL_BYTES (8u * 1024u * 1024u)
#define ARK_DRIVER_API 1u

typedef struct __attribute__((packed)) {
    uint8_t magic[8];        /* "ARCO1\0\0\0" */
    char name[ARCO_NAME_MAX];/* [a-z0-9_-], unique per manifest */
    uint32_t api;            /* must equal ARK_DRIVER_API */
    uint32_t version;        /* major<<16 | minor<<8 | patch */
    uint64_t image_size;     /* bytes after this header, 1..ARCO_IMAGE_MAX */
    uint64_t entry_off;      /* offset of arco_entry inside the image */
    uint64_t data_off;       /* first writable section, 4 KiB aligned,
                                0 or image_size when the image is read-only */
    uint64_t bss_size;       /* extra zeroed RW bytes, multiple of 4096 */
    uint32_t image_crc32;    /* crc32 over the image bytes */
    uint32_t header_crc32;   /* crc32 over this header with this field zero */
    uint8_t image_sha256[32];
    uint8_t reserved[136];   /* must be zero */
} ArcoHeader;

enum { ARCO_OP_INIT = 1, ARCO_OP_DEINIT = 2 };

/* NIC operations a network driver installs with host->net_attach. The struct
 * is copied at attach time; its function pointers must target the calling
 * module's own mapped image and stay valid until net_detach or module
 * removal. send/receive run on the network poll loop's stack: they must be
 * bounded, non-blocking and safe to call whenever the module is LOADED.
 * receive is pull-model: 0 bytes means no frame is waiting. */
typedef struct {
    int (*send)(const uint8_t *frame, uint32_t length);      /* nonzero = queued */
    int (*link)(void);                                       /* nonzero = link up */
    uint32_t (*receive)(uint8_t *frame, uint32_t capacity);  /* bytes, 0 = none */
    void (*read_mac)(uint8_t out[6]);
} ArkNetOps;

/* Function table handed to the driver at INIT. Every entry is a kernel
 * function chosen for a narrow job; there is no access to user pointers,
 * process state or page tables. Order is ABI: append only. */
typedef struct {
    uint64_t (*millis)(void);
    uint64_t (*ticks)(void);
    void (*log)(const char *text);          /* bounded serial line, "[drv] " prefixed */
    void *(*alloc)(size_t bytes);           /* kernel heap; returns NULL past limits */
    void (*free)(void *pointer);
    void *(*map_mmio)(uint64_t physical, uint64_t bytes); /* supervisor alias, or NULL */
    unsigned (*pci_count)(void);
    /* pci_device copies the record of index n into out (16 dwords); false past end. */
    int (*pci_device)(unsigned index, uint32_t out[10]);
    uint32_t (*pci_read)(uint32_t bdf, unsigned offset);
    uint8_t (*pci_read8)(uint32_t bdf, unsigned offset);
    void (*pci_write16)(uint32_t bdf, unsigned offset, uint16_t value);
    void (*pci_command)(uint32_t bdf, uint16_t bits);
    int (*pci_find)(uint16_t vendor, uint16_t device, uint32_t *bdf);
    int (*pci_bar_base)(uint32_t bdf, unsigned bar, uint64_t *base);
    /* device_register copies the caller's ArkDeviceInfo; returns node index or <0. */
    int (*device_register)(const void *info);
    int (*device_set_state)(uint32_t index, uint32_t flags, uint32_t state);
    int (*device_add_counters)(uint32_t index, uint64_t rx, uint64_t tx,
                               uint64_t ops, uint64_t errors);
    int (*device_notify)(uint32_t index);
    /* One poll callback per module; called on the module stack, must return. */
    int (*device_register_poll)(void (*poll)(void));
    /* phys_of translates a VA inside this module's own image or stack window
     * into the physical DMA address of its backing kernel-pool page; 0 when
     * the VA is not module-owned. Module window VAs themselves are never
     * DMA addresses. */
    uint64_t (*phys_of)(const void *va);
    /* net_attach binds the module's ArkNetOps as the machine NIC (one NIC at
     * a time; -16 when already bound). Callable only from arco_entry, where
     * the loader knows the owning slot; the copy keeps module ownership. */
    int (*net_attach)(const void *ops);
    /* net_detach unbinds this module's NIC ops; the network stack stops
     * calling them immediately. The loader also force-detaches on removal. */
    void (*net_detach)(void);
    /* irq_attach binds isr to one ISA line (0..15). The kernel checks isr
     * against the calling module's RX code window, allocates delivery for that
     * line (IOAPIC redirection entry when the MADT lists one, else 8259 PIC
     * IMR), and runs isr in interrupt context on the module stack before EOI.
     * The ISR must be bounded, must not allocate, block, log or call host
     * services; it only acknowledges the device and records state the poll
     * callback or net ops consume. One owner per line; kernel-owned lines
     * (timer, PS/2) and lines already claimed return -16; a missing IOAPIC
     * pin returns -19. Callable only from arco_entry; removal releases the
     * RTE / masks the PIC line. Drivers that cannot attach must not claim
     * interrupt success — poll-only cannot pass the IOAPIC smoke gate.
     * MSI/MSI-X is still TODO. */
    int (*irq_attach)(uint32_t irq, void (*isr)(void));
    uint32_t reserved[5];                   /* zero; future entries appended here */
} ArkDriverHost;

/* Entry point every .arco image exports (symbol arco_entry). op is
 * ARCO_OP_INIT or ARCO_OP_DEINIT; returns 0 on success, <0 rejects the load.
 * The host pointer remains valid for the module's whole lifetime. */
typedef int64_t (*ArcoEntry)(const ArkDriverHost *host, uint32_t op);

#endif
