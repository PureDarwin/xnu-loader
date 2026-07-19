#ifndef JUMP_H
#define JUMP_H

#include "common.h"

VOID jump_to_xnu(VOID *entry, UINT64 boot_args_phys, VOID *stack_top) __attribute__((noreturn));

#if defined(__aarch64__)
/* Secondary-core (1-3) entry point for the BCM2837 spin-table release -
 * see jump.S. Not a normal function call (no args in registers when a
 * spinning core branches here); reads secondary_entry_target/
 * secondary_bootargs_phys instead, which must be filled in before the
 * spin-table slots are written. */
extern UINT64 secondary_entry_target;
extern UINT64 secondary_bootargs_phys;
extern VOID secondary_entry_trampoline(VOID);
#endif

#endif