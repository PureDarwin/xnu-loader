#include "common.h"
#include "app.h"
#include "boot.h"
#include "console.h"
#include "fileio.h"
#include "macho.h"
#include "jump.h"

#if defined(__aarch64__)
EFI_PHYSICAL_ADDRESS g_xnu_bootinfo_base;
#endif


static EFI_STATUS ReserveBootInfoGuard(AppContext *ctx,
                                       EFI_PHYSICAL_ADDRESS *guard_base,
                                       UINTN *guard_pages) {
  EFI_PHYSICAL_ADDRESS base = XNU_BOOTINFO_BASE;
  UINTN pages = (UINTN)((XNU_BOOTINFO_END - XNU_BOOTINFO_BASE) >> EFI_PAGE_SHIFT);

  EFI_STATUS status = uefi_call_wrapper(ctx->bs->AllocatePages, 4,
      AllocateAddress, EfiLoaderData, pages, &base);
  if (EFI_ERROR(status)) {
    log_error(L"boot-info guard: AllocateAddress(0x%lx, %lu pages) failed: %r\r\n",
              (UINT64)XNU_BOOTINFO_BASE, (UINT64)pages, status);
    return status;
  }

  *guard_base = base;
  *guard_pages = pages;
  log_info(L"boot-info guard: reserved 0x%lx - 0x%lx\r\n",
           (UINT64)base,
           (UINT64)(base + ((UINT64)pages << EFI_PAGE_SHIFT)));
  return EFI_SUCCESS;
}

static VOID ReleaseBootInfoGuard(AppContext *ctx,
                                 EFI_PHYSICAL_ADDRESS *guard_base,
                                 UINTN *guard_pages) {
  if (*guard_pages == 0)
    return;

  EFI_STATUS status = uefi_call_wrapper(ctx->bs->FreePages, 2,
      *guard_base, *guard_pages);
  if (EFI_ERROR(status)) {
    log_error(L"boot-info guard: FreePages(0x%lx, %lu pages) failed: %r\r\n",
              (UINT64)*guard_base, (UINT64)*guard_pages, status);
    return;
  }

  log_info(L"boot-info guard: released for boot_build_args\r\n");
  *guard_base = 0;
  *guard_pages = 0;
}

EFI_STATUS AllocKernelMemRegion(AppContext *ctx, UINT64 span_bytes, UINT64 virt_base) {
#if defined(__aarch64__)
  UINT64 required_rem = virt_base & 0x1FFFFFULL;
  UINTN total_pages = (UINTN)((span_bytes + 0x200000ULL + EFI_PAGE_SIZE - 1) >> EFI_PAGE_SHIFT);
  EFI_PHYSICAL_ADDRESS base = 0x7FFFFFFF;

  EFI_STATUS status = uefi_call_wrapper(ctx->bs->AllocatePages, 4,
    AllocateMaxAddress, EfiLoaderData, total_pages, &base);
  if (EFI_ERROR(status)) {
    log_error(L"AllocKernelMemRegion: AllocatePages(%lu pages) failed: %r\r\n",
              (UINT64)total_pages, status);
    return status;
  }

  EFI_PHYSICAL_ADDRESS aligned = (base & ~0x1FFFFFULL) + required_rem;
  if (aligned < base) {
    aligned += 0x200000ULL;
  }

  SetMem((VOID *)(UINTN)base, (UINTN)((UINT64)total_pages << EFI_PAGE_SHIFT), 0);

  ctx->kernel_region_base = aligned;
  ctx->kernel_region_end  = aligned + span_bytes;

  log_info(L"kernel staging: 0x%lx - 0x%lx (%lu pages, 2MB-aligned to virt_base rem 0x%lx)\r\n",
       ctx->kernel_region_base, ctx->kernel_region_end, (UINT64)total_pages, required_rem);
  return EFI_SUCCESS;
#else
  (VOID)virt_base;
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
#endif
}

