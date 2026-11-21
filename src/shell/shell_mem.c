// Memory-related shell commands: sys.mem.info, sys.mem.test, sys.mem.free.

#include "shell_internal.h"
#include <arch/acpi.h>
#include <syscall.h>

extern uint8  data_counter_mmap_entries;
extern uint32 MEMMAP_START;
extern uint32 PML4T_LOCATION;
extern uint32 PAGING_LOCATION_END;
extern uint32 mem_amount[2];

void cmd_meminfo(void) {
    if (use_syscalls) {
        SysMemInfo info;
        sys_meminfo(&info);
        sh_print("Memory Information:\n");
        sh_print_hex(info.mmap_entries, " MMAP entries\n");
        sh_print_hex(info.paging_location_end, " Paging structures end\n");
    } else {
        sh_print("Memory Information:\n");
        sh_print_hex(data_counter_mmap_entries, " MMAP entries\n");
        sh_print_hex((long)&MEMMAP_START, " MEMMAP start address\n");
        sh_print_hex(PML4T_LOCATION, " PML4T location\n");
        sh_print_hex(PAGING_LOCATION_END, " Paging structures end\n");
    }
}

static uint32 memtest_failures;

static void memtest_check(int ok, char *what) {
    sh_print(ok ? "  ok   " : "  FAIL ");
    sh_print(what);
    sh_print("\n");
    if (!ok) memtest_failures++;
}

static uint32 heap_used(void) {
    uint32 used, free_blocks, total;
    sh_heap_stats(&used, &free_blocks, &total);
    return used;
}

// Fill [p, p+len) with a pattern derived from tag; return 1 if it reads back.
static int fill_and_check(uint8 *p, uint64 len, uint8 tag) {
    for (uint64 i = 0; i < len; i++) p[i] = (uint8)(tag ^ i);
    for (uint64 i = 0; i < len; i++)
        if (p[i] != (uint8)(tag ^ i)) return 0;
    return 1;
}

static int disjoint(uint64 a, uint64 alen, uint64 b, uint64 blen) {
    return a + alen <= b || b + blen <= a;
}

void cmd_memtest(void) {
    sh_print("Testing heap allocator...\n");
    memtest_failures = 0;

    uint32 used0, free0, total;
    sh_heap_stats(&used0, &free0, &total);

    // Sizes: 1 page, 2 pages, 5 pages + 1 byte (6 pages)
    uint64 la = 4096, lb = 2 * 4096, lc = 5 * 4096 + 1;
    uint8 *a = (uint8 *)sh_malloc(la);
    uint8 *b = (uint8 *)sh_malloc(lb);
    uint8 *c = (uint8 *)sh_malloc(lc);
    memtest_check(a && b && c, "alloc 1, 2, 6 pages");
    if (!(a && b && c)) {
        sh_free(a); sh_free(b); sh_free(c);
        sh_print("Heap test: FAIL\n");
        return;
    }

    memtest_check((((uint64)a | (uint64)b | (uint64)c) & 4095) == 0,
                  "page aligned");
    memtest_check(heap_used() == used0 + 1 + 2 + 6, "used blocks +9");
    memtest_check(disjoint((uint64)a, la, (uint64)b, lb) &&
                  disjoint((uint64)a, la, (uint64)c, lc) &&
                  disjoint((uint64)b, lb, (uint64)c, lc), "spans disjoint");

    fill_and_check(a, la, 0xA5);
    fill_and_check(b, lb, 0x5A);
    memtest_check(fill_and_check(c, lc, 0x3C), "write full 6-page span");
    int intact = 1;
    for (uint64 i = 0; i < la; i++) if (a[i] != (uint8)(0xA5 ^ i)) intact = 0;
    for (uint64 i = 0; i < lb; i++) if (b[i] != (uint8)(0x5A ^ i)) intact = 0;
    memtest_check(intact, "other spans intact after write");

    // Freeing an address inside a span must be refused.
    sh_free(c + 4096);
    memtest_check(heap_used() == used0 + 9, "kfree inside span refused");

    // First-fit: no free run of 2 blocks exists below b, so b is reused.
    sh_free(b);
    memtest_check(heap_used() == used0 + 7, "kfree releases whole span");
    uint8 *d = (uint8 *)sh_malloc(lb);
    memtest_check(d == b, "re-alloc same size reuses span");

    // More than the whole heap cannot be satisfied.
    void *e = sh_malloc((uint64)(total + 1) * 4096);
    memtest_check(e == NULL, "alloc larger than heap fails");
    sh_free(e);

    sh_free(a);
    sh_free(c);
    sh_free(d);
    memtest_check(heap_used() == used0, "used blocks back to start");

    if (memtest_failures == 0) {
        sh_print("Heap test: PASS\n");
    } else {
        sh_print("Heap test: FAIL (");
        sh_print_dec(memtest_failures);
        sh_print(" checks)\n");
    }
}

