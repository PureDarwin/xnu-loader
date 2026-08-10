#ifndef JUMP_H
#define JUMP_H

#include "common.h"

VOID jump_to_xnu(VOID *entry, UINT64 boot_args_phys, VOID *stack_top) __attribute__((noreturn));

#if defined(__x86_64__)
/*
 * Switch to `new_sp` and call fn(arg) there. Used to get off the
 * firmware-provided stack before copying the kernel image over low memory,
 * which on some boards contains that very stack.
 */
VOID pd_call_on_stack(VOID *new_sp, VOID (*fn)(VOID *), VOID *arg)
    __attribute__((noreturn));
#endif

#endif