EFI_STATUS efi_main(EFI_HANDLE image, EFI_SYSTEM_TABLE *st) {
  AppContext ctx;
  FileBuffer kernel = {0};
  MachoImage image_info;
  MachoLoadResult load_result = {0};
  EFI_STATUS status;
  EFI_PHYSICAL_ADDRESS bootinfo_guard_base = 0;
  UINTN bootinfo_guard_pages = 0;

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
    log_info(L"no local kernel file (%r); trying TFTP (netboot)\r\n", status);
    status = file_read_all_via_tftp(&ctx, (CONST CHAR8 *)"EFI/BOOT/kernel", &kernel);
  }

  if (EFI_ERROR(status)) {
    log_error(L"failed to read kernel from any volume or TFTP: %r\r\n", status);
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

#if defined(__aarch64__)
    /*
     * Place the boot-info block 32MB below the kernel's own linked
     * phys_base, 2MB-aligned - clear of both the kernel image itself and
     * (empirically, under QEMU's raspi3b + u-boot) whatever u-boot/DTB
     * carve-outs occupy low memory. See common.h's XNU_BOOTINFO_BASE
     * comment for why this can't be the x86_64 fixed 0x2800000.
     */
    g_xnu_bootinfo_base = (ctx.phys_base > 0x2200000ULL)
        ? ((ctx.phys_base - 0x2200000ULL) & ~0x1FFFFFULL)
        : 0x2800000ULL;
    log_info(L"arm64 bootinfo base=0x%lx\r\n", (UINT64)g_xnu_bootinfo_base);
#endif
  }

  /* Protect the fixed boot-info block before allocating the large contiguous
   * staging image. Without this guard, AllocateMaxAddress may return a range
   * that crosses XNU_BOOTINFO_BASE; debug kernels are large enough to expose
   * that collision. The guard is released once staging is fixed in place,
   * immediately before boot_build_args() claims the addresses for real. */
  status = ReserveBootInfoGuard(&ctx, &bootinfo_guard_base, &bootinfo_guard_pages);
  if (EFI_ERROR(status)) {
    file_free(&ctx, &kernel);
    return status;
  }

  /* Allocate a high staging buffer for the kernel image. */
  {
    UINT64 lo2 = 0, hi2 = 0;
    macho_compute_vm_range_pub(&image_info, &lo2, &hi2);
    status = AllocKernelMemRegion(&ctx, hi2 - lo2, lo2);
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
     * pstart fixup is needed, boot.efi doesn't do one either. */

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

  ReleaseBootInfoGuard(&ctx, &bootinfo_guard_base, &bootinfo_guard_pages);
  if (bootinfo_guard_pages != 0) {
    file_free(&ctx, &kernel);
    return EFI_DEVICE_ERROR;
  }

  status = boot_collect_memory_map(&ctx, &boot_state);
  if (EFI_ERROR(status)) {
    log_error(L"failed to collect memory map: %r\r\n", status);
    return status;
  }

  CONST CHAR8 *cmdline = "-v debug=0x219 -nogzalloc_mode keepsyms=1 serial=3 gopconsole=1";
  FileBuffer boot_args_file = {0};
  EFI_STATUS args_status = file_read_all_from_any_volume(
      &ctx,
      L"\\EFI\\BOOT\\boot-args.txt",
      &boot_args_file,
      NULL);

  if (!EFI_ERROR(args_status) && boot_args_file.size > 0) {
    UINTN len = boot_args_file.size;
    CHAR8 *bytes = (CHAR8 *)boot_args_file.data;
    /* Trim trailing CR/LF/whitespace a text editor may have left. */
    while (len > 0 &&
           (bytes[len - 1] == '\n' || bytes[len - 1] == '\r' ||
            bytes[len - 1] == ' '  || bytes[len - 1] == '\t'))
      len--;

    if (len > 0) {
      CHAR8 *copy = NULL;
      status = uefi_call_wrapper(ctx.bs->AllocatePool, 3, EfiLoaderData, len + 1, (VOID **)&copy);
      if (!EFI_ERROR(status)) {
        CopyMem(copy, bytes, len);
        copy[len] = '\0';
        cmdline = copy;
        log_info(L"boot-args.txt: using \"%a\"\r\n", cmdline);
      } else {
        log_error(L"boot-args.txt: AllocatePool failed (%r), using default\r\n", status);
      }
    }
    file_free(&ctx, &boot_args_file);
  } else {
    log_info(L"no boot-args.txt found (%r); using default boot args\r\n", args_status);
  }

  status = boot_build_args(&ctx, cmdline, &load_result, &boot_state);
  if (EFI_ERROR(status)) {
    log_error(L"failed to build boot_args: %r\r\n", status);
    return status;
  }

#if defined(__aarch64__)
  /*
   * boot_state.args (the x86_64 boot_args struct boot_build_args just
   * filled in) is unused scratch for arm64 from here on - only kept
   * around because boot_collect_memory_map/exit_boot_services_retry
   * below are shared EFI-bookkeeping machinery that happen to take a
   * BootArgsState*. The struct arm64 XNU actually needs (see boot.h) is
   * built here instead, reusing the device tree boot_build_args already
   * built via dt_build() into boot_state.device_tree.
   *
   * Must happen before exit_boot_services_retry (below): it calls
   * AllocatePages, which isn't legal once boot services have exited.
   */
  LowMemBuffer arm64_args_buf = {0};
  arm64_boot_args *arm64_args = NULL;
  {
    UINT64 image_size_aligned = (load_result.image_size + EFI_PAGE_SIZE - 1) &
                                ~(UINT64)(EFI_PAGE_SIZE - 1);
    status = arm64_boot_build_args(
        &ctx,
        cmdline,
        load_result.lowest_vmaddr,               /* virtBase */
        ctx.kernel_region_base,                  /* physBase - staging IS final for arm64 */
        app_detect_physical_memory_size(&ctx),   /* memSize */
        ctx.kernel_region_base + image_size_aligned, /* topOfKernelData */
        (UINT64)(UINTN)boot_state.device_tree,
        boot_state.device_tree_size,
        &arm64_args_buf,
        &arm64_args);
    if (EFI_ERROR(status)) {
      log_error(L"failed to build arm64 boot_args: %r\r\n", status);
      return status;
    }
  }
#endif

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
#if defined(__aarch64__)
  arm64_boot_log_args(arm64_args);
#else
  boot_log_args(&boot_state);
#endif
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
#if defined(__aarch64__)
  log_info(L"boot_args phys = 0x%lx\r\n", (UINT64)(UINTN)arm64_args);
#else
  log_info(L"boot_args phys = 0x%lx\r\n", (UINT64)(UINTN)boot_state.args);
#endif
  log_info(L"stack_top = 0x%lx\r\n",      (UINT64)(UINTN)stack_top);
  log_info(L"entry     = 0x%lx\r\n",      (UINT64)(UINTN)host_entry);
#endif // VERBOSE_BOOT

  /* No logging after this point */
  status = exit_boot_services_retry(&ctx, image, &boot_state);
  if (EFI_ERROR(status)) {
    log_error(L"ExitBootServices failed: %r\r\n", status);
    return status;
  }

#if defined(__x86_64__)
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
#else
  /*
   * Arm64 doesn't need any of that: the kernel is already sitting exactly
   * where we staged it (ctx.kernel_region_base), and host_entry (computed
   * earlier via macho_compute_host_entry, before this function ever
   * touched phys_entry) is already the correct physical entry address in
   * that same staging region.
   */
  UINT64 vm_base = load_result.lowest_vmaddr;
  UINT64 phys_real = ctx.kernel_region_base;
  VOID *phys_entry = host_entry;
#endif

  /*
   * Kernel collection (KC) support. kc-builder converts the kernel's own
   * mach_header_64 in place into the top-level MH_FILESET header (appending
   * LC_FILESET_ENTRY/LC_DYLD_CHAINED_FIXUPS onto its existing load command
   * list, reusing its existing LC_SEGMENT_64 entries) and sets
   * MH_DYLIB_IN_CACHE (so XNU's kernel_mach_header_is_in_fileset() is true).
   * kc_mh is therefore the exact same object as the kernel's own header,
   * which lives at the start of whichever segment has fileoff==0 (__TEXT --
   * NOT necessarily the lowest-vmaddr segment: here __HIB sits at a lower
   * vmaddr than __TEXT despite coming later in the file, so phys_real/
   * vm_base, which track the lowest vmaddr, point at the wrong segment for
   * this). An earlier revision appended a synthetic "__KCHDR" wrapper
   * elsewhere, which broke kernel_collection_slide()'s internal slide
   * computation: see kc-tools/src/fileset.c's top-of-file comment for the
   * full story. XNU rebases the KC in i386_init IFF boot_args->KC_hdrs_vaddr
   * is set (Version>=2, Revision>=1 already set). We must NOT run our own
   * KASLR reloc pass for a KC (already skipped at kslide==0).
   */
  if (image_info.header->flags & 0x80000000u /* MH_DYLIB_IN_CACHE */) {
    for (UINT32 si = 0; si < load_result.segment_count; si++) {
      if (load_result.segments[si].fileoff == 0) {
        /* Same uniform vmaddr->physical delta the loader used for every
         * segment (phys_real is anchored to vm_base = lowest vmaddr). */
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
#if defined(__aarch64__)
      (UINT64)(UINTN)arm64_args,
#else
      (UINT64)(UINTN)boot_state.args,
#endif
      stack_top);
}
