#ifndef KERNEL_LOADER_H
#define KERNEL_LOADER_H

#include <types.h>
#include <kernel/cpu.h>

#define USER_LOAD_ADDR    0x2000000   // 32MB, above page tables (~9MB), must match apps/link.ld
#define USER_STACK_SIZE   0x10000     // 64KB per AP user stack
#define AP_DATA_SIZE      0x20000     // 128KB per AP private data region (ThreadMeta.data_base)

// Per-AP state. ap_user_stacks: allocated once at boot (ap_user_alloc), reused
// by every launch. ap_entry_addrs: set by loader or app launcher before dispatch.
extern uint64 ap_user_stacks[MAX_CPUS];
extern uint64 ap_entry_addrs[MAX_CPUS];

// Boot (cpu_init): allocate the user stack and private data region of
// one AP. Called once per AP; a launch never allocates.
void ap_user_alloc(uint32 cpu_idx);

// Physical address of the private data region of one AP (0 for the BSP).
uint64 ap_data_region(uint32 cpu_idx);

// Prepare cpu_idx for a ring 3 launch: zero its data region and publish
// it in thread_meta[cpu_idx]. Cannot fail.
void ap_user_prepare(uint32 cpu_idx);

// Load a flat app image at load_addr and zero the rest of its
// APP_REGION_SIZE region (the image's .bss is not in the file).
// Returns the file size, or 0 on failure (message printed).
uint32 app_image_load(const char *path, uint64 load_addr);

// AP ring 3 launcher (called on AP via ap_dispatch, does not return)
uint64 ap_run_ring3(uint64 cpu_idx);

// Load a flat binary from ISO and execute it on all APs in ring 3.
// Each AP runs the binary's _start in ring 3, syscalls back when done.
// Returns 0 on success (all APs completed).
int loader_exec(const char *iso_path);

#endif
