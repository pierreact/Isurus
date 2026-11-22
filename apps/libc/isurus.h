#ifndef ISURUS_H
#define ISURUS_H

#include <types.h>
#include "syscall.h"

// Thread metadata: per-CPU snapshot exposed by the kernel.
// Must match kernel/cpu.h ThreadMeta layout exactly.
typedef struct {
    uint32 cpu_index;
    uint32 numa_node;
    uint32 nic_index;
    uint16 nic_segment;
    uint8  nic_bus;
    uint8  nic_dev;
    uint8  nic_func;
    uint8  reserved[3];
    uint8  nic_mac[6];
    uint8  reserved2[2];
    uint32 reserved3;
    // This core's private data region, zeroed by the kernel before each
    // launch. The app image (.data/.bss) is shared by all cores of the
    // app: per-core writable state goes here, not in static variables.
    uint64 data_base;
    uint64 data_size;
} ThreadMeta;

_Static_assert(__builtin_offsetof(ThreadMeta, data_base) == 32 &&
               sizeof(ThreadMeta) == 48, "ThreadMeta layout changed: update src/include/kernel/cpu.h");

#define NIC_NONE 0xFFFFFFFFu
#define THREAD_NUMA_UNKNOWN 0xFFFFFFFFu

// Exit the current thread (AP re-parks, BSP task exits)
static inline void exit(void) {
    syscall0(SYS_TASK_EXIT);
    // unreachable
    for (;;) {}
}

// Yield to next BSP task (no-op on APs)
static inline void yield(void) {
    syscall0(SYS_YIELD);
}

// Per-app L3 config as parsed from the INI manifest by the kernel.
// Zero means "unset". ip/mask/gw are network byte order.
typedef struct {
    uint32 ip;
    uint32 mask;
    uint32 gw;
    uint16 mtu;
    uint8  forward;
    uint8  reserved;
} AppNetCfg;

// Fetch the calling core's manifest-parsed L3 config. Returns 0 on
// success, -1 if the caller is not an app-dispatched AP core.
static inline int app_net_cfg(AppNetCfg *out) {
    return (int)syscall1(SYS_APP_NET_CFG, (long)out);
}

#endif
