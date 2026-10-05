/* ACPI MADT / xAPIC startup and bounded parallel kernel copy workers.
 * APs run independent user schedulers after the boot worker self-test. */
#include "smp.h"
#include "mmio.h"
#include "process.h"
#include "microcode.h"
#include "acpi.h"
#define CPU_CAP 8u
static volatile uint32_t *lapic;
static unsigned cpu_count = 1;
static uint32_t timer_count = 62500;
static bool users_started;
static uint8_t cpu_lookup[256];
static struct {
    uint32_t apic_id, online, job;
    void *dst;
    const void *src;
    size_t bytes;
    uint64_t completed;
    uint8_t stack[32768] __attribute__((aligned(16)));
    uint8_t idt[4096] __attribute__((aligned(16)));
} cpus[CPU_CAP];
extern const uint8_t ap_stub_start[], ap_stub_end[];
static uint32_t rd(unsigned offset) {
    return lapic[offset / 4];
}
static void wr(unsigned offset, uint32_t value) {
    lapic[offset / 4] = value;
    (void)rd(0x20);
}
static bool ipi(unsigned id, uint32_t value) {
    for (unsigned i = 0; i < 1000000; i++) {
        if (!(rd(0x300) & 0x1000)) {
            wr(0x310, id << 24);
            wr(0x300, value);
            return true;
        }
        __asm__ volatile("pause");
    }
    return false;
}
static uint64_t ticks_now(void) {
    return platform_ticks();
}
static void delay_ticks(unsigned ticks) {
    uint64_t end = ticks_now() + ticks;
    while (ticks_now() < end)
        __asm__ volatile("pause");
}
/* MADT discovery is shared with the IOAPIC router (include/acpi.h). */
static const uint8_t *madt(void) {
    return acpi_find_table("APIC");
}
static void copy_bytes(void *dst, const void *src, size_t bytes) {
    size_t words = bytes / 8;
    __asm__ volatile("cld;rep movsq" : "+D"(dst), "+S"(src), "+c"(words)::"memory");
    bytes &= 7;
    __asm__ volatile("rep movsb" : "+D"(dst), "+S"(src), "+c"(bytes)::"memory");
}
static __attribute__((noreturn)) void ap_main(uint64_t index) {
    struct __attribute__((packed)) {
        uint16_t limit;
        uint64_t base;
    } idtr;
    __asm__ volatile("sidt %0"
                     : "=m"(idtr)); /* INIT leaves no valid IDT; BSP passes a copy below. */
    idtr.limit = 4095;
    idtr.base = (uintptr_t)cpus[index].idt;
    __asm__ volatile("lidt %0" ::"m"(idtr));
    wr(0xf0, 0x1ff);
    wr(0x320, 0x10000);
    wr(0x350, 0x10000);
    wr(0x360, 0x10000);
    wr(0x370, 0x10000);
    if (!microcode_apply_ap()) {
        for (;;)
            __asm__ volatile("cli;hlt");
    }
    cpus[index].apic_id = rd(0x20) >> 24;
    __atomic_store_n(&cpus[index].online, 1, __ATOMIC_RELEASE);
    for (;;) {
        if (__atomic_load_n(&users_started, __ATOMIC_ACQUIRE))
            process_ap_run();
        if (__atomic_load_n(&cpus[index].job, __ATOMIC_ACQUIRE)) {
            copy_bytes(cpus[index].dst, cpus[index].src, cpus[index].bytes);
            cpus[index].completed++;
            __atomic_store_n(&cpus[index].job, 0, __ATOMIC_RELEASE);
        }
        __asm__ volatile("sti;hlt;cli" ::: "memory");
    }
}
unsigned platform_online_cpus(void) {
    return cpu_count;
}
unsigned platform_current_cpu(void) {
    if (!lapic)
        return 0;
    uint32_t a = 1, b, c, d;
    __asm__ volatile("cpuid" : "+a"(a), "=b"(b), "=c"(c), "=d"(d));
    return cpu_lookup[b >> 24];
}
void smp_local_timer(void) {
    if (lapic) {
        wr(0x3e0, 3);
        wr(0x320, 0x20000 | 48);
        wr(0x380, timer_count);
    }
}
void smp_start_users(void) {
    __atomic_store_n(&users_started, true, __ATOMIC_RELEASE);
    for (unsigned i = 1; i < cpu_count; i++)
        (void)ipi(cpus[i].apic_id, 240);
}
void smp_wake_cpu(unsigned cpu) {
    if (lapic && users_started && cpu < cpu_count)
        (void)ipi(cpus[cpu].apic_id, 240);
}

