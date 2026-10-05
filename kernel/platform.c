/* ArkOS x86_64 platform. Target: QEMU PC, standard VGA, i8042 PS/2.
 * No firmware calls are made after GRUB hands control to the kernel.
 */
#include "ark.h"
#include "virtio_input.h"
#include "process.h"
#include "device.h"
#include "pci.h"
#include "module.h"
#include "smp.h"

/* Diagnostics can link platform without the process subsystem. */
extern InterruptFrame *process_on_interrupt(InterruptFrame *) __attribute__((weak));
extern void process_kernel_enter(void) __attribute__((weak));
extern void process_kernel_leave(void) __attribute__((weak));
void interrupt_enter(void) {
    if (process_kernel_enter)
        process_kernel_enter();
}
void interrupt_leave(void) {
    if (process_kernel_leave)
        process_kernel_leave();
}
typedef struct {
    uint64_t first, last;
} RAMRange;
static RAMRange ram_ranges[128];
static unsigned ram_range_count;
static uint8_t acpi_rsdp[36];
static const void *ucode_blob;
static size_t ucode_bytes;
const void *platform_microcode(size_t *bytes) {
    *bytes = ucode_bytes;
    return ucode_blob;
}
const uint8_t *platform_acpi_rsdp(void) {
    return acpi_rsdp[0] ? acpi_rsdp : 0;
}
extern void platform_smp_eoi(void) __attribute__((weak));
/* IOAPIC router (kernel/ioapic.c). Weak so diagnostic fixtures that link
 * platform.c alone keep the legacy 8259 path. */
extern bool ioapic_init(void) __attribute__((weak));
extern int ioapic_route(unsigned) __attribute__((weak));
extern void ioapic_release(unsigned) __attribute__((weak));
extern void ioapic_mask(unsigned) __attribute__((weak));
extern void ioapic_unmask(unsigned) __attribute__((weak));
extern void ioapic_eoi(void) __attribute__((weak));
extern unsigned ioapic_dump(const char *) __attribute__((weak));
extern void ioapic_describe(unsigned, char *, size_t) __attribute__((weak));
/* true once ISA lines are delivered by an IOAPIC; the 8259s stay masked. */
static bool irq_apic_mode;
bool platform_ram_range(uint64_t physical, uint64_t bytes) {
    if (!bytes || physical > UINT64_MAX - bytes)
        return false;
    /* EFI may describe adjacent usable allocations as distinct entries. Cover
     * their union, accepting no gap and requiring no firmware sorting order. */
    uint64_t end = physical + bytes;
    while (physical < end) {
        uint64_t next = physical;
        for (unsigned i = 0; i < ram_range_count; i++)
            if (physical >= ram_ranges[i].first && physical < ram_ranges[i].last &&
                ram_ranges[i].last > next)
                next = ram_ranges[i].last;
        if (next == physical)
            return false;
        physical = next;
    }
    return true;
}

static inline void out8(uint16_t port, uint8_t value) {
    __asm__ volatile("outb %0,%1" : : "a"(value), "Nd"(port));
}
static inline uint8_t in8(uint16_t port) {
    uint8_t value;
    __asm__ volatile("inb %1,%0" : "=a"(value) : "Nd"(port));
    return value;
}
static inline void out16(uint16_t port, uint16_t value) {
    __asm__ volatile("outw %0,%1" : : "a"(value), "Nd"(port));
}
static inline void io_wait(void) {
    out8(0x80, 0);
}
static inline uint64_t irq_save(void) {
#ifdef ARK_INPUT_TEST
    return 0;
#else
    uint64_t flags;
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(flags) : : "memory");
    return flags;
#endif
}
static inline void irq_restore(uint64_t flags) {
#ifdef ARK_INPUT_TEST
    (void)flags;
#else
    if (flags & (1u << 9))
        __asm__ volatile("sti" : : : "memory");
#endif
}

static void serial_byte(char c) {
    /* Bounded polling also allows a machine without COM1 to boot. */
    unsigned tries = 100000;
    while (!(in8(0x3fd) & 0x20) && --tries) {
    }
    if (tries)
        out8(0x3f8, (uint8_t)c);
}
void serial_write(const char *s) {
    while (*s) {
        if (*s == '\n')
            serial_byte('\r');
        serial_byte(*s++);
    }
}
static void serial_number(uint64_t value) {
    char buffer[24];
    uint_to_str(value, buffer);
    serial_write(buffer);
}
static void serial_hex(uint64_t value) {
    const char *digits = "0123456789abcdef";
    char buffer[19];
    buffer[0] = '0';
    buffer[1] = 'x';
    for (int i = 0; i < 16; ++i)
        buffer[2 + i] = digits[(value >> (60 - i * 4)) & 15];
    buffer[18] = 0;
    serial_write(buffer);
}
static __attribute__((noreturn)) void halt_forever(void) {
    for (;;)
        __asm__ volatile("cli; hlt" : : : "memory");
}
extern void kernel_panic(const char *, const InterruptFrame *) __attribute__((weak, noreturn));
extern void kernel_panic_display_init(const BootInfo *) __attribute__((weak));
static __attribute__((noreturn)) void panic(const char *reason) {
    if (kernel_panic)
        kernel_panic(reason, 0);
    serial_write("[fatal] ");
    serial_write(reason);
    serial_write("\n");
    halt_forever();
}

typedef struct __attribute__((packed)) {
    uint32_t type, size;
} MBTag;
typedef struct __attribute__((packed)) {
    uint32_t type, size;
    uint64_t address;
    uint32_t pitch, width, height;
    uint8_t bpp, kind;
    uint16_t reserved;
    uint8_t red_pos, red_size, green_pos, green_size, blue_pos, blue_size;
} MBFramebuffer;
typedef struct __attribute__((packed)) {
    uint32_t type, size, entry_size, entry_version;
} MBMemoryMap;
typedef struct __attribute__((packed)) {
    uint64_t base, length;
    uint32_t type, reserved;
} MBMemoryEntry;

