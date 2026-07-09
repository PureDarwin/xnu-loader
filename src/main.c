#include "common.h"
#include "app.h"
#include "boot.h"
#include "console.h"
#include "fileio.h"
#include "macho.h"
#include "jump.h"

EFI_STATUS AllocKernelMemRegion(AppContext *ctx, UINT64 span_bytes) {
  EFI_PHYSICAL_ADDRESS base = 0x7FFFFFFF;
  UINTN total_pages = (UINTN)((span_bytes + EFI_PAGE_SIZE - 1) >> EFI_PAGE_SHIFT);

  EFI_STATUS status = uefi_call_wrapper(ctx->bs->AllocatePages, 4,
    AllocateMaxAddress, EfiLoaderData, total_pages, &base);
  if (EFI_ERROR(status)) {
    log_error(L"AllocKernelMemRegion: AllocatePages(%lu pages) failed: %r\r\n",
              (UINT64)total_pages, status);
    return status;
  }

  SetMem((VOID *)(UINTN)base, (UINTN)((UINT64)total_pages << EFI_PAGE_SHIFT), 0);

  ctx->kernel_region_base = base;
  ctx->kernel_region_end  = base + ((UINT64)total_pages << EFI_PAGE_SHIFT);

  log_info(L"kernel staging: 0x%lx - 0x%lx (%lu pages)\r\n",
       ctx->kernel_region_base, ctx->kernel_region_end, (UINT64)total_pages);
  return EFI_SUCCESS;
}

