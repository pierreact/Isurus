#ifndef PAGING_H
#define PAGING_H
#include <types.h>

// Replaces the 32-bit boot identity map with one that maps only RAM
// (E820 usable and ACPI ranges, all of them, also above 4 GB) with the
// largest page that fits. Boot-time only, before acpi_init.
void paging_init(void);

// Map the 4 KB pages of [phys, phys+size) that are not mapped yet, as
// uncacheable. For firmware data outside E820 RAM (ACPI tables in
// reserved memory). Mapped pages are left unchanged.
void paging_map_if_unmapped(uint64 phys, uint64 size);

// Lowest E820 RAM address at or above 4 GB, 0 if there is none.
uint64 paging_first_ram_above_4g(void);

#endif