static void parse_multiboot(uint32_t magic, uint32_t address, BootInfo *info) {
    if (magic != 0x36d76289)
        panic("Missing Multiboot2 boot magic.");
    if (!address || (address & 7) || address > 0xfffffff7u)
        panic("Invalid Multiboot2 information address.");
    const uint8_t *base = (const uint8_t *)(uintptr_t)address;
    uint32_t total = *(const uint32_t *)base;
    if (total < 16 || total > 16 * 1024 * 1024 || (total & 7) ||
        (uint64_t)address + total > 0x100000000ull)
        panic("Invalid Multiboot2 information length.");
    memset(info, 0, sizeof(*info));
    bool found_end = false, found_fb = false, found_map = false;
    uint64_t usable = 0;
    for (uint32_t offset = 8; offset <= total - 8;) {
        const MBTag *tag = (const MBTag *)(base + offset);
        if (tag->size < 8 || tag->size > total - offset)
            panic("Malformed Multiboot2 tag.");
        if (tag->type == 0) {
            if (tag->size != 8)
                panic("Malformed Multiboot2 end tag.");
            found_end = true;
            break;
        }
        if (tag->type == 3 && tag->size >= 17) {
            uint32_t mod[4];
            memcpy(mod, tag, 16);
            const char *name = (const char *)tag + 16;
            size_t chars = tag->size - 16;
            if (chars >= 12 && !strncmp(name, "intel-ucode", 11) && !name[11] && mod[3] > mod[2] &&
                mod[3] - mod[2] <= 16 * 1024 * 1024) {
                ucode_blob = (const void *)(uintptr_t)mod[2];
                ucode_bytes = mod[3] - mod[2];
            }
        }
        if ((tag->type == 14 || tag->type == 15) && tag->size >= 28) {
            unsigned len = tag->type == 15 && tag->size >= 44 ? 36 : 20;
            if (len == 36 || !acpi_rsdp[0])
                memcpy(acpi_rsdp, (const uint8_t *)tag + 8, len);
        }
        if (tag->type == 8) {
            const MBFramebuffer *fb = (const MBFramebuffer *)tag;
            if (tag->size < sizeof(*fb) || fb->kind != 1 || fb->bpp != 32)
                panic("A 32-bit RGB framebuffer is required; use QEMU -vga std.");
            if (fb->width < 640 || fb->height < 480 || fb->width > 4096 || fb->height > 2160 ||
                fb->pitch < fb->width * 4 || (fb->pitch & 3))
                panic("Unsupported framebuffer geometry or pitch.");
            uint64_t bytes = (uint64_t)fb->pitch * fb->height;
            if (!fb->address || fb->address >= 0x100000000ull ||
                bytes > 0x100000000ull - fb->address)
                panic("Framebuffer must fit below the 4 GiB identity-map limit.");
            if (fb->red_size != 8 || fb->green_size != 8 || fb->blue_size != 8 ||
                fb->red_pos > 24 || fb->green_pos > 24 || fb->blue_pos > 24)
                panic("Unsupported framebuffer channel layout.");
            uint32_t rm = 255u << fb->red_pos, gm = 255u << fb->green_pos,
                     bm = 255u << fb->blue_pos;
            if ((rm & gm) || (rm & bm) || (gm & bm))
                panic("Overlapping framebuffer color channels.");
            info->framebuffer = fb->address;
            info->width = fb->width;
            info->height = fb->height;
            info->pitch = fb->pitch;
            info->bpp = fb->bpp;
            info->red_pos = fb->red_pos;
            info->red_size = fb->red_size;
            info->green_pos = fb->green_pos;
            info->green_size = fb->green_size;
            info->blue_pos = fb->blue_pos;
            info->blue_size = fb->blue_size;
            found_fb = true;
        } else if (tag->type == 6) {
            const MBMemoryMap *map = (const MBMemoryMap *)tag;
            if (tag->size < sizeof(*map) || map->entry_size < sizeof(MBMemoryEntry) ||
                map->entry_version != 0 || (tag->size - sizeof(*map)) % map->entry_size)
                panic("Malformed Multiboot2 memory map.");
            for (uint32_t at = sizeof(*map); at < tag->size; at += map->entry_size) {
                const MBMemoryEntry *entry = (const MBMemoryEntry *)((const uint8_t *)tag + at);
                if (entry->type == 1) {
                    if (entry->base > UINT64_MAX - entry->length)
                        panic("Multiboot2 RAM range overflow.");
                    if (ram_range_count < 128)
                        ram_ranges[ram_range_count++] =
                            (RAMRange){entry->base, entry->base + entry->length};
                    if (entry->length > UINT64_MAX - usable)
                        panic("Multiboot2 memory size overflow.");
                    usable += entry->length;
                }
            }
            found_map = true;
        }
        uint32_t next = (tag->size + 7u) & ~7u;
        if (next > total - offset)
            panic("Unaligned Multiboot2 tag boundary.");
        offset += next;
    }
    if (!found_end)
        panic("Multiboot2 information has no end tag.");
    if (!found_fb)
        panic("GRUB did not supply a graphics framebuffer.");
    if (!found_map)
        serial_write("[boot] Warning: no physical memory map supplied.\n");
    info->memory_mib = usable / (1024 * 1024);
    serial_write("[boot] Framebuffer ");
    serial_number(info->width);
    serial_write("x");
    serial_number(info->height);
    serial_write("x32 at ");
    serial_hex(info->framebuffer);
    serial_write("; usable RAM ");
    serial_number(info->memory_mib);
    serial_write(" MiB\n");
}

