#include <types.h>
#include <drivers/monitor.h>
#include <kernel/mem.h>
#include <kernel/app.h>
#include <kernel/paging.h>
#include <arch/acpi.h>

extern uint8 data_counter_mmap_entries;
extern uint32 MEMMAP_START;
extern uint32 PML4T_LOCATION;
extern uint32 PAGING_LOCATION_END;
extern uint8 kernelEnd[];

// Page table entry flags
#define PTE_PRESENT   0x01
#define PTE_WRITABLE  0x02
#define PTE_USER      0x04
#define PTE_PWT       0x08
#define PTE_PCD       0x10
#define PTE_PAT_4K    0x80          // PAT bit in a 4 KB PTE (PS bit in a PDE/PDPTE)
#define PTE_PS        0x80          // PDPTE: 1 GB page, PDE: 2 MB page

// Heap: dynamic range from end of BSS to 0x9F000
#define BLOCK_SIZE      4096
#define HEAP_CEILING    0x9F000
#define MAX_BITMAP_SIZE 32              // supports up to 256 blocks (~1MB)

static uint64 heap_start;
static uint64 heap_end;
static uint32 num_blocks;
static uint8 heap_bitmap[MAX_BITMAP_SIZE];
// Blocks in the allocation starting at this block; 0 if not a span start.
static uint8 span_len[MAX_BITMAP_SIZE * 8];

static int block_used(uint32 i) {
    return heap_bitmap[i / 8] & (1 << (i % 8));
}

static void block_set(uint32 i, int used) {
    if (used) heap_bitmap[i / 8] |= (uint8)(1 << (i % 8));
    else      heap_bitmap[i / 8] &= (uint8)~(1 << (i % 8));
}

// End (exclusive) of the bump allocator range. 0 until init_memmgr runs,
// so alloc_pages refuses everything before the limit is known.
static uint64 alloc_limit;

// Bump allocator limit: end of the usable E820 entry that contains
// PAGING_LOCATION_END, capped at APP_LOAD_BASE (app images live above it).
// Real firmware reports holes and reserved ranges above 1 MB, so the
// range is never assumed to be contiguous up to APP_LOAD_BASE.
static uint64 find_alloc_limit(void) {
    uint64 start = (uint64)PAGING_LOCATION_END;
    E820E *map = (E820E *)&MEMMAP_START;
    for (uint32 i = 0; i < data_counter_mmap_entries; i++) {
        if (map[i].type != E820_TYPE_FREE) continue;
        uint64 base = map[i].base;
        uint64 end = base + map[i].length;
        if (start < base || start >= end) continue;
        return end < APP_LOAD_BASE ? end : APP_LOAD_BASE;
    }
    return 0;
}

void init_memmgr() {
    kprint("Initializing memory manager.\n");
    kprint_long2hex(data_counter_mmap_entries,  "MMAP Entries\n");
    kprint_long2hex((long) &MEMMAP_START,       "MEMMAP_START\n");
    kprint_long2hex(PML4T_LOCATION,             "PML4T_LOCATION\n");
    kprint_long2hex(PAGING_LOCATION_END,        "PAGING_LOCATION_END\n");

    alloc_limit = find_alloc_limit();
    if (alloc_limit == 0)
        kprint("MEM: PAGING_LOCATION_END not in a usable E820 range, first alloc_pages will halt\n");
    else
        kprint_long2hex(alloc_limit, " alloc_pages limit\n");
}

// Allocate n contiguous pages from above PAGING_LOCATION_END (high memory).
// Boot-time only, never freed. Never returns 0: a request that does not
// fit below alloc_limit halts the machine, so callers have no failure
// path. All sizes are fixed at build time or by the hardware found at
// boot, so a machine either boots with all its memory or does not boot.
uint64 alloc_pages(uint32 n) {
    uint64 addr = (uint64)PAGING_LOCATION_END;
    uint64 size = (uint64)n * 4096;
    if (n == 0 || addr + size > alloc_limit) {
        kprint("MEM: alloc_pages(");
        kprint_dec(n);
        kprint(" pages) failed, next ");
        kprint_long2hex(addr, ", limit ");
        kprint_long2hex(alloc_limit, "\nMEM: halting\n");
        for (;;) __asm__ volatile("cli; hlt");
    }
    PAGING_LOCATION_END = (uint32)(addr + size);
    return addr;
}

