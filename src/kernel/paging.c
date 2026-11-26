#include <types.h>
#include <drivers/monitor.h>
#include <kernel/mem.h>
#include <kernel/paging.h>

extern uint8 data_counter_mmap_entries;
extern uint32 MEMMAP_START;
extern uint32 PML4T_LOCATION;

#define PTE_PRESENT   0x01ULL
#define PTE_WRITABLE  0x02ULL
#define PTE_USER      0x04ULL
#define PTE_PS        0x80ULL                  // 1 GB page in a PDPTE, 2 MB page in a PDE
#define PTE_ADDR      0x000FFFFFFFFFF000ULL
#define PTE_FLAGS     (PTE_PRESENT | PTE_WRITABLE | PTE_USER)   // same as the boot map; PWT=PCD=0: WB

#define SIZE_4K       0x1000ULL
#define SIZE_2M       0x200000ULL
#define SIZE_1G       0x40000000ULL
#define LOW_4K_END    SIZE_2M                  // 0-2 MB always mapped with 4 KB pages
#define ADDR_4G       0x100000000ULL

static volatile uint64 *pml4;
static int use_1g;
static uint64 count_1g, count_2m, count_4k, count_tables;
static uint64 high_ram;                        // first RAM address >= 4 GB, 0 if none

static void halt(char *msg, uint64 addr) {
    kprint("PAGING: ");
    kprint(msg);
    kprint_long2hex(addr, "\nPAGING: halting\n");
    for (;;) __asm__ volatile("cli; hlt");
}

static uint64 new_table(void) {
    uint64 t = alloc_pages(1);                 // below 32 MB: APs load CR3 in 32-bit mode
    memset((void *)t, 0, SIZE_4K);
    count_tables++;
    return t;
}

// Table referenced by t[idx], created if absent. NULL if t[idx] is a
// large page (the address is already mapped by it).
static volatile uint64 *sub_table(volatile uint64 *t, uint32 idx) {
    if (!(t[idx] & PTE_PRESENT))
        t[idx] = new_table() | PTE_FLAGS;
    if (t[idx] & PTE_PS) return NULL;
    return (volatile uint64 *)(t[idx] & PTE_ADDR);
}

// Identity-map [va, end) write-back, both 4 KB aligned. Uses a 1 GB or
// 2 MB page where the range covers a whole aligned page and the slot is
// free; 4 KB pages otherwise. Addresses already mapped are skipped, so
// overlapping E820 entries are harmless.
static void map_ram(uint64 va, uint64 end, int large) {
    while (va < end) {
        volatile uint64 *pdpt = sub_table(pml4, (va >> 39) & 0x1FF);
        uint32 i1 = (va >> 30) & 0x1FF;
        if (large && use_1g && !(va & (SIZE_1G - 1)) && end - va >= SIZE_1G
            && !(pdpt[i1] & PTE_PRESENT)) {
            pdpt[i1] = va | PTE_FLAGS | PTE_PS;
            count_1g++;
            va += SIZE_1G;
            continue;
        }
        volatile uint64 *pd = sub_table(pdpt, i1);
        if (!pd) { va = (va & ~(SIZE_1G - 1)) + SIZE_1G; continue; }

        uint32 i2 = (va >> 21) & 0x1FF;
        if (large && !(va & (SIZE_2M - 1)) && end - va >= SIZE_2M
            && !(pd[i2] & PTE_PRESENT)) {
            pd[i2] = va | PTE_FLAGS | PTE_PS;
            count_2m++;
            va += SIZE_2M;
            continue;
        }
        volatile uint64 *pt = sub_table(pd, i2);
        if (!pt) { va = (va & ~(SIZE_2M - 1)) + SIZE_2M; continue; }

        uint32 i3 = (va >> 12) & 0x1FF;
        if (!(pt[i3] & PTE_PRESENT)) {
            pt[i3] = va | PTE_FLAGS;
            count_4k++;
        }
        va += SIZE_4K;
    }
}

static int cpu_has_1g_pages(void) {
    uint32 a, b, c, d;
    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(0x80000001), "c"(0));
    return (d >> 26) & 1;                      // CPUID 0x80000001 EDX bit 26: Page1GB
}

void paging_init(void) {
    use_1g = cpu_has_1g_pages();
    pml4 = (volatile uint64 *)new_table();

    // 0-2 MB in 4 KB pages: kernel, trampoline, EBDA and BIOS area (RSDP
    // scan), VGA. MTRR types are mixed here, so no large page.
    map_ram(0, LOW_4K_END, 0);

    uint64 ram_bytes = 0;
    E820E *map = (E820E *)&MEMMAP_START;
    for (uint32 i = 0; i < data_counter_mmap_entries; i++) {
        uint32 type = map[i].type;
        if (type != E820_TYPE_FREE && type != E820_TYPE_ACPI_RECLAIMABLE
            && type != E820_TYPE_ACPI_NVS)
            continue;
        uint64 base = map[i].base;
        uint64 end = base + map[i].length;
        // Usable RAM rounds inward (never map a partial non-RAM page as
        // WB); ACPI ranges round outward (the tables must be readable).
        if (type == E820_TYPE_FREE) {
            base = (base + SIZE_4K - 1) & ~(SIZE_4K - 1);
            end &= ~(SIZE_4K - 1);
        } else {
            base &= ~(SIZE_4K - 1);
            end = (end + SIZE_4K - 1) & ~(SIZE_4K - 1);
        }
        if (end <= base) continue;
        ram_bytes += end - base;
        uint64 hi = base < ADDR_4G ? ADDR_4G : base;
        if (hi < end && (high_ram == 0 || hi < high_ram)) high_ram = hi;
        map_ram(base, end, 1);
    }

    __asm__ volatile("mov %0, %%cr3" : : "r"((uint64)pml4) : "memory");
    PML4T_LOCATION = (uint32)(uint64)pml4;

    kprint("PAGING: RAM ");
    kprint_long2hex(ram_bytes, " bytes mapped, pages 1G ");
    kprint_dec((uint32)count_1g);
    kprint(" 2M ");
    kprint_dec((uint32)count_2m);
    kprint(" 4K ");
    kprint_dec((uint32)count_4k);
    kprint(", tables ");
    kprint_dec((uint32)count_tables);
    kprint(use_1g ? " (1G pages supported)\n" : " (1G pages not supported)\n");
    kprint_long2hex((uint64)pml4, " PAGING: PML4\n");
}

// Walk the current tables. Returns 1 if va is mapped.
static int is_mapped(uint64 va) {
    volatile uint64 *t = pml4;
    for (int level = 3; level >= 0; level--) {
        uint64 e = t[(va >> (12 + 9 * level)) & 0x1FF];
        if (!(e & PTE_PRESENT)) return 0;
        if (level == 0 || (level < 3 && (e & PTE_PS))) return 1;
        t = (volatile uint64 *)(e & PTE_ADDR);
    }
    return 0;
}

void paging_map_if_unmapped(uint64 phys, uint64 size) {
    if (!pml4) halt("paging_map_if_unmapped before paging_init, addr ", phys);
    uint64 end = phys + size;
    for (uint64 p = phys & ~(SIZE_4K - 1); p < end; p += SIZE_4K)
        if (!is_mapped(p))
            map_mmio_range(p, SIZE_4K);
}

uint64 paging_first_ram_above_4g(void) {
    return high_ram;
}