typedef struct __attribute__((packed)) {
    uint16_t low, selector;
    uint8_t ist, attributes;
    uint16_t middle;
    uint32_t high, reserved;
} IDTEntry;
typedef struct __attribute__((packed)) {
    uint16_t limit;
    uint64_t base;
} IDTPointer;
static IDTEntry idt[256] __attribute__((aligned(16)));
extern void *isr_stub_table[256];
static void init_idt(void) {
    for (unsigned i = 0; i < 256; ++i) {
        uint64_t target = (uint64_t)(uintptr_t)isr_stub_table[i];
        idt[i] = (IDTEntry){(uint16_t)target,         8, 0, 0x8e, (uint16_t)(target >> 16),
                            (uint32_t)(target >> 32), 0};
    }
    IDTPointer descriptor = {(uint16_t)(sizeof(idt) - 1), (uint64_t)(uintptr_t)idt};
    __asm__ volatile("lidt %0" : : "m"(descriptor));
}
void platform_load_process_idt(void) {
    IDTPointer p = {sizeof(idt) - 1, (uintptr_t)idt};
    __asm__ volatile("lidt %0" ::"m"(p) : "memory");
}
void platform_enable_process_gates(void) {
    idt[128].attributes = 0xee; /* Ring-3 INT80, interrupt gate (IF cleared). */
    idt[8].ist = 1;             /* Dedicated TSS IST1 double-fault stack. */
}
static void init_pic(void) {
    out8(0x20, 0x11);
    io_wait();
    out8(0xa0, 0x11);
    io_wait();
    out8(0x21, 0x20);
    io_wait();
    out8(0xa1, 0x28);
    io_wait();
    out8(0x21, 4);
    io_wait();
    out8(0xa1, 2);
    io_wait();
    out8(0x21, 1);
    io_wait();
    out8(0xa1, 1);
    io_wait();
    out8(0x21, 0xff);
    out8(0xa1, 0xff);
}
/* Runtime mask control for ISA lines 0..15. In IOAPIC mode the line's RTE
 * mask bit is toggled (only for a routed line) and the 8259s are never
 * touched; otherwise the legacy PIC IMR is used. */
void platform_irq_mask(unsigned irq) {
    if (irq_apic_mode) {
        ioapic_mask(irq);
        return;
    }
    if (irq < 8)
        out8(0x21, (uint8_t)(in8(0x21) | (1u << irq)));
    else if (irq < 16)
        out8(0xa1, (uint8_t)(in8(0xa1) | (1u << (irq - 8))));
}
void platform_irq_unmask(unsigned irq) {
    if (irq_apic_mode) {
        ioapic_unmask(irq);
        return;
    }
    if (irq < 8)
        out8(0x21, (uint8_t)(in8(0x21) & ~(1u << irq)));
    else if (irq < 16)
        out8(0xa1, (uint8_t)(in8(0xa1) & ~(1u << (irq - 8))));
}
/* Allocate delivery for one line (irq_attach): an IOAPIC RTE (vector
 * 32 + irq, MADT override polarity/trigger) or the PIC IMR bit. Release is
 * the exact inverse, so unload/reload leaves no stale or duplicate entry. */