// Per-node page pools (alloc_pages_node). One pool per NUMA node that has
// usable RAM: E820 usable ranges intersected with the node's SRAT memory
// ranges, above NODE_POOL_FLOOR. Same contract as alloc_pages: boot-time
// only, never freed, never returns 0.
#define NODE_POOL_FLOOR   (APP_LOAD_BASE + (uint64)MAX_APPS * APP_REGION_SIZE)  // above the app slots
#define MAX_POOL_RANGES   8                 // per node
#define SRAT_HOT_PLUG     0x2               // SRAT memory affinity flag bits (ACPI 6.5, 5.2.16.2)
#define SRAT_NON_VOLATILE 0x4

typedef struct {
    uint64 next;                            // first free byte
    uint64 end;                             // exclusive
} PoolRange;

typedef struct {
    uint32 node;                            // proximity domain
    uint32 nranges;
    uint64 bytes;                           // total size of the ranges
    PoolRange r[MAX_POOL_RANGES];
} NodePool;

static NodePool pools[MAX_NUMA_NODES];
static uint32 pool_count;
static uint32 bsp_node;

// Node -> pool for nodes without memory of their own (nearest pool).
static uint32 fallback_node[MAX_NUMA_NODES];
static uint32 fallback_pool[MAX_NUMA_NODES];
static uint32 fallback_count;

static void mem_halt(void) {
    kprint("MEM: halting\n");
    for (;;) __asm__ volatile("cli; hlt");
}

static NodePool *pool_of(uint32 node) {
    for (uint32 i = 0; i < pool_count; i++)
        if (pools[i].node == node) return &pools[i];
    return NULL;
}

// Two ranges from overlapping E820 or SRAT entries would hand out the
// same pages twice.
static void check_overlap(uint64 base, uint64 end) {
    for (uint32 i = 0; i < pool_count; i++) {
        for (uint32 j = 0; j < pools[i].nranges; j++) {
            if (base < pools[i].r[j].end && pools[i].r[j].next < end) {
                kprint_long2hex(base, " MEM: node pool range overlaps an existing range at ");
                kprint_long2hex(pools[i].r[j].next, "\n");
                mem_halt();
            }
        }
    }
}

static void pool_add(uint32 node, uint64 base, uint64 end) {
    base = (base + 4095) & ~4095ULL;
    end &= ~4095ULL;
    if (base < NODE_POOL_FLOOR) base = NODE_POOL_FLOOR;
    if (end <= base) return;
    check_overlap(base, end);

    NodePool *p = pool_of(node);
    if (!p) {
        if (pool_count >= MAX_NUMA_NODES) {
            kprint("MEM: more than 16 memory nodes, node ");
            kprint_dec(node);
            kprint(" not used\n");
            return;
        }
        p = &pools[pool_count++];
        p->node = node;
        p->nranges = 0;
        p->bytes = 0;
    }
    if (p->nranges >= MAX_POOL_RANGES) {
        kprint_long2hex(base, " MEM: node pool range limit reached, range not used\n");
        return;
    }
    p->r[p->nranges].next = base;
    p->r[p->nranges].end = end;
    p->nranges++;
    p->bytes += end - base;
}