void cmd_free(void) {
    uint32 used, free_pages, total;
    sh_heap_stats(&used, &free_pages, &total);

    uint64 total_bytes, total_mb;
    uint32 numa_nodes;
    uint64 paging_end;

    if (use_syscalls) {
        SysMemInfo minfo;
        sys_meminfo(&minfo);
        total_bytes = (minfo.mem_amount_high << 32) | minfo.mem_amount_low;
        paging_end = minfo.paging_location_end;
        SysCpuInfo cinfo;
        sys_cpu_info(&cinfo);
        numa_nodes = cinfo.numa_node_count;
    } else {
        total_bytes = ((uint64)mem_amount[0] << 32) | (uint64)mem_amount[1];
        paging_end = (uint64)PAGING_LOCATION_END;
        numa_nodes = acpi_numa_node_count();
    }

    total_mb = total_bytes / (1024 * 1024);
    uint32 heap_used_kb = used * 4;
    uint32 heap_free_kb = free_pages * 4;
    uint32 heap_total_kb = total * 4;

    if (numa_nodes == 0) numa_nodes = 1;
    uint64 per_node_mb = total_mb / numa_nodes;

    uint64 kernel_kb = paging_end / 1024;
    uint64 node0_used_kb = kernel_kb + heap_used_kb;
    uint64 node0_free_mb = per_node_mb - (node0_used_kb / 1024);

    sh_print("              total       used       free\n");

    sh_print("Cluster ");
    sh_print_dec_pad(total_mb, 10);
    sh_print(" MB");
    sh_print_dec_pad(node0_used_kb / 1024, 9);
    sh_print(" MB");
    sh_print_dec_pad(total_mb - (node0_used_kb / 1024), 9);
    sh_print(" MB\n");

    sh_print("Node    ");
    sh_print_dec_pad(total_mb, 10);
    sh_print(" MB");
    sh_print_dec_pad(node0_used_kb / 1024, 9);
    sh_print(" MB");
    sh_print_dec_pad(total_mb - (node0_used_kb / 1024), 9);
    sh_print(" MB\n");

    sh_print("NUMA 0  ");
    sh_print_dec_pad(per_node_mb, 10);
    sh_print(" MB");
    sh_print_dec_pad(node0_used_kb / 1024, 9);
    sh_print(" MB");
    sh_print_dec_pad(node0_free_mb, 9);
    sh_print(" MB\n");

    if (numa_nodes > 1) {
        sh_print("NUMA 1  ");
        sh_print_dec_pad(per_node_mb, 10);
        sh_print(" MB");
        sh_print_dec_pad(0, 9);
        sh_print(" MB");
        sh_print_dec_pad(per_node_mb, 9);
        sh_print(" MB\n");
    }

    sh_print("Heap    ");
    sh_print_dec_pad(heap_total_kb, 10);
    sh_print(" KB");
    sh_print_dec_pad(heap_used_kb, 9);
    sh_print(" KB");
    sh_print_dec_pad(heap_free_kb, 9);
    sh_print(" KB\n");
}
