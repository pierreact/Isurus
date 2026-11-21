# OS Development - Debugging Guide

**Audience:** Kernel developers. For memory layout details, see [memory-map.md](memory-map.md). For architecture decisions, see [architecture.md](architecture.md).

## Bootsector trace (COM1)

The first-stage loader (`src/boot/bootsector.asm`) emits single characters on **COM1** (0x3F8) so you can see how far boot gets before the kernel runs. Read the stream left-to-right:

| Char | Meaning |
|------|--------|
| `0` | Bootsector running (after relocate + screen clear) |
| `C` | CD-ROM no-emul path: copying kernel from memory |
| `H` | CHS read loop entered (raw disk, LBA not available or failed) |
| `X` | Disk read failed |
| `J` | About to **far jmp** to kernel at `0x1000` |

CD-ROM ISO boot: **`0CJ`**. Raw-disk CHS boot: **`0HJ`**. LBA success: **`0J`**. Disk error: **`0HX`**.

If the trace ends with **`J`**, the bootsector completed and jumped to the kernel; a hang after that points at the kernel entry (`kmain`), not the loader.

**QEMU:** `compile_qemu.sh` / `compile_qemu_debug.sh` attach COM1 to **`./bootserial.log`**. Run `tail -f bootserial.log` while QEMU is running.

**Disable** serial I/O in the bootsector (smaller binary / no UART access): from `src/`, `make bootsector.bin BOOT_DEBUG=0` (passes **`-DBOOT_DEBUG=0`** to `nasm`; see `src/Makefile`).

To print **COM1 on the terminal** instead of a file, replace `-serial file:./bootserial.log` with `-serial stdio` (may interact with `-monitor stdio`).

## Quick Start

### 1. Run Normally (with reboot enabled)
```bash
./compile_qemu.sh
```

The OS will boot in QEMU with debug logging enabled. The reboot command will work normally.

### 2. Run in Debug Mode (no reboot on crashes)
```bash
./compile_qemu_debug.sh
```

The OS will boot in QEMU with full debug logging:
- Interrupts logged
- CPU resets logged  
- Guest errors logged
- `-no-reboot` and `-no-shutdown` prevent automatic restarts on triple faults
- Use this when debugging crashes to examine the state

### 2. Check the Debug Log
After a crash or issue, check the QEMU log:
```bash
cat ./qemu.log | tail -100
```

Or use the helper script:
```bash
./check_qemu_log.sh
```

### 3. Interactive Debugging with GDB

**Terminal 1** - Start QEMU:
```bash
./compile_qemu.sh
```

**Terminal 2** - Connect GDB:
```bash
./gdb_debug.sh
```

## What to Look For in QEMU Log

### Triple Fault
```
check_exception old: 0xffffffff new 0xe
Triple fault
```
This means the CPU hit an exception while handling another exception.

### CPU Reset
```
CPU Reset (CPU 0)
```
Usually caused by triple fault or invalid paging structures.

### Page Fault (Exception 14 / 0xe)
```
check_exception old: 0xffffffff new 0xe
```
This is exception 14 (page fault). Common causes:
- Invalid PML4/PDPT/PD/PT entry
- Missing PRESENT bit in paging structure
- Accessing unmapped memory

### General Protection Fault (Exception 13 / 0xd)
```
check_exception old: 0xffffffff new 0xd
```
Common causes:
- Invalid segment selector
- Privilege level violation
- Invalid descriptor

## Common Issues and Solutions

### Machine Resets When Enabling PAE
**Symptoms**: Reset right after "CR3 loaded" message
**Cause**: Invalid paging structures
**Check**:
1. PML4T address is valid (should be 0x100000)
2. First PML4E has PRESENT bit set
3. All referenced tables exist and are valid

### Machine Resets When Enabling Paging
**Symptoms**: Reset after "PAE Enabled" message
**Cause**: Paging structures don't map currently executing code
**Solution**: Ensure low memory (0x0 - 0x200000) is identity mapped