EFI_STATUS efi_main(EFI_HANDLE image, EFI_SYSTEM_TABLE *st) {
  AppContext ctx;
  FileBuffer kernel = {0};
  MachoImage image_info;
  MachoLoadResult load_result = {0};
  EFI_STATUS status;

  status = app_init(&ctx, image, st);
  if (EFI_ERROR(status))
    return status;

  log_info(L"XNU EFI loader start\r\n");

  {
    EFI_LOADED_IMAGE *li = NULL;
    if (!EFI_ERROR(app_get_loaded_image(&ctx, &li)))
      log_info(L"loader image base=0x%lx size=0x%lx\r\n",
               (UINT64)(UINTN)li->ImageBase, (UINT64)li->ImageSize);
  }

  EFI_HANDLE found_handle = NULL;

  status = file_read_all_from_any_volume(
      &ctx,
      L"\\EFI\\BOOT\\kernel",
      &kernel,
      &found_handle);

  if (EFI_ERROR(status)) {
    log_error(L"failed to read kernel from any volume: %r\r\n", status);
    return status;
  }
  ctx.boot_volume = found_handle;

  log_info(L"kernel size: %lu bytes\r\n", kernel.size);

  status = macho_parse(kernel.data, kernel.size, &image_info);
  if (EFI_ERROR(status)) {
    log_error(L"failed to parse Mach-O: %r\r\n", status);
    file_free(&ctx, &kernel);
    return status;
  }

  status = macho_dump(&image_info);
  if (EFI_ERROR(status)) {
    log_error(L"failed to dump Mach-O: %r\r\n", status);
    file_free(&ctx, &kernel);
    return status;
  }

  {
#ifdef KASLR_ENABLED
    ctx.kslide = 2 * KASLR_SLIDE_GRANULE;
    log_info(L"KASLR: fixed slide=0x%x\r\n", ctx.kslide);
#else
    ctx.kslide = 0;
    log_info(L"KASLR: disabled, kslide=0\r\n");
#endif
  }

  /* Compute vm range to derive phys_base before any allocation,
   * matching Apple: phys_base = lo32(lowest_vmaddr) + kslide */
  {
    UINT64 lo = 0, hi = 0;
    status = macho_compute_vm_range_pub(&image_info, &lo, &hi);
    if (EFI_ERROR(status)) {
      log_error(L"failed to compute vm range: %r\r\n", status);
      file_free(&ctx, &kernel);
      return status;
    }
    ctx.phys_base = (UINT32)lo;  /* physical is always unslid; kslide shifts VAs only */
    log_info(L"phys_base=0x%lx kslide=0x%x span=0x%lx\r\n",
             (UINT64)ctx.phys_base, ctx.kslide, hi - lo);
  }

  /* Allocate a high staging buffer for the kernel image. */
  {
    UINT64 lo2 = 0, hi2 = 0;
    macho_compute_vm_range_pub(&image_info, &lo2, &hi2);
    status = AllocKernelMemRegion(&ctx, hi2 - lo2);
  }
  if (EFI_ERROR(status)) {
    log_error(L"AllocKernelMemRegion failed: %r\r\n", status);
    file_free(&ctx, &kernel);
    return status;
  }
  /* phys_base = staging address; post-EBS copy moves it to 0x100000 */
  ctx.phys_base = ctx.kernel_region_base;

  /* Single contiguous allocation at phys_base, matching Apple's model */
  status = macho_load_segments_contiguous(&ctx, &image_info, ctx.phys_base, &load_result);
  if (EFI_ERROR(status)) {
    log_error(L"failed to load segments: %r\r\n", status);
    file_free(&ctx, &kernel);
    return status;
  }

  if (ctx.kslide) {
    status = macho_apply_kaslr_slide(&image_info, &load_result, ctx.kslide);
    if (EFI_ERROR(status)) {
      log_error(L"KASLR: apply slide failed: %r\r\n", status);
      macho_unload_contiguous(&ctx, &load_result);
      file_free(&ctx, &kernel);
      return status;
    }
    /* Note: pstart's __HIB bootstrap immediates (CR3/BootPML4/GDT/stack/ljmp)
     * and the GDT descriptor base ARE in the local reloc table as negative
     * (signed) __DATA-relative entries, so macho_apply_kaslr_slide above
     * already patched them once r_address is treated as signed.  No separate
     * pstart fixup is needed -- boot.efi doesn't do one either. */

    /* XNU rebuilds page tables from its embedded header; slide it too. */
    status = macho_patch_header_slide(&image_info, &load_result, ctx.kslide);
    if (EFI_ERROR(status)) {
      log_error(L"KASLR: patch embedded header failed: %r\r\n", status);
      macho_unload_contiguous(&ctx, &load_result);
      file_free(&ctx, &kernel);
      return status;
    }
  }

  log_info(
      L"loaded image phys_base=0x%lx vm_low=0x%lx span=0x%lx segments=%u\r\n",
      (UINT64)load_result.host_base,
      load_result.lowest_vmaddr,
      load_result.image_size,
      load_result.segment_count);

  UINT64 entry_vmaddr = 0;
  VOID *host_entry = NULL;

  status = macho_find_entry_vmaddr(&image_info, &entry_vmaddr);
  if (EFI_ERROR(status)) {
    log_error(L"failed to find entry vmaddr: %r\r\n", status);
    macho_unload_contiguous(&ctx, &load_result);
    file_free(&ctx, &kernel);
    return status;
  }

  status = macho_compute_host_entry(&load_result, entry_vmaddr, &host_entry);
  if (EFI_ERROR(status)) {
    log_error(L"failed to compute host entry: %r\r\n", status);
    macho_unload_contiguous(&ctx, &load_result);
    file_free(&ctx, &kernel);
    return status;
  }

#ifdef VERBOSE_MACHO
  macho_log_entry_context(&image_info, entry_vmaddr);
  macho_log_section_host_info(&image_info, &load_result, "__HIB", "__bootPT");
  macho_dump_entry_bytes(host_entry, 32);
#endif // VERBOSE_MACHO

  log_info(L"entry vm=0x%lx -> host=0x%lx\r\n", entry_vmaddr, (UINT64)(UINTN)host_entry);

  BootArgsState boot_state = {0};

  status = boot_collect_memory_map(&ctx, &boot_state);
  if (EFI_ERROR(status)) {
    log_error(L"failed to collect memory map: %r\r\n", status);
    return status;
  }

  status = boot_build_args(&ctx, "-v debug=0x219 -nogzalloc_mode keepsyms=1 serial=3",
                           &load_result, &boot_state);
  if (EFI_ERROR(status)) {
    log_error(L"failed to build boot_args: %r\r\n", status);
    return status;
  }

  EFI_PHYSICAL_ADDRESS stack_base = 0xFFFFFFFFULL;
  UINTN stack_pages = 16;

  status = uefi_call_wrapper(
      ctx.bs->AllocatePages, 4,
      AllocateMaxAddress,
      EfiLoaderData,
      stack_pages,
      &stack_base);
  if (EFI_ERROR(status)) {
    log_error(L"failed to allocate stack: %r\r\n", status);
    return status;
  }

  VOID *stack_top = (VOID *)((UINT8 *)(UINTN)stack_base +
                             (stack_pages << EFI_PAGE_SHIFT) - 16);

#ifdef VERBOSE_BOOT
  boot_log_args(&boot_state);
#endif // VERBOSE_BOOT

  /* Reserve pages in the boot-info block (phys >= 0x100000) for the post-EBS
   * memory map relocation, so the map XNU walks survives pmap_lowmem_finalize
   * (see common.h XNU_BOOTINFO_BASE). 16 pages = 64KB, ample for the map. */
  EFI_PHYSICAL_ADDRESS mm_fixed = XNU_MEMMAP_PHYS;
  status = uefi_call_wrapper(ctx.bs->AllocatePages, 4,
      AllocateAddress, EfiLoaderData, 16, &mm_fixed);
  if (EFI_ERROR(status)) {
    log_error(L"reloc: failed to alloc mmap pages: %r\r\n", status);
    return status;
  }

#ifdef VERBOSE_BOOT
  log_info(L"[D] before exit_boot_services\r\n");
  log_info(L"boot_args phys = 0x%lx\r\n", (UINT64)(UINTN)boot_state.args);
  log_info(L"stack_top = 0x%lx\r\n",      (UINT64)(UINTN)stack_top);
  log_info(L"entry     = 0x%lx\r\n",      (UINT64)(UINTN)host_entry);
#endif // VERBOSE_BOOT

  /* No logging after this point */
  status = exit_boot_services_retry(&ctx, image, &boot_state);
  if (EFI_ERROR(status)) {
    log_error(L"ExitBootServices failed: %r\r\n", status);
    return status;
  }

  /* Copy staged kernel image to its SLID physical base (0x100000 + kslide).
   * boot.efi physically relocates the whole image: each segment goes to
   * (vmaddr + kslide) & 0x3FFFFFFF, and the local relocs (base = __HIB) patch
   * every absolute reference by += kslide -- including pstart's own immediates
   * (mov cr3, 0x106000 -> 0x506000; ljmp 0x10105c -> 0x50105c) so the
   * bootstrap runs correctly at the slid physical location.
   * XNU's i386_vm_init derives vm_kernel_slide = kaddr - 0x100000 and panics
   * ("inconsistent slide") unless it equals boot_args->kslide, so kaddr MUST
   * be the slid base too.
   * OVMF's memory (0x800000-0x1780000) is now reclaimed as conventional. */
  UINT64 phys_real  = 0x100000ULL + ctx.kslide;
  {
    UINT64 *src64 = (UINT64 *)(UINTN)ctx.kernel_region_base;
    UINT64 *dst64 = (UINT64 *)(UINTN)phys_real;
    UINTN   words = (UINTN)((load_result.image_size + 7) >> 3);
    for (UINTN i = 0; i < words; i++) dst64[i] = src64[i];
  }

  /* kaddr = slid physical base so vm_kernel_slide == kslide; entry runs at
   * the slid low-identity physical (pstart immediates were relocated). */
  UINT64 vm_base = load_result.lowest_vmaddr;
  INT64  vm_slide = (INT64)phys_real - (INT64)vm_base;

  VOID *phys_entry = (VOID *)(UINTN)((INT64)entry_vmaddr + vm_slide);

  boot_state.args->kaddr = (UINT32)phys_real;
  boot_state.args->kslide = ctx.kslide;

  /*
   * Kernel collection (KC) support, "approach B" (kc-builder / loader-aware).
   * The kernel keeps its own MH_EXECUTE header at fileoff 0 but carries the
   * MH_DYLIB_IN_CACHE flag (so XNU's kernel_mach_header_is_in_fileset() is
   * true).  The top-level MH_FILESET header lives in a private "__KCHDR"
   * segment appended by kc-builder.  XNU rebases the KC in i386_init IFF
   * boot_args->KC_hdrs_vaddr points at that MH_FILESET header (Version>=2,
   * Revision>=1 already set).  We must NOT run our own KASLR reloc pass for a
   * KC (already skipped at kslide==0).
   */
  if (image_info.header->flags & 0x80000000u /* MH_DYLIB_IN_CACHE */) {
    for (UINT32 si = 0; si < load_result.segment_count; si++) {
      const CHAR8 *n = load_result.segments[si].name;
      if (n[0]=='_' && n[1]=='_' && n[2]=='K' && n[3]=='C' &&
          n[4]=='H' && n[5]=='D' && n[6]=='R' && n[7]=='\0') {
        /* FINAL physical of __KCHDR after the staging->phys_real copy, NOT the
         * staging phys_target: XNU reads the KC header here and computes all KC
         * addresses relative to it. */
        boot_state.args->KC_hdrs_vaddr =
            phys_real + (load_result.segments[si].vmaddr - vm_base);
        break;
      }
    }
    /* NO log_info here: this runs post-ExitBootServices, ConOut is dead. */
  }

  /* Relocate the EFI memory map to mm_fixed and coalesce adjacent
   * same-type descriptors to keep entry count within XNU's zone limit. */
  {
    UINT8 *src = (UINT8 *)(UINTN)boot_state.args->MemoryMap;
    UINT8 *dst = (UINT8 *)(UINTN)mm_fixed;
    UINTN sz = (UINTN)boot_state.args->MemoryMapSize;
    UINTN dsz = (UINTN)boot_state.args->MemoryMapDescriptorSize;

    for (UINTN wi = 0; wi < (sz + 7) / 8; wi++)
      ((UINT64 *)dst)[wi] = ((UINT64 *)src)[wi];

    UINTN n = sz / dsz;
    for (UINTN i = 0; i < n; i++) {
      EFI_MEMORY_DESCRIPTOR *d = (EFI_MEMORY_DESCRIPTOR *)(dst + i * dsz);
      if (d->Type == EfiLoaderCode     ||
          d->Type == EfiLoaderData     ||
          d->Type == EfiBootServicesCode ||
          d->Type == EfiBootServicesData)
        d->Type = EfiConventionalMemory;
    }

    UINTN out = 0;
    for (UINTN i = 0; i < n; i++) {
      EFI_MEMORY_DESCRIPTOR *cur  = (EFI_MEMORY_DESCRIPTOR *)(dst + i  * dsz);
      EFI_MEMORY_DESCRIPTOR *prev = (EFI_MEMORY_DESCRIPTOR *)(dst + out * dsz);
      if (out > 0 &&
          cur->Type      == prev->Type &&
          cur->Attribute == prev->Attribute &&
          cur->PhysicalStart ==
              prev->PhysicalStart + (prev->NumberOfPages << EFI_PAGE_SHIFT)) {
        prev->NumberOfPages += cur->NumberOfPages;
      } else {
        if (out != i) {
          EFI_MEMORY_DESCRIPTOR *slot = (EFI_MEMORY_DESCRIPTOR *)(dst + out * dsz);
          for (UINTN b = 0; b < dsz; b++)
            ((UINT8 *)slot)[b] = ((UINT8 *)cur)[b];
        }
        out++;
      }
    }

    boot_state.args->MemoryMap     = (UINT32)mm_fixed;
    boot_state.args->MemoryMapSize = (UINT32)(out * dsz);
  }

  jump_to_xnu(
      phys_entry,
      (UINT64)(UINTN)boot_state.args,
      stack_top);
}