# Memory Map

**Audience:** Kernel developers. This is the canonical reference for physical memory layout. For debugging procedures, see `DEBUG.md`. For architecture decisions, see `ARCHITECTURE.md`.

## Layout

All addresses below are approximate (~) where they depend on kernel binary size. Exact values shift as code grows. The kernel binary is currently ~40 KB (80 sectors).

| Address | Starts at | Size | Purpose | Status |
|---------|-----------|------|---------|--------|
| 0x0000-0x03FF | 0 KB | 1 KB | IVT (unused after boot, preserved) | USED |
| 0x0400-0x04FF | 1 KB | 256 B | BIOS Data (unused after boot) | USED |
| 0x0500-0x05FF | 1.25 KB | 256 B | - | FREE |
| 0x0600-0x07FF | 1.5 KB | 512 B | Relocated Bootsector (unused after boot) | FREE |
| 0x1000-~0xAFFF | 4 KB | ~40 KB | Kernel .text + .data (binary) | USED |
| 0x7C00-0x7DFF | 31 KB | 512 B | Bootsector (BIOS load, overwritten by kernel) | TEMP |
| ~0xB000 | ~44 KB | 1 KB | MEMMAP_START (E820 memory map, in .bss) | USED |
| ~0xB400 | ~45 KB | 4 KB | IDT (IDT64_BASE, in .bss) | USED |
| ~0xC400 | ~49 KB | 4 KB | Temp stack (32-bit and early 64-bit, in .bss) | USED |
| ~0xD400-~0x9F000 | ~53 KB | ~580 KB | Heap (dynamic: kernelEnd to 0x9F000) | FREE |
| 0x9F000-0x9FFFF | 636 KB | 4 KB | AP trampoline (copied at runtime) | TEMP |
| 0xA0000-0xBFFFF | 640 KB | 128 KB | VGA (text buffer at 0xB8000) | HW |
| 0xC0000-0xFFFFF | 768 KB | 256 KB | BIOS ROM / ACPI tables | HW |
| 0x100000 | 1 MB | 4 KB | PML4T | USED |
| 0x101000+ | 1 MB + 4 KB | 5-20 MB | PDPT/PDT/PT of the 32-bit boot map (unused after paging_init, never freed) | USED |
| (bump range) | - | a few pages | Runtime page tables (paging_init), PML4 below 32 MB | USED |
| PAGING_END+ | ~6-21 MB | 64 KB | BSP stack (allocated at runtime) | USED |
| (after BSP stack) | ~6-21 MB | 16 KB each | AP stacks (allocated per CPU at runtime) | USED |
| (after AP stacks) | - | 104 B x 16 | TSS array (per-CPU, in BSS) | USED |
| (after TSS) | - | 64 KB each | BSP task kernel stacks (allocated per task) | USED |
| (after kstacks) | - | 64 KB each | BSP task user stacks (allocated per task) | USED |
| (after stacks) | - | - | - | FREE |
| 0xFEC00000-0xFEF00000 | 4076 MB | 3 MB | APIC MMIO (IOAPIC + LAPIC, mapped at runtime) | HW |

### Runtime-only areas (used temporarily during AP startup)

| Address | Size | Purpose | Status |
|---------|------|---------|--------|
| 0x9F000-0x9FFFF | 4 KB | AP trampoline code + data page | TEMP |
| 0x9FFF0-0x9FFF7 | 8 B | AP trampoline: CR3 value (patched by BSP) | TEMP |
| 0x9FFF8-0x9FFFF | 8 B | AP trampoline: percpu base ptr (patched by BSP) | TEMP |

## Constants

```
KERNEL_START    = 0x1000
BOOTSECT_RELOC  = 0x0600
TRAMPOLINE_BASE = 0x9F000
HEAP_START      = kernelEnd (dynamic, page-aligned)
HEAP_END        = 0x9F000
PML4T_LOCATION  = 0x100000
LAPIC_BASE      = 0xFEE00000
IOAPIC_BASE     = 0xFEC00000
```

## Free Zones

Low memory: kernelEnd to 0x9F000 (~580 KB) - used as heap
High memory: PAGING_LOCATION_END to the alloc_pages limit (end of the usable E820 entry, capped at APP_LOAD_BASE 0x2000000) - bump allocator, never freed, see architecture.md "Page allocator"
App images: 0x2000000+ (16 MB per app slot; the part after the file, the image .bss, is zeroed per launch)
AP data regions: 128 KB per AP in high memory (alloc_pages at boot, zeroed per launch, ThreadMeta.data_base; interim until per-thread page tables)

## Memory types

The page-level type is the PAT entry selected by the leaf entry's PAT, PCD and PWT bits. The kernel does not program IA32_PAT; it uses the power-on value (0x0007040600070406: entry 0 WB, entry 3 UC). The MTRRs set by firmware can lower the type further (an MTRR UC range wins).

