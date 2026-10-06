/* Experimental native ARM64 platform target; no Linux kernel or ABI.
 * QEMU virt, EL1, PL011, FDT, architectural timer, PSCI CPU_ON and workers.
 * Desktop, disk and networking remain x86_64-only in 0.7. */
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
static volatile uint32_t *uart = (volatile uint32_t *)0x09000000;
static uint64_t cpu_ids[8], memory_bytes, timer_frequency;
static unsigned cpu_total = 1, online = 1;
static bool psci_hvc = true;
static struct {
    uint32_t online, job;
    uint64_t result;
} cpus[8];
extern void arm_secondary(void);
static void putc(char c) {
    while (uart[6] & (1u << 5))
        __asm__ volatile("yield");
    uart[0] = (uint8_t)c;
}
static void puts(const char *s) {
    while (*s) {
        if (*s == '\n')
            putc('\r');
        putc(*s++);
    }
}
void arm_log(const char *s) {
    puts(s);
}
extern void arm_mmu_init(void), arm_mmu_enable(void), arm_el0_test(void);
static void number(uint64_t v) {
    char b[24];
    unsigned n = 0;
    do {
        b[n++] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    while (n)
        putc(b[--n]);
}
static bool eq(const char *a, const char *b) {
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}
static bool prefix(const char *a, const char *b) {
    while (*b)
        if (*a++ != *b++)
            return false;
    return true;
}
static uint32_t be32(const uint8_t *p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}
static uint64_t be64(const uint8_t *p) {
    /* The DTB is only 4-byte aligned and, until the MMU is enabled, is mapped
       as Device memory where a wider unaligned access raises an alignment
       fault. Keep this as two 32-bit loads so the compiler cannot fold it into
       a single unaligned 64-bit access. */
    uint32_t hi = be32(p);
    __asm__ volatile("" ::: "memory");
    uint32_t lo = be32(p + 4);
    return (uint64_t)hi << 32 | lo;
}
static uint64_t counter(void) {
    uint64_t value;
    __asm__ volatile("isb; mrs %0,cntvct_el0" : "=r"(value));
    return value;
}
static int64_t psci(uint64_t function, uint64_t a, uint64_t b, uint64_t c) {
    register uint64_t x0 __asm__("x0") = function, x1 __asm__("x1") = a, x2 __asm__("x2") = b,
                         x3 __asm__("x3") = c;
    if (psci_hvc)
        __asm__ volatile("hvc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x3) : "memory");
    else
        __asm__ volatile("smc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x3) : "memory");
    return (int64_t)x0;
}
static bool terminated(const char *s, size_t n) {
    for (size_t i = 0; i < n; i++)
        if (!s[i])
            return true;
    return false;
}
static bool fdt(const uint8_t *b) {
    if (be32(b) != 0xd00dfeed)
        return false;
    uint32_t total = be32(b + 4), off = be32(b + 8), str = be32(b + 12), ss = be32(b + 32),
             sz = be32(b + 36);
    if (total < 40 || total > 2 * 1024 * 1024 || off > total || str > total || ss > total - str ||
        sz > total - off)
        return false;
    unsigned depth = 0, found = 0;
    const char *nodes[32];
    uint32_t at = off, end = off + sz;
    while (at + 4 <= end) {
        unsigned token = be32(b + at);
        at += 4;
        if (token == 1) {
            if (depth == 32 || !terminated((const char *)b + at, end - at))
                return false;
            nodes[depth++] = (const char *)b + at;
            while (at < end && b[at])
                at++;
            at = (at + 4) & ~3u;
        } else if (token == 2) {
            if (!depth)
                return false;
            depth--;
        } else if (token == 3) {
            if (!depth || at + 8 > end)
                return false;
            uint32_t n = be32(b + at), name = be32(b + at + 4);
            at += 8;
            if (n > end - at || name >= ss || !terminated((const char *)b + str + name, ss - name))
                return false;
            const char *k = (const char *)b + str + name;
            const uint8_t *v = b + at;
            if (eq(k, "reg") && prefix(nodes[depth - 1], "cpu@") && (n == 4 || n == 8) && found < 8)
                cpu_ids[found++] = n == 8 ? be64(v) : be32(v);
            if (eq(k, "reg") && prefix(nodes[depth - 1], "memory@") && n >= 16)
                memory_bytes = be64(v + 8);
            if (eq(k, "reg") && prefix(nodes[depth - 1], "pl011@") && n >= 16)
                uart = (volatile uint32_t *)(uintptr_t)be64(v);
            if (eq(nodes[depth - 1], "psci") && eq(k, "method") && n >= 4)
                psci_hvc = !eq((const char *)v, "smc");
            at = (at + n + 3) & ~3u;
        } else if (token == 9) {
            cpu_total = found ? found : 1;
            return true;
        } else if (token != 4)
            return false;
    }
    return false;
}
static uint64_t work(unsigned index) {
    uint64_t sum = 0;
    for (unsigned n = 0; n < 100000; n++)
        sum += (uint64_t)(n + 1) * (index + 1);
    return sum;
}
void arm_ap_main(uint64_t index) {
    arm_mmu_enable();
    if (index >= 8)
        for (;;)
            __asm__ volatile("wfe");
    __atomic_store_n(&cpus[index].online, 1, __ATOMIC_RELEASE);
    __asm__ volatile("sev");
    for (;;) {
        if (__atomic_load_n(&cpus[index].job, __ATOMIC_ACQUIRE)) {
            cpus[index].result = work((unsigned)index);
            __atomic_store_n(&cpus[index].job, 0, __ATOMIC_RELEASE);
            __asm__ volatile("sev");
        }
        __asm__ volatile("wfe");
    }
}
static void bench(void) {
    for (unsigned i = 1; i < cpu_total; i++)
        if (cpus[i].online)
            __atomic_store_n(&cpus[i].job, 1, __ATOMIC_RELEASE);
    __asm__ volatile("sev");
    cpus[0].result = work(0);
    bool good = true;
    for (unsigned i = 1; i < cpu_total; i++)
        if (cpus[i].online) {
            uint64_t deadline = counter() + timer_frequency;
            while (__atomic_load_n(&cpus[i].job, __ATOMIC_ACQUIRE) && counter() < deadline)
                __asm__ volatile("yield");
            if (cpus[i].job || cpus[i].result != 5000050000ull * (i + 1))
                good = false;
        }
    puts(good ? "[arm64] Parallel workers PASS\n" : "[arm64] Parallel workers FAIL\n");
}
static void command(char *s) {
    if (eq(s, "help"))
        puts("help  info  cpus  uptime  bench  protect  echo TEXT  reboot  poweroff\n");
    else if (eq(s, "info")) {
        puts("ArkOS 0.8 ARM64 experimental / native EL1 / QEMU virt\nRAM MiB: ");
        number(memory_bytes >> 20);
        puts("\nPL011 UART / FDT / PSCI / architectural timer\n4 KiB MMU, EL0 SVC and W^X "
             "self-tests; no desktop or EL0 scheduler.\n");
    } else if (eq(s, "cpus")) {
        puts("Online CPUs: ");
        number(online);
        puts("\n");
        for (unsigned i = 0; i < cpu_total; i++) {
            puts("CPU ");
            number(i);
            puts(" MPIDR ");
            number(cpu_ids[i]);
            puts(cpus[i].online ? " online\n" : " offline\n");
        }
    } else if (eq(s, "uptime")) {
        number(counter() / timer_frequency);
        puts(" seconds\n");
    } else if (eq(s, "bench"))
        bench();
    else if (eq(s, "protect"))
        arm_el0_test();
    else if (prefix(s, "echo ")) {
        puts(s + 5);
        puts("\n");
    } else if (eq(s, "reboot"))
        (void)psci(0x84000009, 0, 0, 0);
    else if (eq(s, "poweroff"))
        (void)psci(0x84000008, 0, 0, 0);
    else if (*s)
        puts("Unknown command; type help.\n");
}
void arm_main(uint64_t dtb) {
    uint64_t el;
    __asm__ volatile("mrs %0,CurrentEL" : "=r"(el));
    puts("[arm64] ArkOS native entry EL");
    number(el >> 2);
    puts("\n");
    if (el != 4) {
        puts("[arm64] EL1 required; use virt,virtualization=off\n");
        for (;;)
            __asm__ volatile("wfe");
    }
    __asm__ volatile("mrs %0,cntfrq_el0" : "=r"(timer_frequency));
    if (!timer_frequency)
        for (;;)
            __asm__ volatile("wfe");
    if (dtb < 0x40000000 || dtb > 0x401fffff)
        dtb = 0x40000000;
    if (!fdt((const uint8_t *)(uintptr_t)dtb)) {
        puts("[arm64] Invalid FDT; stopped\n");
        for (;;)
            __asm__ volatile("wfe");
    }
    puts("[arm64] FDT validated; RAM MiB=");
    number(memory_bytes >> 20);
    puts(" timer Hz=");
    number(timer_frequency);
    puts("\n");
    uint64_t bsp;
    __asm__ volatile("mrs %0,mpidr_el1" : "=r"(bsp));
    bsp &= 0xff00ffffffull;
    for (unsigned i = 0; i < cpu_total; i++)
        if (cpu_ids[i] == bsp) {
            uint64_t tmp = cpu_ids[0];
            cpu_ids[0] = bsp;
            cpu_ids[i] = tmp;
            break;
        }
    arm_mmu_init();
    arm_mmu_enable();
    arm_el0_test();
    cpus[0].online = 1;
    for (unsigned i = 1; i < cpu_total; i++) {
        if (psci(0xc4000003, cpu_ids[i], (uintptr_t)arm_secondary, i))
            continue;
        uint64_t end = counter() + timer_frequency;
        while (!__atomic_load_n(&cpus[i].online, __ATOMIC_ACQUIRE) && counter() < end)
            __asm__ volatile("yield");
        if (cpus[i].online)
            online++;
    }
    puts("[arm64] PSCI online CPUs=");
    number(online);
    puts("\n");
    bench();
    puts("[arm64] Serial shell ready\nArkOS ARM64> ");
    char line[256];
    unsigned len = 0;
    for (;;) {
        if (uart[6] & (1u << 4)) {
            __asm__ volatile("yield");
            continue;
        }
        unsigned value = uart[0];
        if (value & 0xf00)
            continue;
        char c = (char)value;
        if (c == '\r' || c == '\n') {
            putc('\r');
            putc('\n');
            line[len] = 0;
            command(line);
            len = 0;
            puts("ArkOS ARM64> ");
        } else if (c == 8 || c == 127) {
            if (len) {
                len--;
                puts("\b \b");
            }
        } else if ((unsigned char)c >= 32 && len < sizeof line - 1) {
            line[len++] = c;
            putc(c);
        }
    }
}