// Called after acpi_init (SRAT parsed), before the first alloc_pages_node.
void mem_numa_init(void) {
    E820E *map = (E820E *)&MEMMAP_START;
    uint32 naff = 0;
    const ACPINumaMemAffinity *aff = acpi_memory_affinities(&naff);

    // BSP node: initial APIC ID (CPUID.1:EBX[31:24]) looked up in SRAT.
    uint32 a, b, c, d;
    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(1), "c"(0));
    bsp_node = 0;
    acpi_cpu_to_node((uint8)(b >> 24), &bsp_node);

    for (uint32 i = 0; i < data_counter_mmap_entries; i++) {
        if (map[i].type != E820_TYPE_FREE) continue;
        uint64 base = map[i].base;
        uint64 end = base + map[i].length;
        if (naff == 0) {                    // no SRAT: one node, node 0
            pool_add(0, base, end);
            continue;
        }
        for (uint32 j = 0; j < naff; j++) {
            uint64 sb = aff[j].base;
            uint64 se = sb + aff[j].length;
            uint64 lo = base > sb ? base : sb;
            uint64 hi = end < se ? end : se;
            if (lo >= hi) continue;
            if (aff[j].flags & (SRAT_HOT_PLUG | SRAT_NON_VOLATILE)) {
                kprint_long2hex(lo, " MEM: SRAT range hot-pluggable or non-volatile, not used, to ");
                kprint_long2hex(hi, "\n");
                continue;
            }
            pool_add(aff[j].proximity_domain, lo, hi);
        }
    }

    for (uint32 i = 0; i < pool_count; i++) {
        kprint("MEM: node ");
        kprint_dec(pools[i].node);
        kprint(" pool ");
        kprint_dec(pools[i].bytes >> 20);
        kprint(" MB in ");
        kprint_dec(pools[i].nranges);
        kprint(" range(s), first ");
        kprint_long2hex(pools[i].r[0].next, "\n");
    }
    kprint("MEM: BSP node ");
    kprint_dec(bsp_node);
    putc('\n');
    if (pool_count == 0) {
        kprint("MEM: no usable RAM above the app slots for node pools\n");
        mem_halt();
    }
}

uint32 mem_bsp_node(void) {
    return bsp_node;
}

uint32 mem_cpu_node(uint8 lapic_id) {
    uint32 node;
    if (acpi_cpu_to_node(lapic_id, &node)) return node;
    return bsp_node;
}

// Pool for a node without memory: the pool at the smallest SLIT distance,
// or the BSP's pool if there is no SLIT. Chosen once per node and logged.
static NodePool *nearest_pool(uint32 node) {
    for (uint32 i = 0; i < fallback_count; i++)
        if (fallback_node[i] == node) return &pools[fallback_pool[i]];

    NodePool *bsp = pool_of(bsp_node);
    uint32 best = bsp ? (uint32)(bsp - pools) : 0;
    uint32 best_dist = 0x100;
    for (uint32 i = 0; i < pool_count; i++) {
        uint8 dist;
        if (acpi_distance(node, pools[i].node, &dist) && dist < best_dist) {
            best_dist = dist;
            best = i;
        }
    }
    kprint("MEM: node ");
    kprint_dec(node);
    kprint(" has no memory pool, using node ");
    kprint_dec(pools[best].node);
    if (best_dist < 0x100) {
        kprint(" (SLIT distance ");
        kprint_dec(best_dist);
        kprint(")\n");
    } else {
        kprint(" (no SLIT: BSP node)\n");
    }
    if (fallback_count < MAX_NUMA_NODES) {
        fallback_node[fallback_count] = node;
        fallback_pool[fallback_count] = best;
        fallback_count++;
    }
    return &pools[best];
}

// Allocate n contiguous pages on NUMA node `node` (proximity domain).
// A node SRAT does not describe (PCI_NUMA_UNKNOWN, THREAD_NUMA_UNKNOWN)
// means the BSP's node. A node without memory uses the nearest node.
// Never returns 0: halts when the node's pool is full.
uint64 alloc_pages_node(uint32 node, uint32 n) {
    if (pool_count == 0) {
        kprint("MEM: alloc_pages_node before mem_numa_init\n");
        mem_halt();
    }
    if (node == 0xFFFFFFFFu) node = bsp_node;
    NodePool *p = pool_of(node);
    if (!p) p = nearest_pool(node);

    uint64 size = (uint64)n * 4096;
    for (uint32 i = 0; n > 0 && i < p->nranges; i++) {
        PoolRange *r = &p->r[i];
        if (r->end - r->next < size) continue;
        uint64 addr = r->next;
        r->next += size;
        return addr;
    }
    kprint("MEM: alloc_pages_node(node ");
    kprint_dec(p->node);
    kprint(", ");
    kprint_dec(n);
    kprint(" pages) failed\n");
    mem_halt();
    return 0;
}

