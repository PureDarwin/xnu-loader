#ifndef COMMON_H
#define COMMON_H

#include <efi.h>
#include <efilib.h>
#include <string.h>

/*
 * GNU-EFI 4.x marks CopyMem_1 and SetMem as EFIAPI (__attribute__((ms_abi))),
 * meaning they expect arguments in RCX/RDX/R8 (Windows calling convention).
 * Our code is compiled with the SysV ABI (RDI/RSI/RDX), so every direct call
 * to CopyMem/SetMem from our code has an ABI mismatch and silently does nothing.
 *
 * Override both macros with inline wrappers that use the correct calling convention.
 */
#undef CopyMem
#undef SetMem

static inline VOID *XnuCopyMem(VOID *dst, const VOID *src, UINTN n) {
  UINT8 *d = (UINT8 *)dst;
  const UINT8 *s = (const UINT8 *)src;
  while (n--) *d++ = *s++;
  return dst;
}
static inline VOID *XnuSetMem(VOID *dst, UINTN n, UINT8 val) {
  UINT8 *d = (UINT8 *)dst;
  while (n--) *d++ = val;
  return dst;
}

#define CopyMem(dst, src, n) XnuCopyMem((VOID *)(UINTN)(dst), (const VOID *)(UINTN)(src), (UINTN)(n))
#define SetMem(dst, n, val) XnuSetMem((VOID *)(UINTN)(dst), (UINTN)(n), (UINT8)(val))

typedef struct AppContext {
  EFI_HANDLE image_handle;
  EFI_SYSTEM_TABLE *st;
  EFI_BOOT_SERVICES *bs;
  EFI_RUNTIME_SERVICES *rt;
} AppContext;

typedef struct FileBuffer {
  VOID *data;
  UINTN size;
} FileBuffer;

#endif