int platform_irq_route(unsigned irq) {
    if (irq >= 16)
        return -22;
    if (irq_apic_mode) {
        int rc = ioapic_route(irq);
        if (!rc)
            ioapic_dump("after route+");
        return rc;
    }
    platform_irq_unmask(irq);
    return 0;
}
void platform_irq_release(unsigned irq) {
    if (irq >= 16)
        return;
    if (irq_apic_mode) {
        ioapic_release(irq);
        ioapic_dump("after route-");
        return;
    }
    platform_irq_mask(irq);
}
bool platform_irq_apic_mode(void) {
    return irq_apic_mode;
}
/* Short delivery text for diagnostics ("ioapic0 gsi11 vec43 level/high"). */
void platform_irq_describe(unsigned irq, char *out, size_t cap) {
    if (!cap)
        return;
    out[0] = 0;
    if (irq >= 16)
        return;
    if (irq_apic_mode)
        ioapic_describe(irq, out, cap);
    else
        strcopy(out, "8259 pic", cap);
}
static volatile uint64_t ticks;
static volatile uint64_t milliseconds;
extern void clock_init(void) __attribute__((weak));
extern uint64_t clock_millis(void) __attribute__((weak));
#define EVENT_CAP 1024u
static Event events[EVENT_CAP];
static bool event_edge[EVENT_CAP];
static volatile unsigned event_head, event_tail;
static bool relative_pointer_enabled = true, mouse_wait_release;
static uint8_t last_mouse_buttons;
static uint8_t mouse_packet[4], mouse_at, mouse_length = 3, mouse_id;
static uint64_t mouse_byte_tick;
static bool overflow_mouse_pending;
static Event overflow_mouse_state;
static bool same_direction(int a, int b) {
    return !a || !b || (a < 0) == (b < 0);
}
static void remove_event(unsigned at) {
    unsigned last = (event_head + EVENT_CAP - 1) % EVENT_CAP;
    for (unsigned i = at; i != last; i = (i + 1) % EVENT_CAP) {
        unsigned next = (i + 1) % EVENT_CAP;
        events[i] = events[next];
        event_edge[i] = event_edge[next];
    }
    event_head = last;
}
static void enqueue(Event event) {
    bool edge = false;
    if (event.type == EV_MOUSE) {
        if (!relative_pointer_enabled)
            return;
        edge = event.buttons != last_mouse_buttons;
        last_mouse_buttons = event.buttons;
        if (overflow_mouse_pending)
            overflow_mouse_state.buttons = event.buttons;
        if (!edge && !event.dx && !event.dy)
            return;
        /* Never merge into a press/release. Otherwise a later motion would
         * change the location of the original click. Do not combine changes
         * in direction either: clipping a path at a screen edge matters. */
        if (!edge && event_head != event_tail) {
            unsigned previous = (event_head + EVENT_CAP - 1) % EVENT_CAP;
            Event *last = &events[previous];
            int64_t x = (int64_t)last->dx + event.dx, y = (int64_t)last->dy + event.dy;
            if (last->type == EV_MOUSE && !event_edge[previous] && last->buttons == event.buttons &&
                same_direction(last->dx, event.dx) && same_direction(last->dy, event.dy) &&
                x >= INT32_MIN && x <= INT32_MAX && y >= INT32_MIN && y <= INT32_MAX) {
                last->dx = (int)x;
                last->dy = (int)y;
                return;
            }
        }
    }
    unsigned next = (event_head + 1) % EVENT_CAP;
    if (next == event_tail) {
        /* Prefer discarding a redundant motion over a key or button edge. */
        unsigned discard = event_tail;
        while (discard != event_head && (events[discard].type != EV_MOUSE || event_edge[discard]))
            discard = (discard + 1) % EVENT_CAP;
        if (discard != event_head)
            remove_event(discard);
        else {
            /* A bounded queue cannot retain an unbounded stalled stream.
             * Keep all existing edges and recover the latest button state
             * after draining, so overload cannot leave a button stuck. */
            if (event.type == EV_MOUSE) {
                overflow_mouse_state = (Event){EV_MOUSE, 0, 0, 0, event.buttons};
                overflow_mouse_pending = true;
            }
            return;
        }
        next = (event_head + 1) % EVENT_CAP;
    }
    events[event_head] = event;
    event_edge[event_head] = edge;
    __asm__ volatile("" : : : "memory");
    event_head = next;
}
bool platform_next_event(Event *event) {
    uint64_t flags = irq_save();
    bool available = event_tail != event_head;
    if (available) {
        *event = events[event_tail];
        event_tail = (event_tail + 1) % EVENT_CAP;
    } else if (overflow_mouse_pending) {
        *event = overflow_mouse_state;
        overflow_mouse_pending = false;
        available = true;
    }
    irq_restore(flags);
    return available;
}
void platform_set_relative_pointer_enabled(bool enabled) {
    uint64_t flags = irq_save();
    if (enabled != relative_pointer_enabled) {
        /* Keep keyboard ordering intact while removing packets accumulated
         * under the old pointer source. Both takeover AND release purge. */
        unsigned write = event_tail;
        for (unsigned read = event_tail; read != event_head; read = (read + 1) % EVENT_CAP)
            if (events[read].type != EV_MOUSE) {
                events[write] = events[read];
                event_edge[write] = event_edge[read];
                write = (write + 1) % EVENT_CAP;
            }
        event_head = write;
        overflow_mouse_pending = false;
        mouse_at = 0;
        last_mouse_buttons = 0;
        mouse_wait_release = enabled;
        relative_pointer_enabled = enabled;
    }
    irq_restore(flags);
}
uint64_t platform_millis(void) {
    uint64_t ms = clock_millis ? clock_millis() : UINT64_MAX;
    return ms == UINT64_MAX ? milliseconds : ms;
}
uint64_t platform_ticks(void) {
    return platform_millis() / 10;
}
void platform_idle(void) {
    /* CLI/check/STI/HLT prevents sleeping past an already queued input event.
     * The CPU defers an IRQ until after the instruction following STI.
     */
    __asm__ volatile("cli" : : : "memory");
    if (event_head == event_tail && !overflow_mouse_pending)
        __asm__ volatile("sti; hlt" : : : "memory");
    else
        __asm__ volatile("sti" : : : "memory");
}

static const unsigned char key_plain[128] = {
    [0x02] = '1',  [0x03] = '2', [0x04] = '3', [0x05] = '4', [0x06] = '5',  [0x07] = '6',
    [0x08] = '7',  [0x09] = '8', [0x0a] = '9', [0x0b] = '0', [0x0c] = '-',  [0x0d] = '=',
    [0x10] = 'q',  [0x11] = 'w', [0x12] = 'e', [0x13] = 'r', [0x14] = 't',  [0x15] = 'y',
    [0x16] = 'u',  [0x17] = 'i', [0x18] = 'o', [0x19] = 'p', [0x1a] = '[',  [0x1b] = ']',
    [0x1e] = 'a',  [0x1f] = 's', [0x20] = 'd', [0x21] = 'f', [0x22] = 'g',  [0x23] = 'h',
    [0x24] = 'j',  [0x25] = 'k', [0x26] = 'l', [0x27] = ';', [0x28] = '\'', [0x29] = '`',
    [0x2b] = '\\', [0x2c] = 'z', [0x2d] = 'x', [0x2e] = 'c', [0x2f] = 'v',  [0x30] = 'b',
    [0x31] = 'n',  [0x32] = 'm', [0x33] = ',', [0x34] = '.', [0x35] = '/',  [0x37] = '*',
    [0x39] = ' ',  [0x4a] = '-', [0x4e] = '+'};
static const unsigned char key_shifted[128] = {
    [0x02] = '!', [0x03] = '@', [0x04] = '#', [0x05] = '$', [0x06] = '%', [0x07] = '^',
    [0x08] = '&', [0x09] = '*', [0x0a] = '(', [0x0b] = ')', [0x0c] = '_', [0x0d] = '+',
    [0x1a] = '{', [0x1b] = '}', [0x27] = ':', [0x28] = '"', [0x29] = '~', [0x2b] = '|',
    [0x33] = '<', [0x34] = '>', [0x35] = '?'};
/* PS/2 and each VirtIO keyboard own their edge state. A release/reset on one
 * device must not cancel Shift held on another (':' used to become ';'). */
