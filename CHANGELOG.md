# Changelog

## [2026-11-26] - Memory: NUMA-local boot allocation

### Added
- alloc_pages_node(node, n): per-node bump pools built after acpi_init from E820 usable RAM intersected with SRAT memory ranges, above the app slots (96 MB). Same contract as alloc_pages: boot-time only, never freed, halts when full.
- SRAT ranges flagged hot-pluggable or non-volatile are not used; overlapping ranges halt the boot.
- Node unknown to SRAT uses the BSP's node; a node without memory uses the nearest node by SLIT distance (logged).
- sys.thread.ls column MEM (SRAT node of each AP's stacks and data region) and DATA (data region address); sys.mem.info prints bytes used per node pool.
- Tests: node-1 cores and NICs get node-1 memory on the 2-node QEMU; above 4 GB in the 6 GB run; node pools unchanged across app launches.

### Changed
- AP kernel stacks, AP user stacks and data regions: allocated on the core's node (were all below 32 MB, node 0).
- virtio rings and RX/TX buffers: allocated on the NIC's node.
- BSP task stacks and BSP L2 packet pools: allocated on the BSP's node, above 96 MB.
- alloc_pages keeps page tables, the BSP stack and AHCI below 32 MB.

## [2026-11-25] - CI: Gemini review model gemini-4-hydrogen

### Changed
- Gemini review main model gemini-4-hydrogen, fallback gemini-3.8-flash (both free tier). gemini-3.8-flash returned 503 in 3 of 3 runs on 2026-11-25.

## [2026-11-25] - CI: Gemini comment-only PR review

### Added
- Workflow gemini-review.yml: Gemini reviews same-repository PRs on open, reopen, ready for review and push (30 min cooldown), on a `/gemini-review` comment from a collaborator, or by manual dispatch. Gemini has read-only file tools and no GitHub token; a separate job posts one review with event COMMENT. Fork PRs are skipped.
- On HTTP 503 or 429 from gemini-3.8-flash the review runs once more with gemini-3.6-flash (both free tier). When no review can be made, a comment-only review states the reason (overloaded, quota, missing secret, API error) and links the run.
- Review prompt .gemini/commands/isurus-review.toml (x86-64, SMP, NUMA, DMA priorities; severity and confirmed/potential per finding), GEMINI.md (imports CLAUDE.md, points to docs), .geminiignore.
- Docs: developer/gemini-review.md (setup, permissions, security model, cost).

## [2026-11-24] - Paging: runtime page tables map all RAM with large pages

### Fixed
- RAM above 4 GB was never mapped: the 32-bit boot code picks the highest E820 entry by comparing only the low 32 bits of the base. Measured before: PAGING_LOCATION_END 0x908000 with both -m 2G and -m 8G (4 GB mapped in both cases). The runtime tables map every E820 RAM range, also above 4 GB.

### Changed
- New paging_init (src/kernel/paging.c), called after heap_init, replaces the 32-bit boot identity map: 0-2 MB in 4 KB pages, E820 usable and ACPI ranges with 1 GB, 2 MB or 4 KB pages, nothing else. Measured in QEMU: -m 2G maps 2 GB with 1022 2 MB pages and 991 4 KB pages (default CPU, no 1 GB pages); -m 6G with pdpe1gb adds four 1 GB pages above 4 GB.
- MMIO and reserved ranges are no longer mapped as RAM. map_mmio_range creates their pages (with the User bit, as all pages until per-thread page tables) and halts if an MMIO address falls in a RAM large page.
- ACPI tables are mapped UC before they are read when they are outside E820 RAM; tables longer than 4 MB are ignored. The ACPI reset register is mapped UC when it is in memory space.

### Added
- Boot log: page counts per size, and the type of the first RAM page at or above 4 GB. Tests: 2 MB pages in the main boot; the TAP phase boots with -m 6G and -cpu qemu64,+pdpe1gb and checks 1 GB pages and WB RAM above 4 GB.

## [2026-11-22] - Paging: RAM write-back, MMIO uncacheable

### Fixed
- The boot identity map set PWT on every entry. With the power-on PAT (0x0007040600070406) PWT=1 selects entry 1, write-through: every store to RAM went to memory. Measured at boot in QEMU before the fix: RAM, IOAPIC and LAPIC pages all WT. RAM pages are now WB (PWT=0, PCD=0).
- MMIO pages already covered by the boot identity map kept the boot map type, because map_mmio_range skipped present entries. Measured: LAPIC and IOAPIC pages WT; PCI ECAM (0xB0000000 in QEMU) goes through the same path. map_mmio_range now sets UC (PCD|PWT) on present entries too. Legacy VGA 0xA0000-0xBFFFF is mapped UC at boot.

### Added
- Boot log: IA32_PAT value and the page-level type of one RAM, IOAPIC, LAPIC and VGA page. Tests check WB for RAM and UC for the three MMIO pages.
- Docs: memory-map.md "Memory types".

## [2026-11-22] - Apps: per-core data region, .bss zeroed per launch

### Fixed
- dpdk_l2 and dpdk_l3 used one `static` 128 KB packet pool for all cores of the app (the image is loaded once and shared). Each core zeroed and re-initialized the same pool while the others used it. Measured with apps/percore using a static buffer: 96922 to 307596 words per core overwritten by other cores; with the data region: 0.
- The kernel never zeroed an app's `.bss` (the flat binary does not contain it), so `.bss` started with the previous app's data. app_image_load now zeroes the app region after the file, and refuses a file larger than the region (APP_REGION_SIZE).

### Added
- ThreadMeta `data_base` / `data_size`: a private 128 KB data region per AP (AP_DATA_SIZE), allocated at boot with the user stack (ap_user_alloc, from cpu_init), zeroed before every launch (ap_user_prepare). Interim: removed when per-thread page tables give each core a private copy of .data/.bss. Layout asserted in both cpu.h and apps/libc/isurus.h.
- apps/percore and conf/percore.ini: test app that checks the region and `.bss` are zero at entry and the region is private. Run twice in test/run_tests.sh.

### Changed
- dpdk_l2, dpdk_l3: packet pool at `meta->data_base`. dpdk_l2 MAX_ITERATIONS comment: the dispatch is parallel; the bound stays because no command stops an app.
- loader_exec and app_launch share app_image_load and ap_user_prepare.
- Docs: architecture.md "App memory (until per-thread page tables)", ThreadMeta fields; memory-map.md; tutorial.md "Per-core state". CLAUDE.md AP rules: until per-thread page tables, per-core state in the data region, not in `static` variables.

## [2026-11-21] - alloc_pages: bounded, no per-operation leaks

### Fixed
- Every disk read leaked alloc_pages memory: ahci_read_sectors allocated a DMA buffer per call, and iso9660 allocated one per path component (find_entry), per directory listing and per file read. app_launch and loader_exec allocated new 64 KB user stacks per core on every launch. Measured before the fix: the bump pointer grew 832 KB over 3 demo_app launches and one directory listing. With enough launches it would have reached the app region at 0x2000000 with no error.
- alloc_pages had no upper bound. It now stops at the end of the usable E820 entry that contains PAGING_LOCATION_END, capped at APP_LOAD_BASE (0x2000000). A request that does not fit prints a message and halts.

### Changed
- AHCI: one 64 KB DMA bounce buffer allocated in ahci_init; reads are chunked through it. IDENTIFY uses it too.
- iso9660: one static 2 KB sector buffer. Directories are walked one sector at a time (records never cross a sector); file reads put whole sectors directly into the caller's buffer and only the partial last sector goes through the static buffer. Malformed directory records (length < 34 or past the sector) stop the walk.
- AP user stacks: one 64 KB stack per AP, allocated at boot in cpu_init and reused by every launch. An app launch allocates nothing.
- alloc_pages is boot-time only and never returns 0, so its callers have no failure path.
- Tests: bump pointer must not move across 3 demo_app launches and one directory listing.
- Docs: architecture.md "Page allocator (alloc_pages)", memory-map.md high-memory range and AP user stacks. CLAUDE.md memory rules: alloc_pages boot-time only, halts when out of range.

## [2026-11-21] - App launch: NIC lock and duplicate cores

### Fixed
- app_launch set the slot's nic_locked[] during the NIC conflict check, then zeroed the whole slot with memset before filling it, so no NIC was ever locked. A second app on a core sharing the NIC (per-numa mode: cores 2 and 3 on node 1) was loaded while the first ran. The locks are now collected in a local array and copied into the slot after the memset.
- app_launch accepted a core listed twice in a manifest (cores=1,1): core_to_app is filled only after validation. Such manifests are now refused with "APP: core N listed twice".

### Added
- conf/niclk_a.ini, conf/niclk_b.ini, conf/dupcore.ini: test manifests.
- Tests: second app on a locked NIC refused and not loaded, first app still finishes; duplicate core refused and not loaded.

## [2026-11-21] - kmalloc multi-page spans

### Fixed
- kmalloc returned one 4 KB block regardless of size. It now returns ceil(size/4096) contiguous blocks (first-fit). Callers that asked for more than 4 KB wrote into the next block: pci_ids_init (data/pci.ids is 4552 bytes) on every boot, and FAT32 cluster buffers on volumes with clusters larger than 4 KB.
- kfree freed one block. It now frees the whole span, using a per-block span length recorded by kmalloc. A pointer that is not the start of an allocation is reported and ignored.

### Changed
- sys.mem.test: checks multi-page allocation, alignment, disjoint spans, full-span writes, span reuse after free, refusal of kfree inside a span, oversize failure, and used-block accounting. Prints "Heap test: PASS" or "Heap test: FAIL (n checks)".

## [2026-11-21] - Zero .bss at boot

### Fixed
- Kernel .bss was never zeroed. On CD boot the bootsector copy zeroes it only up to 0x1000 + KSIZE*512 (0x18000 when found); above it, .bss held leftover boot image bytes (kernel .rodata). Static variables placed there started non-zero: with a 256-byte larger mem.o .bss, input_head read 0x12489 (shell consumed garbage as the first command) and drive_sectors read "cdef" (FAT32 mounted at /mnt/IDE with no IDE controller). kernel32 in src/boot/kmain.s now zeroes [__bss, kernelEnd) before any .bss use. On real hardware RAM content at boot is undefined, so this applies to every boot path.

### Changed
- Tests: "Shell runs in ring 3" matches "Current privilege level: ring 3" (the old pattern "ring 3" also matched a boot message and passed when the command was lost). New absence checks: no "Unknown command" in either phase, no /mnt/IDE FAT32 mount when no IDE controller is present.

## [2026-04-25] - Wake-up-latency infrastructure (PIT + virtio-net INTx)

### Context

Host -> guest ICMP under `compile_qemu.sh` showed 2-10 ms RTT with
high jitter. tcpdump on tap0 with `-ttt` confirmed the latency was
entirely in the guest reply path; tap0 itself adds <50 us. Two root
causes: (1) the kernel never reprograms the PIT, so it ran at QEMU's
default ~100 Hz (10 ms wake granularity); (2) no virtio-net IRQ was
delivered, so the only thing waking the BSP between commands was the
timer.

### Added

- `pit_init_periodic(uint32 hz)` in `src/arch/apic.c` (declared in
  `src/include/arch/apic.h`). Channel 0, mode 3, divisor =
  1193182/hz. Bounded to [50, 10000] Hz. NOT called from boot today
  -- on QEMU q35 with HPET in legacy-replacement mode (the default),
  HPET owns IRQ0 and the PIT writes are no-op. The helper is kept as
  the LAPIC-timer migration anchor and for bare-metal use where HPET
  legacy mode can be disabled.
- `lapic_bsp_id()` getter in `src/arch/apic.c`.
- `ioapic_route_irq` made non-static (declared in apic.h) so the net
  stack can unmask GSIs after PCI device init.
- `IRQ_VIRTIO_NET = 0x40` define in apic.h.
- `irq_pin` field on `PCIDevice` (PCI config 0x3D) plus
  `pci_find_gsi()` helper. The helper trusts PCI config 0x3C as the
  resolved GSI, which works on QEMU q35 (firmware writes the actual
  GSI). Real-hardware compatibility note in the helper's comment:
  bare metal needs `_PRT` parsing; that lands as a future milestone
  per the `Real-hardware compatibility check` doctrine in
  `ai.rules`.
- `nic_enable_intx(idx, vector)` in `src/net/nic.c`: looks up the
  NIC slot's PCI device, finds its GSI, registers the underlying
  virtio device for ISR ack via `virtio_isr_register`, and unmasks
  the IOAPIC entry with PCI INTx convention flags
  (level-triggered, active-low). Called from `l2_kern_init` for the
  mgmt NIC.
- `virtio_isr_register` / `virtio_net_isr_ack` in `src/net/virtio.c`:
  the asm ISR calls the ack fn, which reads the device's ISR-status
  byte to deassert INTx (legacy virtio convention).
- IDT slot 0x40 + `asm64_isr_virtio_net_handler` in
  `src/boot/asm64/asm64_setup_idt.inc`. Body: save GPRs, call
  `virtio_net_isr_ack`, bump `net_service_isr_count`, send LAPIC
  EOI, restore, iretq. Drain happens later in
  `net_service_tick` from `sys_wait_input`.
- `net_service_isr_count` (volatile uint64) bumped by the ISR;
  exposed in `sys.net.stats` via `NetServiceStats.isr_count` so we
  can observe whether IRQs are actually delivered.
- PCI `INTERRUPT_DISABLE` bit cleared in `virtio_pci_init_device`.

### QEMU result and real-hardware angle

**Latency reduction in QEMU: zero.** `isr_count` stays 0 across runs.
On QEMU q35 with `pxb-pcie` + `pcie-root-port`, virtio-net does not
deliver INTx; the PCIe path is MSI-X. The full INTx infrastructure
landed and is exercised at boot (banner: `NIC: slot 0 INTx GSI 10 ->
vector 0x40`), but no interrupts arrive. This was anticipated as a
risk in the `Real-hardware compatibility (Phase B)` plan section:
"PCIe-native devices may not raise INTx on some real boards ... For
real bare-metal we may need MSI-X support." That was stated for real
hardware and turns out also to be the QEMU-q35 behaviour.

This commit ships the **plumbing** (PCI INTx pin parse, GSI lookup,
IDT slot, ISR shape, IOAPIC public route, INTERRUPT_DISABLE bit,
ISR-ack registration). Most of it is reusable for the eventual MSI-X
support; the ISR shape and `isr_count` regression sentinel come over
unchanged. **No latency win is claimed for QEMU.** When MSI-X lands,
expect sub-millisecond passive ping latency without further changes
to the foundation.

### Real-hardware compatibility (per `ai.rules`)

- **PIT @ 1000 Hz**: would work on bare metal with HPET legacy mode
  off. Not called today; needs `hpet_disable_legacy_mode()` (future)
  before invocation. LAPIC timer is the better long-term answer.
- **PCI 0x3C as GSI**: QEMU writes the resolved GSI. Real hardware
  often writes 0xFF; the OS must walk `_PRT` (extend
  `src/arch/aml.c`). Tagged in the `pci_find_gsi` comment so the
  bare-metal bring-up grep can find it.
- **PCI INTx polarity flags**: hardcoded to active-low / level
  (PCI convention). Real boards may report different polarity via
  MADT ISO entries; revisit when bare-metal data is available.
- **MSI-X**: required for real bare-metal PCIe devices and for
  QEMU q35 pcie-root-port. Followup milestone; the current INTx
  shape provides the IDT slot, ISR pattern, and stats counter that
  MSI-X will reuse.

### ai.rules

Added a permanent doctrine line at the top of `ai.rules`:
"Real-hardware compatibility check" -- every plan and every
implementation must explicitly assess QEMU vs bare-metal behaviour
and call out the bare-metal mitigation. Saved to project memory so
future sessions inherit the rule.

## [2026-04-25] - BSP net_service foundation (passive packet servicing)

### Added
- New top-level `src/services/` tree: pluggable BSP services consuming the network stack. First and only inhabitant today is the foundation itself; future telnet daemon / Prometheus exporter / syslog receiver land here as siblings.
- `src/services/net_service.{c,h}`: BSP cooperative polling foundation. `net_service_tick()` drains every BSP-owned `L2Context` (mgmt + inter-node) up to 16 frames per context per tick. `net_service_drain(ctx)` is the public single-context drain that replaces the old `static drain_mgmt_nic` in `shell_net.c`. `NetServiceStats` (`ticks`, `frames_processed`, `frames_per_tick_max`, `ticks_with_zero_frames`) lives kernel-wide in net_service.c.
- Hook into `sys_handle_wait_input`'s hlt loop alongside `app_check_completion()`. The shell task's idle is the foundation's heartbeat; no new BSP task slot. Cooperative single-writer-per-`L2Context` invariant preserved.
- `net_service_init` called from `kmain.s` boot sequence after `l2_kern_init`.
- `src/services/README.md`: foundation contract, invariants, call-chain Mermaid diagram, future port -> handler shape sketch.
- `docs/developer/architecture.md` "Services" subsection with Mermaid call-chain and directory-layout diagrams. Mermaid renders natively on GitHub.
- pktrace point `PKT_NET_SERVICE_TICK` reserved for future per-tick traces (not stamped today; the existing per-frame stamps in `l2_poll` / `ip_rx` cover the work).

### Changed
- `shell_net.c`: deleted the static `drain_mgmt_nic`; the five call sites (`cmd_net_arp`, `cmd_net_stats`, plus the open-coded poll loops in `cmd_net_arping` / `cmd_net_ping`) now call `net_service_drain`.
- `sys.net.stats` extended with the `net_service` block (ticks, frames, max/tick, idle ticks).
- `compile_qemu.sh` (already on tap0 from the previous commit) now Just Works for host -> guest ping; no need to type sys.net.* in the guest. ARP and ICMP echo are answered passively.
- Tests: dropped the 1 Hz background-drain hack from the host -> guest ping phase. Replaced with a passive `ping -c 3 10.0.2.15` while the guest sits at the idle prompt; expects 0% loss. Added `net_service_ticks > 0`, `net_service_frames > 0`, and `frames_per_tick_max <= 16` (batch-cap regression sentinel) assertions. 52 -> 55 green.

### Out of scope (deferred, named for visibility)
- L4 port -> handler dispatch table (lands with first consumer: telnet, first UDP service, or Prometheus exporter).
- ARP table aging / TTL eviction; ARP probe (RFC 5227) handling; gratuitous ARP emission.
- ICMP error emission (Time Exceeded on TTL=0 forward, Destination Unreachable, Frag Needed).
- Promotion to a standalone BSP task slot. Revisit only if a long shell handler creates an unacceptable starvation gap; mitigation is to call `net_service_tick()` at the offender's loop boundary first.

## [2026-04-24] - Parallel app_launch

### Added
- ap_dispatch_async() + ap_work_done() in src/kernel/cpu.c: fire-and-forget AP dispatch so multiple cores can run ring 3 simultaneously. ap_dispatch stays as the blocking wrapper.
- app_check_completion() now scans active app slots, updates cores_done, and reaps the slot (clears core_to_app, releases nic_locked, prints "APP: <name> finished") once all cores have returned. Called from sys_wait_input's idle path so the shell reaps while waiting for input.
- app_launch prints a one-line BSP-serialized banner "APP: <name> ip=..." when the manifest carries an IP, so tests/users see a clean config line that does not get interleaved with per-core ready banners.

### Changed
- app_launch: sequential "dispatch + block for each core" replaced with async dispatch of all cores in parallel. The call returns as soon as every AP has been signalled; the slot stays active=1 until app_check_completion reaps it. dpdk_l2 with cores 1/2/3 now runs ~3x faster wall-clock.
- sys_handle_task_exit, sys_handle_nic_send, sys_handle_nic_recv, sys_handle_app_net_cfg: replaced "scan percpu for first in_usermode=1" with get_current_cpu(). The old pattern assumed one AP in ring 3 at a time; with parallel apps, multiple APs are in_usermode simultaneously and the wrong one got reaped on TASK_EXIT.
- Tests: serial output from concurrent cores interleaves at byte granularity; per-core line checks replaced with BSP-serialized banners (APP: dispatching cores:, APP: <name> ip=..., APP: <name> finished). Test waits bumped to account for wall-clock ~= max-core time under parallel share of host CPU. 47 -> 49 green.

## [2026-04-21] - IPv4 + ICMP Echo (OSI Layer 3)

### Added
- src/net/ip.c + src/include/net/ip.h: shared IPv4 parse/build/checksum/TTL/forward-or-deliver. RFC 1071 one's-complement checksum. Drops fragments (MF/offset) and IHL != 5. Same source compiles into both kernel.bin and libc.a.
- src/net/icmp.c + src/include/net/icmp.h: ICMP Echo Request handling (auto-reply), Echo Request emission for sys.net.ping. Unsupported types drop with counters.
- net/l2.h: IpStats (ipv4_rx/tx, dropped, oversize, ttl_expired, forwarded, icmp_echo_rx/tx) + mask/gw/mtu/forward fields on L2Context. L3 rides directly on L2 so both live in the per-NIC context.
- pktrace: 5 new points PKT_IP_PARSE, PKT_IP_DELIVER, PKT_IP_FORWARD, PKT_ICMP_ECHO_RX, PKT_ICMP_ECHO_TX.
- Kernel mgmt NIC L3 config: l2_kern_init seeds 10.0.2.15 / 255.255.255.0 / gw 10.0.2.2 / mtu 1500 matching the QEMU user netdev defaults.
- Shell commands sys.net.ping <ip>, sys.net.ip, sys.net.route. sys.net.stats extended with IpStats block.
- apps/dpdk_l3: per-AP L3 demo. Reads ip/mask/gw/mtu/forward from its INI manifest via the new SYS_APP_NET_CFG syscall, initialises an L2Context with L3 fields, dispatches ETH_TYPE_IPV4 frames to ip_rx. Bounded with MAX_ITERATIONS like dpdk_l2.
- SYS_APP_NET_CFG (syscall 31) + libc wrapper app_net_cfg(): lets an app core retrieve its kernel-parsed manifest-supplied L3 config.
- conf/dpdk_l3.ini manifest (cores=1,2,3, ip=10.0.0.10/24, gw=10.0.0.1, forward=0).
- Tests: 10 new L3 cases (sys.net.ip, sys.net.route, sys.net.ping 10.0.2.2 round-trip, IP + ICMP stats tick, DPDK L3 app per-core ready banner + IP print). 47/47 green.

### Changed
- src/kernel/app.c: manifest_handler rewritten from first-char to full-string key matching (tiny local kstreq since libc isn't linked into the kernel). Old scheme would have collided on mask vs mtu.
- AppManifest / AppSlot (src/include/kernel/app.h): carry AppNetConfig (ip/mask/gw/mtu/forward) parsed from INI.
- apps/libc/Makefile: build shared net_ip.o and net_icmp.o from src/net into libc.a alongside the other shared net sources.

## [2026-04-21] - DPDK L2 reflector demo app

### Added
- apps/dpdk_l2: per-core polled L2 reflector for AP threads only (BSP untouched). Each AP polls its assigned local NIC (same CPU, same NUMA node per nic_assign), MAC-swaps every non-ARP frame addressed to it, and re-transmits via SYS_NIC_SEND. Stats line (rx/tx frames and bytes, RX->TX min/avg/max cycles) printed every 2^19 poll iterations. No kernel changes; the path exercises SYS_NIC_SEND / SYS_NIC_RECV as the thin passthrough. Bounded to 2^20 polls per core today because the kernel app dispatcher is still serial (src/kernel/app.c TODO); trivially becomes an infinite poll once dispatch goes non-blocking.
- apps/libc/nic_backend.[ch]: reusable NetBackend that drives the per-core NIC via SYS_NIC_SEND / SYS_NIC_RECV and pulls MAC from ThreadMeta. Shared glue for future dpdk_l3 / dpdk_l4 apps.
- conf/dpdk_l2.ini manifest (cores=1,2,3, BSP excluded). Binary at /BIN/DPDK_L2.
- apps/link.ld: .text.start subsection so the binary's entry-point function is emitted first. _start is tagged with __attribute__((section(".text.start"))); prevents the flat-binary entry byte from landing on an unrelated helper.
- Tests: dpdk_l2 launches, per-core ready banner, stats line printed.

### Changed
- apps/libc/Makefile, apps/demo_app/Makefile, apps/dpdk_l2/Makefile: add -fno-stack-protector so libc-linked userland binaries don't require __stack_chk_fail.

## [2026-04-20] - Shell line editor, module splits

### Added
- Shell: 4 KB rolling command history (skip consecutive duplicates, evict whole oldest entries on overflow), up/down recall.
- Shell: full line editing with left/right cursor movement, insert-at-cursor, backspace-before-cursor. Line redraw uses only \b and spaces so it works uniformly on VGA and serial.
- Keyboard driver: 0xE0-prefix state machine emits ANSI CSI sequences (ESC [ A/B/C/D) for arrow keys, matching the serial/telnet wire format.
- Tests: up-arrow recall, mid-line insert via left arrow, right-arrow cursor move.

### Changed
- src/shell/shell.c (1207 lines) split into 9 per-subsystem files: shell.c (core + dispatch), shell_io.c (output wrappers + format helpers), shell_util.c (string/parse), shell_sys.c, shell_mem.c, shell_cpu.c, shell_disk.c, shell_fs.c, shell_net.c. Shared state moved to src/shell/shell_internal.h. No behavior change.
- src/arch/acpi.c (905 lines) split into 5 per-concern files: acpi.c (RSDP/RSDT/XSDT discovery, utilities, dispatch, orchestration + DSDT/SSDT AML bridge), acpi_madt.c, acpi_numa.c (SRAT+SLIT+accessors+acpi_pci_to_node), acpi_platform.c (FADT/HPET/MCFG/DMAR/IVRS+accessors), acpi_lsacpi.c (sys.acpi.ls output). Shared state moved to src/arch/acpi_internal.h. No behavior change.

## [2026-04-18] - L2 networking fixes, GPU strategy, CI

### Added
- GPU strategy document (docs/developer/gpu-strategy.md): direct MMIO pass-through for AMD compute GPUs
- arping and tcpdump in CI (GitHub Actions) and devbox
- TAP-based L2 network testing infrastructure with automatic tap0 setup/teardown
- Buffer pool (PktBufPool): pre-allocated 2KB slots for zero-copy TX/RX
- l2_send_zc (zero-copy send) and l2_poll_batch (batch receive up to 32 frames)
- L2 poll return codes: L2_OK, L2_EMPTY, L2_CONSUMED (replaces overloaded -1)

### Fixed
- Virtio-net header size: negotiate VIRTIO_NET_F_MRG_RXBUF for 12-byte headers matching VirtioNetHdr struct
- ARP IP byte order: MGMT_IP_DEFAULT is host byte order, needs htonl() for wire comparison
- drain_mgmt_nic: continue past multicast/ARP frames, only stop on L2_EMPTY
- GCC codegen: -fcf-protection=none prevents endbr64 on CPUs without CET support
- GCC codegen: -fno-stack-protector removes canary code incompatible with ring 3 shell
- 16-bit boot code: a32 prefix for .data references past 0xFFFF

## [2026-04-15] - Ethernet L2, ARP, packet tracing

### Added
- Ethernet L2 layer: frame parsing/construction with NetBackend vtable for dual kernel/userland compilation
- ARP: 32-entry static table, request/reply handling, gratuitous learning
- Packet tracing: application-driven PkTrace with TSC timestamps, buffer occupancy, per-stage latency
- L2 kernel backend: BSP management NIC (IP 10.0.2.15, ARP active) and inter-node NIC (no IP)
- Shell commands: sys.net.arp, sys.net.arping, sys.net.stats, sys.net.trace
- SYS_NIC_SEND and SYS_NIC_RECV syscalls for userland NIC access
- Userland L2 library (apps/libc/net/) for DPDK applications

### Fixed
- Boot message strings moved from .data to .text, permanently fixing 16-bit relocation overflow when kernel .data/.rodata grows

## [2026-04-13] - AP ring 3 execution, binary loader, PCI IDs on ISO

### Added
- AHCI SATA controller driver (SATA + SATAPI/CD-ROM)
- ISO 9660 read-only filesystem
- VFS layer: ISO mounted at /, FAT32 at /mnt/IDE
- Boot media discovery: auto-mount ISO from AHCI SATAPI
- User application build system: apps/ directory with libc and linker script
- Binary loader: reads flat binaries from ISO, executes in ring 3 on APs
- Demo app: per-core ThreadMeta reporter running in ring 3
- AP ring 3 infrastructure: SYSCALL MSRs, TSS, GDT/IDT per AP
- AP work dispatch: BSP can execute functions on any AP via polling
- SYS_TEST_AP and SYS_EXEC syscalls
- PCI vendor/class names loaded at runtime from /DATA/PCI.IDS on boot ISO

### Fixed
- Syscall wrappers: declare all kernel-clobbered registers (RDI, RSI, RDX, R8, R9, R10) preventing compiler from keeping live values across SYSCALL

### Changed
- PCI IDs moved from compiled-in rodata to text file on ISO (shrinks kernel binary)
- gen_pci_ids.sh generates data/pci.ids text file instead of C source

## [2026-04-12] - NIC assignment modes and per-CPU thread metadata

### Added
- ThreadMeta struct in kernel/cpu.h: per-CPU snapshot of cpu_index, NUMA node, assigned NIC (PCI BDF + MAC). Will be mapped read-only into ring 3 thread address space when AP threads are implemented.
- NIC assignment modes: per-numa (one NIC per NUMA node, shared) and per-core (one NIC per core, locality-respecting)
- BSP_NIC_COUNT (=2) reserves the first 2 NICs in enumeration order for BSP use (mgmt + inter-node), excluded from the AP assignment pool
- nic_assign() recomputes the per-CPU assignment respecting NUMA locality strictly (no fallback to non-local NICs)
- Auto-detected default mode at boot: per-core if AP NICs >= AP cores, otherwise per-numa
- sys.nic.mode [per-numa|per-core] - show or set the assignment mode
- sys.thread.ls - per-CPU table with NUMA, NIC name, PCI BDF, MAC

## [2026-04-11] - PCI vendor and class name lookup

### Added
- scripts/gen_pci_ids.sh fetches pci.ids from pci-ids.ucw.cz and generates src/drivers/pci_ids.c
- Vendor whitelist (~25 entries) keeps the embedded table small
- pci_vendor_name() and pci_class_name() lookup functions
- sys.pci.ls now shows vendor and class names after the NUMA column

## [2026-04-11] - Pin MEMMAP_START at fixed address

### Changed
- MEMMAP_START moved from .bss to absolute address 0x500 (defined in link.ld)
- Removes 16-bit relocation overflow that prevented growing the kernel beyond ~64KB total
- The 1024-byte E820 buffer overlaps the relocated bootsector area at 0x600-0x7FF (unused after kernel takes over)

## [2026-04-11] - Bootsector chunked memcopy

### Changed
- CD-ROM memcopy path now uses NASM %rep to unroll 64KB chunks, advancing ES/DS between iterations
- Supports kernels of any size, not limited to 64KB by 16-bit DI/SI register width

## [2026-04-11] - AML subset walker for DSDT/SSDT

### Added
- src/arch/aml.c: ~400-line AML grammar walker for extracting Device(_BBN, _PXM) declarations from DSDT/SSDT
- Parses Scope, Device, Method, Name with PkgLength and NameString grammar
- Skips opcodes it doesn't understand using PkgLength when possible
- Handles Method(_PXM, 0) {Return(constant)} as well as Name(_PXM, X)
- Used as a fallback in acpi_pci_to_node() to discover PCI host bridge proximity from DSDT _PXM (which QEMU's pxb-pcie uses instead of SRAT Type 5)

## [2026-04-11] - PCI device NUMA proximity

### Added
- SRAT Type 5 (Generic Initiator Affinity) parsing in arch/acpi.c
- ACPINumaPCIAffinity struct and acpi_pci_to_node() with three lookup paths: SRAT Type 5, AML _BBN/_PXM walk, and ECAM base address inference from SRAT memory affinities
- numa_node field in PCIDevice, populated during enumeration
- numa_node field in NICSlot, copied from underlying PCIDevice
- nic_get_numa_node() and nic_find_for_node() in the NIC API
- sys.pci.ls and sys.nic.ls show NUMA proximity per device
- QEMU scripts use pxb-pcie + pcie-root-port to give each NIC a NUMA affinity

## [2026-04-11] - Reorganize src/ by subsystem

### Changed
- Move flat src/ layout into subdirectories: boot/, kernel/, arch/, drivers/, net/, fs/, shell/
- Drop the system. prefix from .c and .h filenames
- include/ mirrors the source tree (kernel/, arch/, drivers/, net/, fs/, shell/)
- Includes use the new paths: #include <kernel/mem.h>, #include <drivers/ide.h>, etc.
- Build output renamed from kernel to kernel.bin (to free up the kernel/ directory name)
- Makefile rewritten with subdirectory-aware rules

### Removed
- src/system.kbd.c (2-byte empty stub)

## [2026-04-10] - Renamed project from ZINC to Isurus

### Changed
- Project renamed to Isurus (shark genus containing the mako) to avoid collision with existing Zinc OS
- "Zero-Interrupt NUMA Cluster" kept as descriptive subtitle (no longer an acronym)
- ISO volume label: ZINC_OS -> ISURUS_OS

## [2026-04-02] - PCI enumeration, virtio-net driver, NIC abstraction

### Added
- PCI device enumeration via PCIe ECAM (MCFG ACPI table)
- Config space read/write, BAR decoding (MMIO/IO, 32/64-bit)
- Virtio PCI transport: capability walking, virtqueue allocation, feature negotiation
- Virtio-net driver: TX/RX via virtqueues, MAC address, link status
- NIC abstraction layer: vtable dispatch supporting multiple NIC drivers
- sys.pci.ls and sys.nic.ls shell commands
- Generic map_mmio_range() for arbitrary physical MMIO mapping
- memcpy/memset for freestanding kernel

### Changed
- QEMU machine type switched from i440fx to Q35 (PCIe ECAM support)
- All QEMU scripts include 4 virtio-net-pci devices (2 BSP + 1 per NUMA node)
- Linker script captures GCC subsections (.text.*, .rodata.*, .bss.*) and discards .eh_frame/.note.gnu.property
- IDE driver skips init when no controller present (Q35 has AHCI, not PIIX IDE)
- Boot init sequence: pci_init and nic_init called after fat32_init

## [2026-03-31] - Ring 3 shell with SYSCALL/SYSRET

### Added
- GDT64 expanded with ring 3 code/data segments (0x18, 0x20, 0x28) and TSS descriptor (0x30)
- Per-CPU TSS array with RSP0 for ring 3 to ring 0 transitions
- SYSCALL/SYSRET infrastructure: IA32_STAR, IA32_LSTAR, IA32_SFMASK MSRs, EFER.SCE
- SWAPGS-based per-CPU data access pattern (IA32_KERNEL_GS_BASE = &percpu[cpu])
- Syscall dispatch table with 23 syscalls (display, memory, disk, FS, ACPI, task management)
- User-side syscall wrappers in include/syscall.h (inline asm)
- Input ring buffer decoupling keyboard/serial ISRs from ring 3 shell
- BSP cooperative multitasking: task create, yield, exit, wait/wake, round-robin scheduling
- sys.cpu.ring shell command to verify current privilege level

### Changed
- Shell runs in ring 3 on BSP via IRETQ transition from kernel
- Shell uses syscall wrappers for all kernel services (putc, kprint, disk, memory, ACPI)
- Keyboard and serial drivers push to input ring buffer instead of calling shell directly
- PerCPU struct expanded to 40 bytes (added user_stack_top, user_rsp, cr3, in_usermode)
- AP trampoline updated for new PerCPU size

## [2026-03-27] - Move stacks to high memory

### Changed
- BSP stack (64KB) and AP stacks (16KB x CPUs) allocated from high memory (above 1MB) via PAGING_LOCATION_END bump allocator
- Heap range now dynamic: starts at page-aligned kernelEnd instead of hardcoded 0xD000
- BSS reduced from ~336KB to ~12KB (only MEMMAP, IDT, 4KB temp stack, C globals)

### Fixed
- BSS/heap overlap: BSP stack and AP stacks in BSS collided with heap at 0xD000-0x9F000

## [2026-03-27] - Boot Rework and SMP Fix

### Changed
- Bootsector rewritten: multi-path boot (LBA, CHS, CD-ROM no-emulation El Torito)
- KSIZE computed dynamically from kernel binary size
- Build switched to xorriso El Torito no-emul ISO as primary boot artifact
- QEMU launches from ISO with NUMA topology and serial trace
- 16-bit display routines rewritten with rep movsw/stosw
- AP trampoline moved from 0x8000 to 0x9F000 (was overwriting kernel .data)
- APs park with cli (BSP owns all interrupts)
- Trampoline uses segment-relative addressing (ORG 0, DS=CS) for high-memory compatibility
- memory_map.md rewritten with correct addresses and starts-at column

### Added
- Serial debug trace on COM1 for boot diagnostics (DBG macro)
- devbox.json/devbox.lock for reproducible build toolchain
- C23 bool/true/false guards in types.h

### Fixed
- AP trampoline at 0x8000 overwrote kernel .data, causing VGA garbage
- AP trampoline at 0x9F000 triple-faulted: 16-bit address overflow with ORG > 0xFFFF
- Explicit -fno-stack-protector (devbox GCC enables it by default, unusable without FS base)

## [2026-03-14] - Project Rename and Documentation Overhaul

### Changed
- Renamed project from "Remote NUMA Kernel" to ZINC (Zero-Interrupt NUMA Cluster)
- License changed from GPL-2.0 to dual AGPLv3 (community) / commercial
- Added status notices to README.md, ARCHITECTURE.md, application-model.md (target architecture vs current state)
- Marked SPDK as planned (not yet implemented) across all docs
- Standardized documentation audience pointers and cross-references
- Clarified DSM page fault mechanism (how BSP returns page data to faulting AP)
- Clarified locality map is intentionally static after failover (by design, not an accident)
- Updated ARCHITECTURE.md status section to include device libraries

### Added
- LICENSE file: dual AGPLv3 / commercial with support and contact info
- LICENSES/AGPL-3.0.txt: full AGPLv3 text
- Linux HPC comparison section in doc/user/application-model.md
- Documentation guide in README.md (per-audience reading paths)
- Glossary entries: ZINC, Affinity, Locality map, Ownership, Soft real-time, Tier 1/2/3
- Soft real-time characterization in application-model.md

### Removed
- doc/memory_use.text (stale draft, contradicted memory_map.md)
- doc/todo.text (most items already completed, replaced with new version)
- doc/docresources.text (bare URLs with no context)
- All GPU/GPDK references (removed from README, ARCHITECTURE, application-model, glossary)
- Duplicated boot sequence from CHANGELOG.md
- Duplicated memory layout from DEBUG.md (now references memory_map.md)

## [2026-03-20] - ACPI table parsing and system topology

### Added
- ACPI table parser framework: RSDT/XSDT root table discovery with checksum validation
- Parsed tables: MADT (CPU/LAPIC/IOAPIC), SRAT (NUMA affinity), SLIT (distance matrix), HPET, FADT/FACP, MCFG (PCIe ECAM), DMAR/IVRS (IOMMU)
- Shell topology commands: sys.cpu.ls, sys.numa.ls, sys.acpi.ls
- Reboot via ACPI FADT reset register

### Fixed
- IRQ context handling and BSP stack sizing for nested interrupts

## [2026-03-19] - ACPI/APIC/SMP bringup and license

### Added
- ACPI root table discovery and basic MADT parsing
- LAPIC and IOAPIC initialization
- SMP: AP bringup via INIT-SIPI-SIPI with trampoline at 0x9F000
- AGPL-3.0 license (dual AGPLv3 / commercial)

## [2026-03-09] - 80x28 text mode

### Changed
- Switched to 80x28 text mode with C-based VGA monitor driver

## [2026-03-08] - FAT32 and IDE support

### Added
- IDE disk driver with PIO read/write
- FAT32 read-only driver: directory listing (ls) and file reading (cat)
- kmalloc/kfree: 4KB-page bitmap allocator
- kprint: kernel string output to VGA

## [2026-03-07] - Project goals

### Added
- Initial project goals document for remote NUMA kernel research

## [2026-02-22] - Kernel shell and modernization

### Added
- Kernel shell with basic commands (help, clear, echo, reboot)
- Updated build toolchain for modern NASM/GCC

### Fixed
- K16 panic on boot: disabled interrupts until IVT setup complete
- Triple fault on PAE enable: load CR3 before enabling PAE
- Triple fault on paging enable: disable interrupts before enabling paging
- Removed scroll() call causing screen gibberish
- Reboot command: multi-method reset
- Debug scripts: compile_qemu_debug.sh, check_qemu_log.sh
- QEMU logging with -d int,cpu_reset,guest_errors

### Changed
- Reordered long mode init: CR3 - PAE - LME - Paging

## [2014-10-04] - E820 memory map definitions

### Added
- E820 entry definitions in C
- True/false constants in C types

## [2014-09-26] - Memory alignment

### Changed
- Aligned MEMMAP on 16-byte boundary

## [2014-09-23] - Keyboard driver and memory detection

### Added
- Keyboard driver with scancode handling
- Fixed physical memory detection and paging structure accuracy

### Fixed
- Page fault bug: display CR2 value on fault

## [2014-09-18] - Link script and ISR rework

### Added
- ISRs for CPU exceptions (divide-by-zero, GPF, page fault, etc.)

### Changed
- Reworked linker script: moved data structures from arbitrary addresses to BSS
- Kernel binary size reduced by half

## [2014-09-15] - VGA driver in C

### Added
- Video driver rewritten in C (replacing assembly)

## [2014-09-10] - IDT64 and timer/keyboard ISRs

### Added
- 64-bit IDT with basic ISRs for timer and keyboard
- Makefile and build scripts
- Bochs emulator support

## [2014-09-04] - Interrupts and boot improvements

### Added
- 64-bit IDT and basic ISR framework
- Documentation for interrupts and memory layout
- Bootsector loads multiple sectors

## [2013-08-25] - 64-bit paging

### Added
- 64-bit long mode with paging enabled

## [2013-06-05] - First commit

### Added
- Initial project: 16-bit bootsector, 32-bit protected mode transition
