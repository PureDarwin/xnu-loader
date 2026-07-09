#ifndef COMMON_H
#define COMMON_H

#include <efi.h>
#include <efilib.h>
#include <string.h>

//#define VERBOSE_MACHO
//#define VERBOSE_BOOT
//#define KASLR_ENABLED

/*
 * Boot-info block layout (physical addresses).
 *
 * XNU's pmap_lowmem_finalize() unmaps EVERYTHING below phys 0x100000 (it does
 * pmap_remove(kernel_pmap, LOWGLOBAL_ALIAS+PAGE_SIZE, vm_kernel_base)), so any
 * boot structure XNU keeps referencing (boot_args, device tree, EFI system /
 * runtime tables, EFI memory map) MUST live at phys >= 0x100000.  XNU never
 * copies boot_args; it accesses it in place via the physmap, so we place all
 * of these in a fixed reserved block just above the kernel image and inflate
 * boot_args->ksize so XNU's physfree covers the block (making it static kernel
 * memory that is mapped and never freed).  This mirrors boot.efi's
 * AllocateKernelMemory scheme.
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
#define XNU_BOOTINFO_BASE       0x2800000ULL   /* 2MB-aligned, above KC image  */
#define XNU_BOOTARGS_PHYS       (XNU_BOOTINFO_BASE + 0x00000) /* 1 page       */
#define XNU_EFITABLES_PHYS      (XNU_BOOTINFO_BASE + 0x01000) /* 1 page       */
#define XNU_DEVTREE_PHYS        (XNU_BOOTINFO_BASE + 0x02000) /* 2 pages      */
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