#define KEYBOARD_SOURCES 9u
typedef struct {
    uint8_t shift, ctrl, alt;
    bool caps_down, extended;
    unsigned pause_bytes;
} KeyboardState;
static KeyboardState keyboards[KEYBOARD_SOURCES];
static bool caps_lock;
static void keyboard_source_byte(unsigned source, uint8_t byte) {
    if (source >= KEYBOARD_SOURCES)
        return;
    KeyboardState *state = &keyboards[source];
    if (state->pause_bytes) {
        --state->pause_bytes;
        return;
    }
    if (byte == 0xe1) {
        state->pause_bytes = 5;
        return;
    }
    if (byte == 0xe0) {
        state->extended = true;
        return;
    }
    bool ext = state->extended, released = (byte & 0x80) != 0;
    state->extended = false;
    uint8_t sc = byte & 0x7f;
    if (!ext && (sc == 0x2a || sc == 0x36)) {
        uint8_t bit = sc == 0x2a ? 1 : 2;
        if (released)
            state->shift &= (uint8_t)~bit;
        else
            state->shift |= bit;
        return;
    }
    if (sc == 0x38) {
        uint8_t bit = ext ? 2 : 1;
        if (released)
            state->alt &= (uint8_t)~bit;
        else
            state->alt |= bit;
        return;
    }
    if (sc == 0x1d) {
        uint8_t bit = ext ? 2 : 1;
        if (released)
            state->ctrl &= (uint8_t)~bit;
        else
            state->ctrl |= bit;
        return;
    }
    if (!ext && sc == 0x3a) {
        if (!released && !state->caps_down)
            caps_lock = !caps_lock;
        state->caps_down = !released;
        return;
    }
    if (released)
        return;
    unsigned shift_bits = 0, ctrl_bits = 0, alt_bits = 0;
    for (unsigned i = 0; i < KEYBOARD_SOURCES; i++) {
        shift_bits |= keyboards[i].shift;
        ctrl_bits |= keyboards[i].ctrl;
        alt_bits |= keyboards[i].alt;
    }
    int key = 0;
    if (sc == 0x1c)
        key = KEY_ENTER;
    else if (ext) {
        switch (sc) {
        case 0x48:
            key = KEY_UP;
            break;
        case 0x50:
            key = KEY_DOWN;
            break;
        case 0x4b:
            key = KEY_LEFT;
            break;
        case 0x4d:
            key = KEY_RIGHT;
            break;
        case 0x49:
            key = KEY_PAGE_UP;
            break;
        case 0x51:
            key = KEY_PAGE_DOWN;
            break;
        case 0x47:
            key = KEY_HOME;
            break;
        case 0x4f:
            key = KEY_END;
            break;
        case 0x5b:
        case 0x5c:
            key = KEY_LAUNCHER;
            break;
        case 0x53:
            key = KEY_DELETE;
            break;
        case 0x35:
            key = '/';
            break;
        default:
            break;
        }
    } else {
        switch (sc) {
        case 0x01:
            key = KEY_ESCAPE;
            break;
        case 0x0e:
            key = KEY_BACKSPACE;
            break;
        case 0x0f:
            key = KEY_TAB;
            break;
        case 0x3b:
            key = KEY_F1;
            break;
        case 0x3c:
            key = KEY_F2;
            break;
        case 0x3d:
            key = KEY_F3;
            break;
        case 0x3e:
            key = KEY_F4;
            break;
        case 0x3f:
            key = KEY_LAUNCHER;
            break;
        default:
            key = key_plain[sc];
            if (key >= 'a' && key <= 'z') {
                if (ctrl_bits)
                    key = key - 'a' + 1;
                else if ((shift_bits != 0) != caps_lock)
                    key -= 'a' - 'A';
            } else if (shift_bits && key_shifted[sc])
                key = key_shifted[sc];
            break;
        }
    }
    if (alt_bits && key == KEY_F4)
        key = 23;
    if (key)
        enqueue((Event){EV_KEY, key, 0, 0, 0});
}
static void keyboard_byte(uint8_t byte) {
    keyboard_source_byte(0, byte);
}
/* VirtIO key codes match set-1 in the base range; extended keys are explicit. */
void platform_keyboard_device_input(unsigned source, unsigned code, int value) {
    if (!source || source >= KEYBOARD_SOURCES || value < 0 || value > 2)
        return;
    unsigned sc = code;
    bool ext = false;
    if (code >= 96) {
        ext = true;
        switch (code) {
        case 96:
            sc = 0x1c;
            break;
        case 97:
            sc = 0x1d;
            break;
        case 100:
            sc = 0x38;
            break;
        case 102:
            sc = 0x47;
            break;
        case 103:
            sc = 0x48;
            break;
        case 104:
            sc = 0x49;
            break;
        case 105:
            sc = 0x4b;
            break;
        case 106:
            sc = 0x4d;
            break;
        case 107:
            sc = 0x4f;
            break;
        case 108:
            sc = 0x50;
            break;
        case 109:
            sc = 0x51;
            break;
        case 111:
            sc = 0x53;
            break;
        case 125:
            sc = 0x5b;
            break;
        case 126:
            sc = 0x5c;
            break;
        default:
            return;
        }
    } else if (code > 83)
        return;
    if (ext)
        keyboard_source_byte(source, 0xe0);
    keyboard_source_byte(source, (uint8_t)(sc | (value ? 0 : 0x80)));
}
void platform_keyboard_input(unsigned code, int value) {
    platform_keyboard_device_input(1, code, value);
}
static void mouse_byte(uint8_t byte) {
    /* A lost byte must not turn the next report's header into a coordinate. */
    if (mouse_at && ticks - mouse_byte_tick > 3)
        mouse_at = 0;
    mouse_byte_tick = ticks;
    if (!mouse_at && !(byte & 8))
        return; /* Packet synchronization bit. */
    mouse_packet[mouse_at++] = byte;
    if (mouse_at != mouse_length)
        return;
    mouse_at = 0;
    uint8_t flags = mouse_packet[0];
    int dx = (int)mouse_packet[1] - ((flags & 0x10) ? 256 : 0);
    int dy = (int)mouse_packet[2] - ((flags & 0x20) ? 256 : 0);
    /* Overflow represents saturated 9-bit movement, not zero movement. */
    if (flags & 0x40)
        dx = (flags & 0x10) ? -256 : 255;
    if (flags & 0x80)
        dy = (flags & 0x20) ? -256 : 255;
    if (!relative_pointer_enabled)
        return;
    if (mouse_wait_release) {
        if (flags & 7)
            return;
        mouse_wait_release = false;
    }
    /* PS/2 uses positive-up Y; desktop coordinates use positive-down Y. */
    uint8_t buttons = flags & 7;
    if (mouse_id == 4)
        buttons |= (mouse_packet[3] & 0x30) >> 1;
    enqueue((Event){EV_MOUSE, 0, dx, -dy, buttons});
    if (mouse_length == 4) {
        int wheel = mouse_id == 4 ? (int)(mouse_packet[3] & 15) : (int)(int8_t)mouse_packet[3];
        if (mouse_id == 4 && (wheel & 8))
            wheel -= 16;
        if (wheel)
            enqueue((Event){EV_SCROLL, 0, 0, wheel, buttons});
    }
}
static void controller_byte(uint8_t status, uint8_t byte) {
    if (status & 0xc0) {
        if (status & 0x20)
            mouse_at = 0;
        return; /* Restart framing after an auxiliary parity/timeout error. */
    }
    if (status & 0x20)
        mouse_byte(byte);
    else
        keyboard_byte(byte);
}
static void drain_input(void) {
    for (unsigned i = 0; i < 64; i++) {
        uint8_t status = in8(0x64);
        if (!(status & 1))
            break;
        controller_byte(status, in8(0x60));
    }
}
static bool ps2_wait_write(void) {
    for (unsigned i = 0; i < 100000; ++i)
        if (!(in8(0x64) & 2))
            return true;
    return false;
}
static bool ps2_command(uint8_t command) {
    if (!ps2_wait_write())
        return false;
    out8(0x64, command);
    return true;
}
static bool ps2_data(uint8_t data) {
    if (!ps2_wait_write())
        return false;
    out8(0x60, data);
    return true;
}
static bool ps2_read(bool auxiliary, uint8_t *value) {
    for (unsigned i = 0; i < 100000; ++i) {
        uint8_t status = in8(0x64);
        if (!(status & 1))
            continue;
        uint8_t byte = in8(0x60);
        if ((status & 0xc0) || ((status & 0x20) != (auxiliary ? 0x20 : 0)))
            continue;
        *value = byte;
        return true;
    }
    return false;
}
static bool ps2_device_command(bool auxiliary, uint8_t value) {
    for (int attempt = 0; attempt < 3; ++attempt) {
        uint8_t response;
        if (auxiliary && !ps2_command(0xd4))
            return false;
        if (!ps2_data(value) || !ps2_read(auxiliary, &response))
            return false;
        if (response == 0xfa)
            return true;
        if (response != 0xfe)
            return false;
    }
    return false;
}
static bool mouse_rate(uint8_t rate) {
    return ps2_device_command(true, 0xf3) && ps2_device_command(true, rate);
}
static void mouse_extensions(void) {
    uint8_t id = 0;
    mouse_length = 3;
    mouse_id = 0;
    if (mouse_rate(200) && mouse_rate(100) && mouse_rate(80) && ps2_device_command(true, 0xf2) &&
        ps2_read(true, &id) && (id == 3 || id == 4)) {
        mouse_id = id;
        mouse_length = 4;
        if (id == 3 && mouse_rate(200) && mouse_rate(200) && mouse_rate(80) &&
            ps2_device_command(true, 0xf2) && ps2_read(true, &id) && id == 4)
            mouse_id = 4;
    }
    (void)mouse_rate(200);
    mouse_at = 0;
}
static void init_ps2(void) {
    ps2_command(0xad);
    ps2_command(0xa7);
    for (unsigned i = 0; i < 256 && (in8(0x64) & 1); ++i)
        (void)in8(0x60);
    uint8_t config = 0;
    if (!ps2_command(0x20) || !ps2_read(false, &config)) {
        serial_write("[input] Warning: i8042 controller unavailable.\n");
        return;
    }
    /* Translation converts keyboard set 2 into the set 1 decoded above. */
    config = (uint8_t)((config | 0x40) & ~(0x01 | 0x02));
    if (!ps2_command(0x60) || !ps2_data(config))
        return;
    ps2_command(0xae);
    ps2_command(0xa8);
    bool keyboard = ps2_device_command(false, 0xf6) && ps2_device_command(false, 0xf4);
    bool mouse = ps2_device_command(true, 0xf6);
    if (mouse) {
        /* Native relative fallback: standard three-byte stream, 1:1 scaling.
         * Absolute virtio tablet is preferred when available. */
        (void)ps2_device_command(true, 0xe6);
        mouse_extensions();
        (void)ps2_device_command(true, 0xea);
        mouse = ps2_device_command(true, 0xf4);
    }
    config = (uint8_t)((config | 0x43) & ~0x30);
    ps2_command(0x60);
    ps2_data(config);
    serial_write(keyboard ? "[input] PS/2 keyboard ready\n"
                          : "[input] Warning: keyboard did not acknowledge setup.\n");
    if (mouse) {
        serial_write("[input] PS/2 packet bytes=");
        serial_number(mouse_length);
        serial_write(" id=");
        serial_number(mouse_id);
        serial_write("\n");
    }
    serial_write(mouse ? "[input] PS/2 mouse ready\n"
                       : "[input] Warning: mouse did not acknowledge setup.\n");
}

