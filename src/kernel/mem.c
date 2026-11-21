#include <types.h>
#include <drivers/monitor.h>
#include <kernel/mem.h>
#include <kernel/app.h>

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
        kprint("MEM: PAGING_LOCATION_END not in a usable E820 range, alloc_pages disabled\n");
    else
        kprint_long2hex(alloc_limit, " alloc_pages limit\n");
}

// Allocate n contiguous pages from above PAGING_LOCATION_END (high memory).
// Returns 0 when the request does not fit below alloc_limit.
uint64 alloc_pages(uint32 n) {
    uint64 addr = (uint64)PAGING_LOCATION_END;
    uint64 size = (uint64)n * 4096;
    if (n == 0 || addr + size > alloc_limit) {
        kprint("MEM: alloc_pages(");
        kprint_dec(n);
        kprint(" pages) failed, next ");
        kprint_long2hex(addr, ", limit ");
        kprint_long2hex(alloc_limit, "\n");
        return 0;
    }
    PAGING_LOCATION_END = (uint32)(addr + size);
    return addr;
}

// Allocate 64KB BSP stack from high memory, return stack top
uint64 alloc_bsp_stack(void) {
    uint64 base = alloc_pages(16);      // 16 pages = 64KB
    if (base == 0) {
        // kmain switches RSP to the return value; no stack, no boot.
        kprint("MEM: no memory for BSP stack, halting\n");
        for (;;) __asm__ volatile("cli; hlt");
    }
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

// Allocate a new page-table page from above PAGING_LOCATION_END.
// Returns 0 when alloc_pages fails.
static uint64 alloc_page_table(void) {
    uint64 addr = alloc_pages(1);
    if (addr == 0) return 0;
    // Zero all 512 entries (4KB page)
    volatile uint64 *p = (volatile uint64 *)addr;
    for (int i = 0; i < 512; i++) p[i] = 0;
    return addr;
}

// Identity-map [phys_start, phys_start+size) as uncacheable MMIO.
// Returns 0 on success, -1 if a page-table page could not be allocated
// (pages mapped before the failure stay mapped).
int map_mmio_range(uint64 phys_start, uint64 size) {
    uint64 end = phys_start + size;
    uint64 pml4t_addr = (uint64)PML4T_LOCATION;
    int rc = 0;

    for (uint64 vaddr = phys_start; vaddr < end; vaddr += 4096) {
        uint32 pml4_idx = (vaddr >> 39) & 0x1FF;
        uint32 pdpt_idx = (vaddr >> 30) & 0x1FF;
        uint32 pd_idx   = (vaddr >> 21) & 0x1FF;
        uint32 pt_idx   = (vaddr >> 12) & 0x1FF;

        volatile uint64 *pml4 = (volatile uint64 *)pml4t_addr;

        // Check/create PDPT entry
        if (!(pml4[pml4_idx] & PTE_PRESENT)) {
            uint64 new_pdpt = alloc_page_table();
            if (new_pdpt == 0) { rc = -1; break; }
            pml4[pml4_idx] = new_pdpt | PTE_PRESENT | PTE_WRITABLE | PTE_USER | PTE_PWT;
        }
        volatile uint64 *pdpt = (volatile uint64 *)(pml4[pml4_idx] & 0x000FFFFFFFFFF000ULL);

        // Check/create PD entry
        if (!(pdpt[pdpt_idx] & PTE_PRESENT)) {
            uint64 new_pd = alloc_page_table();
            if (new_pd == 0) { rc = -1; break; }
            pdpt[pdpt_idx] = new_pd | PTE_PRESENT | PTE_WRITABLE | PTE_USER | PTE_PWT;
        }
        volatile uint64 *pd = (volatile uint64 *)(pdpt[pdpt_idx] & 0x000FFFFFFFFFF000ULL);

        // Check/create PT entry
        if (!(pd[pd_idx] & PTE_PRESENT)) {
            uint64 new_pt = alloc_page_table();
            if (new_pt == 0) { rc = -1; break; }
            pd[pd_idx] = new_pt | PTE_PRESENT | PTE_WRITABLE | PTE_USER | PTE_PWT;
        }
        volatile uint64 *pt = (volatile uint64 *)(pd[pd_idx] & 0x000FFFFFFFFFF000ULL);

        // Map the page: identity map with cache disable (PCD) for MMIO
        if (!(pt[pt_idx] & PTE_PRESENT)) {
            pt[pt_idx] = vaddr | PTE_PRESENT | PTE_WRITABLE | PTE_PCD | PTE_PWT;
        }
    }

    // Flush TLB by reloading CR3
    uint64 cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    __asm__ volatile("mov %0, %%cr3" : : "r"(cr3));

    if (rc != 0) {
        kprint_long2hex(phys_start, " MEM: map_mmio_range failed at base\n");
    }
    return rc;
}

// Map APIC MMIO region (legacy wrapper). Called from kmain; the LAPIC
// and IOAPIC are required, so a failure halts the boot.
void map_mmio_region(void) {
    if (map_mmio_range(0xFEC00000ULL, 0x300000ULL) != 0) {
        kprint("MEM: cannot map LAPIC/IOAPIC MMIO, halting\n");
        for (;;) __asm__ volatile("cli; hlt");
    }
    kprint("MEM: MMIO region 0xFEC00000-0xFEF00000 mapped\n");
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