| Range | Bits | Type |
|-------|------|------|
| RAM (`paging_init`: 0-2 MB, E820 usable, ACPI reclaimable, ACPI NVS, also above 4 GB) | PWT=0 PCD=0 | WB |
| MMIO (`map_mmio_range`: LAPIC/IOAPIC/HPET 0xFEC00000-0xFEF00000, legacy VGA 0xA0000-0xBFFFF, PCI ECAM, device BARs, ACPI reset register in memory space) | PWT=1 PCD=1 | UC |
| ACPI tables outside E820 RAM (`paging_map_if_unmapped`, from acpi.c) | PWT=1 PCD=1 | UC |

Boot prints `IA32_PAT` and the type of one RAM, IOAPIC, LAPIC and VGA page, and of the first RAM page at or above 4 GB if there is one (`MEM: page type ...`).

## Page tables

The 32-bit boot code builds an identity map with 4 KB pages (asm32_setup_64bits_paging_structures.inc). It is used only until `paging_init` (src/kernel/paging.c), called after `heap_init`, builds the runtime tables and loads CR3:

- 0-2 MB: 4 KB pages (kernel, AP trampoline, EBDA and BIOS area, VGA; mixed MTRR types).
- Each E820 usable, ACPI reclaimable and ACPI NVS range: 1 GB pages where the range covers a whole aligned GB and CPUID 0x80000001 EDX bit 26 is set, else 2 MB pages where it covers a whole aligned 2 MB, else 4 KB pages. Usable ranges round inward to 4 KB, ACPI ranges outward.
- Nothing else is mapped. MMIO is mapped only by `map_mmio_range`, in 4 KB pages; it halts if the address falls in a large page (a device range reported as RAM).
- All tables come from `alloc_pages`, below 32 MB: APs load CR3 in 32-bit mode (ap_trampoline.asm).
- All entries have the User bit until per-thread page tables: BSP ring-3 tasks call kernel and driver code directly.
- Boot log: `PAGING: RAM <bytes> bytes mapped, pages 1G <n> 2M <n> 4K <n>`.

## Stack

| Mode | Address | Notes |
|------|---------|-------|
| 16-bit (bootsector) | SS=0x8000, SP=0xF000 -> 0x8F000 | |
| 16-bit (kernel) | SS=0x8000, SP=0xF000 -> 0x8F000 | |
| 32-bit (protected mode, transient) | ESP=0x80000 (flat SS) | Set explicitly in `kernel32:` (kmain.s). Transient: pushes during PIC/IDT/paging setup land in free conventional RAM between kernel end and VGA; the region is later reused by the heap. The small GDT flat-data descriptor at `gdt+0x10` has granularity byte `0xCF` (G=1, B=1) so stack ops use the full 32-bit ESP. |
| 64-bit (BSP early) | temp_stack_end (0x16000, in .bss) | Until `alloc_bsp_stack` runs |
| 64-bit (BSP) | PAGING_LOCATION_END+ | 64 KB, allocated from high memory |
| 64-bit (APs) | percpu[i].stack_top | 16 KB per AP, allocated from high memory |
| 64-bit (BSP ring 3) | task.user_rsp | 64 KB per task, allocated from high memory |
| 64-bit (BSP ring 0, syscall) | task.kstack_top | 64 KB per task kernel stack |
| 64-bit (AP ring 3) | ap_user_stacks[i] + 64 KB | 64 KB per AP, allocated at boot in cpu_init, reused by every launch |

**Why the 32-bit stack is pinned to 0x80000:** on the 16 -> 32 bit
transition the CPU keeps ESP's low 16 bits from real-mode SP
(0xF000) but loads a flat SS. Without an explicit `mov esp` the
32-bit stack lands at linear 0xF000 - inside kernel `.text` -
silently corrupting the kernel as every `call` pushes a return
address. Fixed 2026-04-22 (kmain.s: `mov esp, 0x80000` at
`kernel32:`, plus `gdt+0x10` granularity 0x8F -> 0xCF so pushes
use ESP not SP). Before modifying anything around the 32-bit
transition, verify bytes at `0xEF?? - 0xF000` in a fresh boot
snapshot match `kernel.bin` - any drift means the transition is
re-corrupting kernel text.

## SMP / NUMA

- Up to 16 CPUs supported (MAX_CPUS)
- QEMU configured with 4 CPUs: 2 sockets x 2 cores
- NUMA node 0: CPUs 0-1 (1 GB)
- NUMA node 1: CPUs 2-3 (1 GB)
- APs are woken via INIT-SIPI-SIPI to trampoline at 0x9F000
- APs park with interrupts disabled (cli; hlt) - BSP owns all IRQs

## E820 Format

24 bytes per entry at MEMMAP_START:
- qword: base
- qword: length
- dword: type
- dword: ACPI flags