InterruptFrame *interrupt_dispatch(InterruptFrame *frame) {
    unsigned vector = (unsigned)frame->vector;
    if (vector == 240 || vector == 48) {
        if (platform_smp_eoi)
            platform_smp_eoi();
        return process_on_interrupt ? process_on_interrupt(frame) : frame;
    }
    if (vector == 255)
        return frame;
    if (process_on_interrupt && (frame->cs & 3) == 3 && (vector < 32 || vector == 128))
        return process_on_interrupt(frame);
    if (vector < 32) {
        static const char *names[32] = {"divide error",
                                        "debug",
                                        "NMI",
                                        "breakpoint",
                                        "overflow",
                                        "bound range",
                                        "invalid opcode",
                                        "device unavailable",
                                        "double fault",
                                        "coprocessor overrun",
                                        "invalid TSS",
                                        "segment not present",
                                        "stack fault",
                                        "general protection",
                                        "page fault",
                                        "reserved",
                                        "x87 fault",
                                        "alignment check",
                                        "machine check",
                                        "SIMD fault",
                                        "virtualization",
                                        "control protection",
                                        "reserved",
                                        "reserved",
                                        "reserved",
                                        "reserved",
                                        "reserved",
                                        "reserved",
                                        "hypervisor",
                                        "VMM communication",
                                        "security",
                                        "reserved"};
        serial_write("[exception] ");
        serial_write(names[vector]);
        serial_write(" vector=");
        serial_number(vector);
        serial_write(" error=");
        serial_hex(frame->error);
        serial_write(" rip=");
        serial_hex(frame->rip);
        if (vector == 14) {
            uint64_t cr2;
            __asm__ volatile("mov %%cr2,%0" : "=r"(cr2));
            serial_write(" cr2=");
            serial_hex(cr2);
        }
        serial_write("\n");
        if (kernel_panic)
            kernel_panic(names[vector], frame);
        halt_forever();
    }
    if (vector < 32 || vector >= 48)
        return frame;
    /* Spurious IRQ7/15 have no matching ISR bit to acknowledge. The 8259s are
     * fully masked in IOAPIC mode, so vectors 39/47 are then real RTEs. */
    if (!irq_apic_mode && (vector == 39 || vector == 47)) {
        uint16_t port = vector == 39 ? 0x20 : 0xa0;
        out8(port, 0x0b);
        if (!(in8(port) & 0x80)) {
            if (vector == 47)
                out8(0x20, 0x20);
            return frame;
        }
    }
    if (vector == 32) {
        ++milliseconds;
        ticks = platform_ticks();
    }
    if (vector == 33 || vector == 44)
        drain_input();
    /* Module-bound lines run their ISR on the module stack; the driver
     * acknowledges its device there, then the EOI completes the cycle: LAPIC
     * EOI for IOAPIC delivery (also clears Remote IRR on level RTEs), PIC EOI
     * on the legacy path. */
    module_irq_dispatch(vector - 32);
    if (irq_apic_mode)
        ioapic_eoi();
    else {
        if (vector >= 40)
            out8(0xa0, 0x20);
        out8(0x20, 0x20);
    }
    if (vector == 32 && process_on_interrupt)
        return process_on_interrupt(frame);
    return frame;
}

