# Isurus OS

x86-64 kernel (Zero-Interrupt NUMA Cluster), NASM + C, AGPLv3 / commercial.

## Doctrine: real-hardware check
QEMU is the dev environment, not the target. Every plan and implementation answers:
1. Does this rely on a QEMU-specific shortcut, default, or value? Where?
2. What does real hardware (modern Intel/AMD, BIOS or UEFI) do differently?
3. Smallest forward-compatible mitigation now, and the exact followup needed for bare metal.

## Architecture
- Bootsector loads kernel to 0x1000; 16-bit (IVT, E820) -> 32-bit (GDT, PIC, IDT, paging) -> 64-bit (long mode, mm, shell)
- BSP (CPU 0): kernel, all interrupts, cooperative ring-3 tasks (shell, coordinator, telnet, ...), sti/hlt loop
- APs: one pinned thread per core, never interrupted, no context switching
- CPUs at max frequency: no C-states deeper than C1, no P-state scaling
- Memory layout: docs/developer/memory-map.md

## Key files
- src/boot/bootsector.asm, src/boot/kmain.s, src/boot/asm{16,32,64}/*.inc
- src/{kernel,arch,drivers,net,fs,shell}/*.c, headers in src/include/<subsystem>/
- src/link.ld (kernel @ 0x1000)
- docs/README.md: documentation index

## Memory rules
BSP (kmalloc/kfree, kernel + BSP ring 3 only):
- kmalloc(size) returns exactly ceil(size/4096) contiguous page-aligned 4 KB blocks; minimum one page
- Pack small objects into one page; never many sub-page kmallocs
- Need >4 KB: pass the real size; never loop kmalloc(4096) (blocks not adjacent)
- kfree takes the exact pointer kmalloc returned, frees the whole span
- Long-lived DMA / paging / per-AP stacks: alloc_pages (src/kernel/mem.c, bump allocator, never freed)
- alloc_pages is boot-time only; never per read, per launch or per request: use a static buffer or one allocated at boot
- alloc_pages never returns 0: it halts when out of range, callers do not check (docs/developer/architecture.md "Page allocator")

AP ring-3 threads:
- No malloc; sys_kmalloc forbidden from AP context
- Each AP owns node_RAM / cores_on_node of its NUMA node
- Working set is one static struct (preferred: explicit named fields) or static arrays, sized at build time; no runtime allocation on the data path
- Interim, until per-thread page tables: all cores of an app share one image (.data/.bss, one CR3), so a writable `static` is shared by every core. Put the per-core struct in the private data region `meta->data_base` / `meta->data_size` (128 KB, zeroed per launch) instead
- Allowed: libc bump/slab helper inside the app's own region, never reaching the kernel
- Reference: apps/dpdk_l2/main.c (packet pool at `meta->data_base`); check: apps/percore

## Build and test
Run in repo root, always via `devbox run -- <cmd>` (enforced by hook):
- scripts/compile.sh: build only
- scripts/compile_qemu.sh: VGA + QEMU monitor
- scripts/compile_qemu_serial.sh: shell on stdio
- scripts/compile_qemu_debug.sh: no reboot on crash
- scripts/compile_qemu_tap.sh: mgmt NIC on tap0
- test/run_tests.sh: build, headless boot, serial + TAP tests

Logs (logs/, gitignored): compile.log, compile_logs.log, serial.log, qemu.log, tests.log, tap0_capture.txt; also test/serial_output.log

## Known traps
- Layout-sensitive test failures (flip with cosmetic changes, `-O` level, unused code): suspect boot code corrupting kernel `.text` first. See docs/developer/debugging.md "Layout-sensitive failures".
- The small GDT in kmain.s is live during the 32-bit phase; GDT64 only after LME.
- QEMU tolerance != real-hardware correctness.

## Behavior
- Concise, direct, bullet lists; no AI phrasing ("let me", "I'll help", "great job"), ASCII only: no emojis, em/en dashes, curly quotes, unicode arrows/ellipsis (enforced by hooks on edits, commits, replies)
- Ask before unrequested work or when unclear; never assume
- Never fabricate metrics or benchmarks; measured data only
- Read files/logs yourself; never ask the user to
- Preserve existing comments when refactoring unless irrelevant to the new code

## Style
- Code comments: only critical/non-obvious
- Docs: lean reference format, no tutorials
- Commits: direct, technical
- CHANGELOG.md: updated in every commit that changes behavior, fixes bugs, or adds features (enforced by hook); bullets, one change per line
- docs/project/todo.md: move completed items to Done

## State
- Working: boot, ring-3 BSP shell (SYSCALL/SYSRET, TSS, cooperative tasks), FAT32, IDE, kmalloc, SMP AP bringup, ACPI (MADT/SRAT/SLIT/HPET/FADT/MCFG/DMAR/IVRS + AML _BBN/_PXM), PCI ECAM with NUMA proximity, virtio-net, NIC abstraction + per-numa/per-core assignment, per-CPU ThreadMeta, Q35
- Not yet: AP ring-3 threads, per-thread page tables, clustering, DSM, DPDK, SPDK

## Revisit later
- BSP kmalloc is page-multiple first-fit, no subpage pool. If small allocations dominate, add a slab/subpage pool inside one kmalloc'd page (O(1) vs O(num_blocks)).