int mem_addr_node(uint64 addr, uint32 *node_out) {
    uint32 naff = 0;
    const ACPINumaMemAffinity *aff = acpi_memory_affinities(&naff);
    for (uint32 i = 0; i < naff; i++) {
        if (addr >= aff[i].base && addr - aff[i].base < aff[i].length) {
            *node_out = aff[i].proximity_domain;
            return 1;
        }
    }
    return 0;
}

uint32 mem_pool_count(void) {
    return pool_count;
}

// Pool i: node, total bytes, bytes used.
void mem_pool_info(uint32 i, uint32 *node, uint64 *bytes, uint64 *used) {
    *node = pools[i].node;
    *bytes = pools[i].bytes;
    uint64 free = 0;
    for (uint32 j = 0; j < pools[i].nranges; j++)
        free += pools[i].r[j].end - pools[i].r[j].next;
    *used = pools[i].bytes - free;
}

// Allocate 64KB BSP stack from high memory, return stack top
uint64 alloc_bsp_stack(void) {
    uint64 base = alloc_pages(16);      // 16 pages = 64KB
    kprint("BSP stack: ");
    kprint_long2hex(base, " - ");
    kprint_long2hex(base + 0x10000, "\n");
    return base + 0x10000;              // stack grows down from top
}

void heap_init() {
    // Heap starts at page-aligned address after all BSS
    heap_start = ((uint64)kernelEnd + 4095) & ~4095ULL;
    heap_end = HEAP_CEILING;
    num_blocks = (heap_end - heap_start) / BLOCK_SIZE;

    // Clamp to bitmap capacity
    if (num_blocks > MAX_BITMAP_SIZE * 8)
        num_blocks = MAX_BITMAP_SIZE * 8;

    for(uint32 i = 0; i < MAX_BITMAP_SIZE; i++) {
        heap_bitmap[i] = 0;
    }
    for(uint32 i = 0; i < MAX_BITMAP_SIZE * 8; i++) {
        span_len[i] = 0;
    }

    kprint("Heap: ");
    kprint_long2hex(heap_start, " - ");
    kprint_long2hex(heap_end, " (");
    kprint_dec(num_blocks);
    kprint(" blocks)\n");
}

// First-fit: ceil(size / BLOCK_SIZE) contiguous blocks.
void *kmalloc(size_t size) {
    if(size == 0) return NULL;
    if(size > (size_t)num_blocks * BLOCK_SIZE) return NULL;

    uint32 n = (uint32)((size + BLOCK_SIZE - 1) / BLOCK_SIZE);
    uint32 run = 0;

    for(uint32 i = 0; i < num_blocks; i++) {
        if(block_used(i)) {
            run = 0;
            continue;
        }
        if(++run < n) continue;

        uint32 first = i + 1 - n;
        for(uint32 b = first; b <= i; b++)
            block_set(b, 1);
        span_len[first] = (uint8)n;
        return (void*)(uint64)(heap_start + first * BLOCK_SIZE);
    }

    return NULL;
}

void kfree(void *ptr) {
    if(ptr == NULL) return;

    uint64 addr = (uint64)ptr;
    if(addr < heap_start || addr >= heap_start + (uint64)num_blocks * BLOCK_SIZE
       || (addr - heap_start) % BLOCK_SIZE != 0) {
        kprint_long2hex(addr, " kfree: not a heap pointer\n");
        return;
    }

    uint32 first = (addr - heap_start) / BLOCK_SIZE;
    uint32 n = span_len[first];
    if(n == 0) {
        kprint_long2hex(addr, " kfree: not an allocation start\n");
        return;
    }

    for(uint32 b = first; b < first + n; b++)
        block_set(b, 0);
    span_len[first] = 0;
}