void platform_init(uint32_t magic, uint32_t mb_addr, BootInfo *info) {
    __asm__ volatile("cli" : : : "memory");
    /* Install our 64-bit IDT before parsing any boot data. */
    init_idt();
    serial_write("[boot] Long mode active; 64-bit IDT installed\n");
    parse_multiboot(magic, mb_addr, info);
    if (clock_init)
        clock_init();
    if (kernel_panic_display_init)
        kernel_panic_display_init(info);
    init_pic();
    /* Real machines deliver ISA/PCI INTx through the IOAPIC; the 8259 path is
     * kept only when the MADT has no usable IOAPIC (or no BSP LAPIC). */
    if (ioapic_init)
        irq_apic_mode = ioapic_init();
    else
        serial_write("[irq] legacy PIC only (IOAPIC router not linked)\n");
    if (irq_apic_mode) {
        out8(0x21, 0xff);
        out8(0xa1, 0xff);
        serial_write("[irq] 8259 PIC fully masked: IMR master=");
        serial_hex(in8(0x21));
        serial_write(" slave=");
        serial_hex(in8(0xa1));
        serial_write("\n");
    }
    init_ps2();
    device_init();
    pci_init();
    {   /* Fixed platform nodes: real topology only, no invented model string. */
        ArkDeviceInfo node = {0};
        node.bus = ARK_BUS_PLATFORM;
        node.flags = ARK_DEV_PRESENT;
        node.state = ARK_DEV_STATE_OK;
        node.class_id = ARK_DEV_CLASS_PLATFORM;
        node.blocks = info->memory_mib;
        strcopy(node.name, "ArkOS platform", sizeof node.name);
        strcopy(node.driver, "platform", sizeof node.driver);
        uint_to_str(info->memory_mib, node.detail);
        size_t at = strlen(node.detail);
        strcopy(node.detail + at, " MiB RAM", sizeof node.detail - at);
        device_register(&node);
        node.class_id = ARK_DEV_CLASS_SERIAL;
        node.blocks = 0;
        node.detail[0] = 0;
        strcopy(node.name, "COM1 console", sizeof node.name);
        strcopy(node.driver, "serial8250", sizeof node.driver);
        device_register(&node);
        node.class_id = ARK_DEV_CLASS_CPU;
        node.name[0] = 0;
        node.detail[0] = 0;
        uint32_t max = 0x80000000u, a = 0, b, c, d;
        __asm__ volatile("cpuid" : "+a"(max), "=b"(b), "=c"(c), "=d"(d));
        if (max >= 0x80000004u) {
            char brand[52] = {0};
            for (unsigned leaf = 0; leaf < 3; leaf++) {
                a = 0x80000002u + leaf;
                b = c = d = 0;
                __asm__ volatile("cpuid" : "+a"(a), "=b"(b), "=c"(c), "=d"(d));
                uint32_t *out = (uint32_t *)(void *)(brand + leaf * 16);
                out[0] = b;
                out[1] = d;
                out[2] = c;
                out[3] = a;
            }
            for (unsigned i = 0; i < sizeof brand - 1 && brand[i]; i++)
                if (brand[i] == ' ')
                    brand[i] = 0;
            strcopy(node.name, brand[0] ? brand : "x86_64 processor", sizeof node.name);
        } else
            strcopy(node.name, "x86_64 processor", sizeof node.name);
        strcopy(node.driver, "cpuid", sizeof node.driver);
        node.blocks = platform_online_cpus();
        uint_to_str(platform_online_cpus(), node.detail);
        strcopy(node.detail + strlen(node.detail), " online CPU", sizeof node.detail);
        device_register(&node);
    }
    const unsigned divisor = 1193182u / 1000u;
    out8(0x43, 0x36);
    out8(0x40, (uint8_t)divisor);
    out8(0x40, (uint8_t)(divisor >> 8));
    /* IRQ0 timer, IRQ1 keyboard, IRQ2 cascade (PIC only), IRQ12 mouse. */
    if (irq_apic_mode) {
        static const unsigned kernel_lines[] = {0, 1, 12};
        for (unsigned i = 0; i < sizeof kernel_lines / sizeof kernel_lines[0]; i++)
            if (ioapic_route(kernel_lines[i]) < 0) {
                serial_write("[irq] Warning: kernel ISA line ");
                serial_number(kernel_lines[i]);
                serial_write(" not routed\n");
            }
        ioapic_dump("boot");
    } else {
        out8(0x21, 0xf8);
        out8(0xa1, 0xef);
        serial_write("[irq] legacy 8259 PIC delivery: IRQ0/1/2/12 unmasked\n");
    }
    serial_write("[boot] PIT 1000 Hz; legacy clock 100 Hz; interrupts enabled\n");
    __asm__ volatile("sti" : : : "memory");
}

