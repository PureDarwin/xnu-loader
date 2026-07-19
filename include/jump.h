#ifndef JUMP_H
#define JUMP_H

#include "common.h"

VOID jump_to_xnu(VOID *entry, UINT64 boot_args_phys, VOID *stack_top) __attribute__((noreturn));

#endif