// Count used/free blocks by scanning the bitmap
void heap_stats(uint32 *used_blocks, uint32 *free_blocks, uint32 *total_blocks) {
    uint32 used = 0;
    for(uint32 i = 0; i < num_blocks; i++) {
        if(block_used(i))
            used++;
    }
    *used_blocks = used;
    *free_blocks = num_blocks - used;
    *total_blocks = num_blocks;
}

// Allocate a new page-table page from above PAGING_LOCATION_END
static uint64 alloc_page_table(void) {
    uint64 addr = alloc_pages(1);
    // Zero all 512 entries (4KB page)
    volatile uint64 *p = (volatile uint64 *)addr;
    for (int i = 0; i < 512; i++) p[i] = 0;
    return addr;
}

// Large pages map only E820 RAM (paging_init), so MMIO inside one means
// the firmware memory map reports a device range as RAM.
static void mmio_in_large_page(uint64 vaddr) {
    kprint_long2hex(vaddr, " MEM: MMIO address inside a RAM large page\nMEM: halting\n");
    for (;;) __asm__ volatile("cli; hlt");
}

// Identity-map [phys_start, phys_start+size) as uncacheable MMIO
void map_mmio_range(uint64 phys_start, uint64 size) {
    uint64 end = phys_start + size;
    uint64 pml4t_addr = (uint64)PML4T_LOCATION;

    for (uint64 vaddr = phys_start; vaddr < end; vaddr += 4096) {
        uint32 pml4_idx = (vaddr >> 39) & 0x1FF;
        uint32 pdpt_idx = (vaddr >> 30) & 0x1FF;
        uint32 pd_idx   = (vaddr >> 21) & 0x1FF;
        uint32 pt_idx   = (vaddr >> 12) & 0x1FF;

        volatile uint64 *pml4 = (volatile uint64 *)pml4t_addr;

        // Check/create PDPT entry
        if (!(pml4[pml4_idx] & PTE_PRESENT)) {
            uint64 new_pdpt = alloc_page_table();
            pml4[pml4_idx] = new_pdpt | PTE_PRESENT | PTE_WRITABLE | PTE_USER;
        }
        volatile uint64 *pdpt = (volatile uint64 *)(pml4[pml4_idx] & 0x000FFFFFFFFFF000ULL);

        // Check/create PD entry
        if (!(pdpt[pdpt_idx] & PTE_PRESENT)) {
            uint64 new_pd = alloc_page_table();
            pdpt[pdpt_idx] = new_pd | PTE_PRESENT | PTE_WRITABLE | PTE_USER;
        }
        if (pdpt[pdpt_idx] & PTE_PS) mmio_in_large_page(vaddr);
        volatile uint64 *pd = (volatile uint64 *)(pdpt[pdpt_idx] & 0x000FFFFFFFFFF000ULL);

        // Check/create PT entry
        if (!(pd[pd_idx] & PTE_PRESENT)) {
            uint64 new_pt = alloc_page_table();
            pd[pd_idx] = new_pt | PTE_PRESENT | PTE_WRITABLE | PTE_USER;
        }
        if (pd[pd_idx] & PTE_PS) mmio_in_large_page(vaddr);
        volatile uint64 *pt = (volatile uint64 *)(pd[pd_idx] & 0x000FFFFFFFFFF000ULL);

        // Map the page: identity map, UC (PCD|PWT, PAT bit 0) for MMIO.
        // A present 4 KB page (VGA in the first 2 MB) gets UC too, other
        // bits kept.
        // PTE_USER: BSP ring-3 tasks call driver code that reads MMIO
        // (shell sys.nic.ls -> nic_link_status), as for all pages until
        // per-thread page tables.
        if (!(pt[pt_idx] & PTE_PRESENT)) {
            pt[pt_idx] = vaddr | PTE_PRESENT | PTE_WRITABLE | PTE_USER | PTE_PCD | PTE_PWT;
        } else {
            pt[pt_idx] = (pt[pt_idx] & ~(uint64)(PTE_PAT_4K | PTE_PCD | PTE_PWT)) | PTE_PCD | PTE_PWT;
        }
    }

    // Flush TLB by reloading CR3
    uint64 cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    __asm__ volatile("mov %0, %%cr3" : : "r"(cr3));
}