static uint8_t cmos_read(uint8_t reg) {
    out8(0x70, (uint8_t)(0x80 | reg));
    io_wait();
    return in8(0x71);
}
bool platform_datetime(int *year, int *month, int *day, int *h, int *m, int *s, int *weekday) {
    uint64_t flags = irq_save();
    uint8_t first[7] = {0}, second[7] = {0}, format = 2;
    static const uint8_t registers[] = {0, 2, 4, 7, 8, 9, 0x32};
    bool valid = false;
    for (unsigned attempt = 0; attempt < 8 && !valid; attempt++) {
        unsigned spins = 100000;
        while ((cmos_read(0x0a) & 0x80) && --spins) {
        }
        if (!spins)
            break;
        for (unsigned i = 0; i < 7; i++)
            first[i] = cmos_read(registers[i]);
        format = cmos_read(0x0b);
        if (cmos_read(0x0a) & 0x80)
            continue;
        for (unsigned i = 0; i < 7; i++)
            second[i] = cmos_read(registers[i]);
        valid = !(cmos_read(0x0a) & 0x80) && !memcmp(first, second, sizeof first);
    }
    out8(0x70, 0x0d);
    irq_restore(flags);
    *year = *month = *day = *h = *m = *s = *weekday = 0;
    if (!valid)
        return false;
    bool pm = (first[2] & 0x80) != 0;
    first[2] &= 0x7f;
    if (!(format & 4))
        for (unsigned i = 0; i < 7; i++)
            first[i] = (uint8_t)((first[i] & 15) + (first[i] >> 4) * 10);
    if (!(format & 2))
        first[2] = (uint8_t)(first[2] % 12 + (pm ? 12 : 0));
    *s = first[0];
    *m = first[1];
    *h = first[2];
    *day = first[3];
    *month = first[4];
    *year = (first[6] >= 19 && first[6] <= 99 ? first[6] : 20) * 100 + first[5];
    if (*month < 1 || *month > 12 || *day < 1 || *day > 31 || *h > 23 || *m > 59 || *s > 59)
        return false;
    static const int offsets[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
    int y = *year - (*month < 3);
    *weekday = (y + y / 4 - y / 100 + y / 400 + offsets[*month - 1] + *day) % 7;
    return true;
}
void platform_time(int *h, int *m, int *s) {
    int year, month, day, weekday;
    (void)platform_datetime(&year, &month, &day, h, m, s, &weekday);
}

void platform_reboot(void) {
    serial_write("[power] Reboot requested\n");
    __asm__ volatile("cli" : : : "memory");
    if (ps2_wait_write())
        out8(0x64, 0xfe);
    /* If the controller does not reset, force a triple fault. */
    IDTPointer empty = {0, 0};
    __asm__ volatile("lidt %0; int3" : : "m"(empty) : "memory");
    halt_forever();
}
void platform_poweroff(void) {
    serial_write("[power] Power off requested (QEMU PC ACPI port)\n");
    /* This is explicitly a QEMU-PC poweroff, not a general ACPI interpreter. */
    out16(0x604, 0x2000);
    out16(0xb004, 0x2000); /* Older QEMU/Bochs-compatible machines. */
    serial_write("[power] Machine did not power off; CPU halted.\n");
    halt_forever();
}