### Layout-sensitive failures (uninitialized `.bss`, fixed 2026-11-21)
**Symptoms**: After a change that only moves `.bss` (a new static array), the first shell command arrives as garbage (`Unknown command: $$$$' '...`) and FAT32 mounts at `/mnt/IDE` although `IDE: No controller present`.
**Cause**: Nothing zeroed `.bss`. The CD boot copy (bootsector, KSIZE = kernel sectors + 32) zeroes `.bss` only up to `0x1000 + KSIZE*512` (0x18000 at the time). Above it, memory still held the El Torito load: kernel `.rodata` bytes. Variables that moved above that line started non-zero (`input_head`, `drive_sectors`).
**Solution**: `kernel32:` in `src/boot/kmain.s` zeroes `[__bss, kernelEnd)` right after setting ESP. The 16-bit code writes no `.bss`.
**Diagnosis**: no CPU write hit a GDB watchpoint on the variable, so the value was present from boot. Address minus 0x7C00 minus 0x200 (bootsector) gave the `kernel.bin` offset of the leftover bytes; `xxd` at that offset showed the same values.

### Layout-sensitive failures (boot stack corrupting `.text`, fixed 2026-04-22)
**Symptoms**: Tests pass/fail depending on cosmetic code changes, `-O` level, or an unused helper. Failing test is whichever function lands near the stack area (~0xEF78 / end of `.text`).
**Cause**: 16->32 bit transition in `src/boot/kmain.s` never set ESP. 16-bit stack was SS=0x8000:SP=0xF000 (linear 0x8F000). After PE=1 and flat SS reload, ESP=0x0000F000, inside kernel `.text`. Small GDT flat-data descriptor had granularity byte 0x8F (B=0), so stack ops used 16-bit SP. Every 32-bit push (PIC init, IDT fill, paging setup, long-mode probe) wrote into kernel code; e.g. `call sh_print` in `cmd_thread_ls` went from `e8 83 e3 ff ff` to `e8 83 e3 ff 00`.
**Solution** (`src/boot/kmain.s`):
1. Small GDT flat-data granularity 0x8F -> 0xCF (B=1, stack ops use ESP).
2. First instruction at `kernel32:`: `mov esp, 0x80000`.
GDT64 is not involved; it loads only after LME is set.
**Diagnosis recipe** (tooling not committed):
- If canaries on kernel stack, user stack, saved RIP/RSP and `.text` are clean but tests still fail, the damage happened before 64-bit code ran. Snapshot kernel memory right after boot and `cmp` against `kernel.bin`.
- `-d int,cpu_reset,guest_errors,in_asm`: a TB retranslation means its bytes changed under it.
- ELF build for GDB: remove `OUTPUT_FORMAT(binary)` from `src/link.ld`, add `objcopy -O binary` to `src/Makefile`, `-g` to CFLAGS. Attach via `-gdb tcp::1234 -S`, watchpoint the suspect address, break on `shell_execute_command`, `dump memory` between commands, `cmp -l` snapshots.

## GDB Commands

```gdb
# Connect to QEMU
target remote localhost:1234

# Show registers
info registers

# Examine memory at address
x/10x 0x100000    # Show 10 hex values at PML4T

# Disassemble
x/10i $pc         # Show next 10 instructions

# Step one instruction
si

# Continue execution
c

# Set breakpoint
b *0x1000         # Break at address
```

## QEMU Monitor Commands

In the QEMU monitor (stdio):
```
info registers     # Show CPU registers
info mem           # Show memory mappings (after paging enabled)
info tlb           # Show TLB entries
x /10x 0x100000    # Examine memory
```

## Memory dump from QEMU monitor

```
memsave 0 0xffffffff dump.dmp
```

## Memory Layout

See [memory-map.md](memory-map.md) for the full memory map.