void platform_smp_eoi(void) {
    if (lapic)
        wr(0xb0, 0);
}
void smp_copy(void *dst, const void *src, size_t bytes) {
    if (users_started || cpu_count == 1 || bytes < 512 * 1024) {
        copy_bytes(dst, src, bytes);
        return;
    }
    size_t part = (bytes / cpu_count) & ~63ull;
    for (unsigned i = 1; i < cpu_count; i++) {
        cpus[i].dst = (uint8_t *)dst + part * i;
        cpus[i].src = (const uint8_t *)src + part * i;
        cpus[i].bytes = i + 1 == cpu_count ? bytes - part * i : part;
        __atomic_store_n(&cpus[i].job, 1, __ATOMIC_RELEASE);
        if (!ipi(cpus[i].apic_id, 0xf0)) { /* Do not return while a worker could still write. */
            while (!ipi(cpus[i].apic_id, 0xf0))
                __asm__ volatile("pause");
        }
    }
    copy_bytes(dst, src, part);
    for (unsigned i = 1; i < cpu_count; i++)
        while (__atomic_load_n(&cpus[i].job, __ATOMIC_ACQUIRE))
            __asm__ volatile("pause");
}
void smp_init(void) {
    uint32_t a = 1, b, c, d;
    __asm__ volatile("cpuid" : "+a"(a), "=b"(b), "=c"(c), "=d"(d));
    if (!(d & (1u << 9)))
        return;
    const uint8_t *m = madt();
    if (!m) {
        serial_write("[smp] No valid MADT; uniprocessor fallback\n");
        return;
    }
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(0x1b));
    if (hi || (lo & (1u << 10))) {
        serial_write("[smp] x2APIC not supported; BSP only\n");
        return;
    }
    uint64_t base = lo & 0xfffff000u;
    if (!base || !platform_map_mmio(base, 4096))
        return;
    lapic = (volatile uint32_t *)(uintptr_t)base;
    lo |= 1u << 11;
    __asm__ volatile("wrmsr" ::"a"(lo), "d"(hi), "c"(0x1b));
    wr(0xf0, 0x1ff);
    cpus[0].apic_id = rd(0x20) >> 24;
    cpus[0].online = 1;
    if (!platform_ram_range(0x8000, 4096) || (size_t)(ap_stub_end - ap_stub_start) > 0xf00) {
        serial_write("[smp] Low trampoline RAM unavailable\n");
        return;
    }
    memcpy((void *)0x8000, ap_stub_start, (size_t)(ap_stub_end - ap_stub_start));
    uint64_t cr3;
    __asm__ volatile("mov %%cr3,%0" : "=r"(cr3));
    *(volatile uint64_t *)0x8f00 = cr3;
    struct __attribute__((packed)) {
        uint16_t limit;
        uint64_t base;
    } idtr;
    __asm__ volatile("sidt %0" : "=m"(idtr));
    uint32_t len;
    memcpy(&len, m + 4, 4);
    if (len < 44)
        return;
    for (unsigned at = 44; at + 2 <= len && cpu_count < CPU_CAP;) {
        unsigned type = m[at], size = m[at + 1];
        if (size < 2 || at + size > len)
            break;
        if (type == 0 && size >= 8) {
            uint32_t flags;
            memcpy(&flags, m + at + 4, 4);
            unsigned id = m[at + 3];
            if ((flags & 1) && id != cpus[0].apic_id) {
                unsigned slot = cpu_count;
                cpus[slot].apic_id = id;
                memcpy(cpus[slot].idt, (void *)(uintptr_t)idtr.base, 4096);
                for (unsigned v = 0; v < 256; v++)
                    cpus[slot].idt[v * 16 + 4] = 0;
                *(volatile uint64_t *)0x8f08 =
                    (uintptr_t)(cpus[slot].stack + sizeof cpus[slot].stack);
                *(volatile uint64_t *)0x8f10 = slot;
                *(volatile uint64_t *)0x8f18 = (uintptr_t)ap_main;
                if (!ipi(id, 0xc500))
                    break;
                delay_ticks(1);
                if (!ipi(id, 0x8500))
                    break;
                delay_ticks(1);
                if (!ipi(id, 0x4608))
                    break;
                delay_ticks(1);
                if (!__atomic_load_n(&cpus[slot].online, __ATOMIC_ACQUIRE))
                    (void)ipi(id, 0x4608);
                uint64_t end = ticks_now() + 20;
                while (!__atomic_load_n(&cpus[slot].online, __ATOMIC_ACQUIRE) && ticks_now() < end)
                    __asm__ volatile("pause");
                if (cpus[slot].online) {
                    cpu_lookup[id] = (uint8_t)slot;
                    cpu_count++;
                } else {
                    serial_write("[smp] AP startup timeout; stopping enumeration\n");
                    break;
                }
            }
        }
        at += size;
    }
    serial_write("[smp] Online CPUs=");
    char n[24];
    uint_to_str(cpu_count, n);
    serial_write(n);
    serial_write(" (per-CPU user schedulers)\n");
    /* Exercise every AP and compare the full result before exposing workers. */
    static uint8_t source[1024 * 1024], target[1024 * 1024];
    for (unsigned i = 0; i < sizeof source; i++)
        source[i] = (uint8_t)(i * 17 + 9);
    smp_copy(target, source, sizeof source);
    bool good = true;
    for (unsigned i = 0; i < sizeof source; i++)
        if (target[i] != source[i])
            good = false;
    for (unsigned i = 1; i < cpu_count; i++)
        if (!cpus[i].completed)
            good = false;
    /* BSP expiry checks wake idle CPUs by IPI at millisecond deadlines. APs only
     * need a 10 ms preemption tick, avoiding redundant 1 kHz interrupts on every
     * rendering CPU. */
    wr(0x3e0, 3);
    wr(0x320, 0x10000 | 48);
    wr(0x380, 0xffffffff);
    delay_ticks(5);
    timer_count = (0xffffffffu - rd(0x390)) / 5;
    if (!timer_count)
        timer_count = 62500;
    wr(0x320, 0x10000);
    serial_write(good ? "[smp] Parallel physical copy self-test PASS\n"
                      : "[fatal] SMP copy FAIL\n");
    if (!good)
        for (;;)
            __asm__ volatile("cli;hlt");
}
