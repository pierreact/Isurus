// Isurus per-core memory test (test/run_tests.sh).
//
// On each core, checks:
//   region0: the private data region (meta->data_base) is zero at entry
//            (kernel zeroes it per launch).
//   bss0:    this core's element of a .bss array is zero at entry
//            (kernel zeroes the image's .bss per launch).
//   mismatches: the data region is private. Each core fills it with a
//            value unique to (core, round) and reads it back; a word
//            changed by another core counts as a mismatch.
// One line per core, built in a buffer and printed with one call. Cores
// print in cpu_index order through a turn counter in the shared .bss
// (AP console output has no lock), so lines do not interleave on the
// serial port. Assumes the manifest lists cores 1..N without a gap.

#include "../libc/isurus.h"
#include "../libc/stdio.h"

#define ROUNDS   64
#define MAX_CPU  16
#define TURN_SPIN_MAX (1ULL << 32)   // give up waiting for the previous core

static uint64 bss_probe[MAX_CPU];
static volatile uint32 print_turn;      // .bss, shared by all cores: last cpu_index that printed

static char *put_str(char *d, const char *s) {
    while (*s) *d++ = *s++;
    return d;
}

static char *put_dec(char *d, uint64 v) {
    char tmp[20];
    int n = 0;
    do { tmp[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (n) *d++ = tmp[--n];
    return d;
}

__attribute__((section(".text.start")))
void _start(ThreadMeta *meta) {
    uint32 cpu = meta->cpu_index;
    volatile uint64 *p = (volatile uint64 *)meta->data_base;
    uint64 n = meta->data_size / 8;
    char line[96];
    char *d = line;

    if (cpu >= MAX_CPU || p == 0 || n == 0) {
        puts("[percore] no data region\n");
        exit();
    }

    uint64 region_dirty = 0;
    for (uint64 i = 0; i < n; i++)
        if (p[i] != 0) region_dirty++;
    int bss_dirty = bss_probe[cpu] != 0;
    bss_probe[cpu] = cpu + 1;

    uint64 mismatches = 0;
    for (uint64 r = 0; r < ROUNDS; r++) {
        uint64 v = ((uint64)(cpu + 1) << 32) | (r + 1);
        for (uint64 i = 0; i < n; i++) p[i] = v;
        for (uint64 i = 0; i < n; i++)
            if (p[i] != v) mismatches++;
    }

    for (uint64 k = 0; print_turn != cpu - 1 && k < TURN_SPIN_MAX; k++) {}

    d = put_str(d, "[percore] cpu=");
    d = put_dec(d, cpu);
    d = put_str(d, region_dirty ? " region0=dirty" : " region0=ok");
    d = put_str(d, bss_dirty ? " bss0=dirty" : " bss0=ok");
    d = put_str(d, " mismatches=");
    d = put_dec(d, mismatches);
    d = put_str(d, "\n");
    *d = 0;
    puts(line);
    print_turn = cpu;
    exit();
}
