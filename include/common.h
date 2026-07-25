#ifndef COMMON_H
#define COMMON_H

#include <efi.h>
#include <efilib.h>
#include <string.h>

#define VERBOSE_MACHO
#define VERBOSE_BOOT
//#define KASLR_ENABLED

/*
 * Boot-info block layout (physical addresses).
 *
 * The kernel image lands at [0x100000, ~0x1779000); this block sits above it.
 * Runtime-services VAs are packed above it (see boot.c SVAM) starting at
 * XNU_RT_VA_BASE; ksize is inflated so physfree covers everything.
 *
 * Layout (phys):
 *   [0x100000, ~0x1b02000)   kernel image (fileset KC extends this far)
 *   [0x2800000, 0x2820000)   boot-info block (boot_args/tables/DT/memmap)
 *   [0x3200000, va_cursor)   runtime-services VA pack; physfree = round_up(va_cursor)
 */
#if defined(__aarch64__)
extern EFI_PHYSICAL_ADDRESS g_xnu_bootinfo_base;
#define XNU_BOOTINFO_BASE       g_xnu_bootinfo_base
#else
#define XNU_BOOTINFO_BASE       0x2800000ULL   /* 2MB-aligned, above KC image  */
#endif
#define XNU_BOOTARGS_PHYS       (XNU_BOOTINFO_BASE + 0x00000) /* 1 page       */
#define XNU_EFITABLES_PHYS      (XNU_BOOTINFO_BASE + 0x01000) /* 1 page       */
#define XNU_DEVTREE_PHYS        (XNU_BOOTINFO_BASE + 0x02000) /* 2 pages      */
#if defined(__aarch64__)
#define XNU_ARM64_BOOTARGS_PHYS (XNU_BOOTINFO_BASE + 0x04000) /* 1 page       */
#endif
#define XNU_TRUSTCACHE_PHYS     (XNU_BOOTINFO_BASE + 0x05000) /* 1 page       */
#define XNU_MEMMAP_PHYS         (XNU_BOOTINFO_BASE + 0x10000) /* up to 16 pg  */
#define XNU_BOOTINFO_END        (XNU_BOOTINFO_BASE + 0x20000) /* 128KB block  */
#define XNU_RT_VA_BASE          0x3200000ULL   /* runtime-services VA pack base */

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
  EFI_HANDLE boot_volume;
  UINT32 kslide;
  UINT64 phys_base;
  EFI_PHYSICAL_ADDRESS kernel_region_base;
  EFI_PHYSICAL_ADDRESS kernel_region_end;
} AppContext;

typedef struct FileBuffer {
  VOID *data;
  UINTN size;
} FileBuffer;

#endif