// Memory type of the page that maps vaddr in the current page tables:
// PAT entry selected by the leaf's PAT/PCD/PWT bits (Intel SDM vol. 3,
// "PAT"). The MTRRs can still lower it (an MTRR UC range wins).
// Returns -1 if vaddr is not mapped.
static int page_mem_type(uint64 vaddr, uint64 pat) {
    volatile uint64 *t = (volatile uint64 *)(uint64)PML4T_LOCATION;
    for (int level = 3; level >= 0; level--) {
        uint64 e = t[(vaddr >> (12 + 9 * level)) & 0x1FF];
        if (!(e & PTE_PRESENT)) return -1;
        int leaf = level == 0 || (level < 3 && (e & 0x80));   // PS bit
        if (leaf) {
            uint64 pat_bit = level == 0 ? (e >> 7) & 1 : (e >> 12) & 1;
            uint32 idx = (uint32)(pat_bit << 2 | ((e >> 4) & 1) << 1 | ((e >> 3) & 1));
            return (int)((pat >> (8 * idx)) & 7);
        }
        t = (volatile uint64 *)(e & 0x000FFFFFFFFFF000ULL);
    }
    return -1;
}

static char *mem_type_name(int type) {
    switch (type) {
    case 0: return "UC";
    case 1: return "WC";
    case 4: return "WT";
    case 5: return "WP";
    case 6: return "WB";
    case 7: return "UC-";
    case -1: return "unmapped";
    default: return "reserved";
    }
}

static void report_mem_type(char *label, uint64 vaddr, uint64 pat) {
    kprint(label);
    kprint(mem_type_name(page_mem_type(vaddr, pat)));
    putc('\n');
}

// Map APIC MMIO region (legacy wrapper)
void map_mmio_region(void) {
    map_mmio_range(0xFEC00000ULL, 0x300000ULL);
    kprint("MEM: MMIO region 0xFEC00000-0xFEF00000 mapped\n");
    map_mmio_range(0xA0000ULL, 0x20000ULL);      // legacy VGA (text buffer at 0xB8000)

    // Page-level memory types, from the PAT MSR as firmware left it
    uint32 lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(0x277));
    uint64 pat = (uint64)hi << 32 | lo;
    kprint_long2hex(pat, " MEM: IA32_PAT\n");
    report_mem_type("MEM: page type RAM 0x100000: ", 0x100000ULL, pat);
    report_mem_type("MEM: page type IOAPIC 0xFEC00000: ", 0xFEC00000ULL, pat);
    report_mem_type("MEM: page type LAPIC 0xFEE00000: ", 0xFEE00000ULL, pat);
    report_mem_type("MEM: page type VGA 0xB8000: ", 0xB8000ULL, pat);
    uint64 high = paging_first_ram_above_4g();
    if (high) {
        kprint_long2hex(high, "");
        report_mem_type(" MEM: page type RAM above 4 GB: ", high, pat);
    }
}

// Prevent GCC from replacing these loops with calls to themselves
#pragma GCC push_options
#pragma GCC optimize("no-tree-loop-distribute-patterns")

void *memcpy(void *dest, const void *src, size_t n) {
    uint8 *d = (uint8 *)dest;
    const uint8 *s = (const uint8 *)src;
    for (size_t i = 0; i < n; i++)
        d[i] = s[i];
    return dest;
}

void *memset(void *dest, int val, size_t n) {
    uint8 *d = (uint8 *)dest;
    for (size_t i = 0; i < n; i++)
        d[i] = (uint8)val;
    return dest;
}

#pragma GCC pop_options
