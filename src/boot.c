#include "boot.h"
#include "console.h"
#include "devtree.h"
#include "fileio.h"
#include <efiprot.h>

static UINT64 align_up_u64(UINT64 v, UINT64 a) {
  return (v + (a - 1)) & ~(a - 1);
}

/* Walk ACPI XSDT to find a table by 4-byte signature. Returns physical
 * address of the table header, or 0 if not found. */
static UINT64 acpi_find_table(AppContext *ctx, const CHAR8 *sig) {
  static const EFI_GUID acpi20 = ACPI_20_TABLE_GUID;

  /* Find RSDP from EFI configuration table */
  UINT64 rsdp_addr = 0;
  for (UINTN i = 0; i < ctx->st->NumberOfTableEntries; i++) {
    EFI_CONFIGURATION_TABLE *e = &ctx->st->ConfigurationTable[i];
    if (CompareMem(&e->VendorGuid, &acpi20, sizeof(EFI_GUID)) == 0) {
      rsdp_addr = (UINT64)(UINTN)e->VendorTable;
      break;
    }
  }
  if (!rsdp_addr) return 0;

  /* RSDP: +24 = XSDT physical address (8 bytes) */
  UINT64 xsdt_addr = *(UINT64 *)(UINTN)(rsdp_addr + 24);
  if (!xsdt_addr) return 0;

  /* XSDT header: +4 = length (4 bytes); entries start at +36, each 8 bytes */
  UINT32 xsdt_len = *(UINT32 *)(UINTN)(xsdt_addr + 4);
  UINTN  n_entries = (xsdt_len - 36) / 8;
  UINT64 *entries  = (UINT64 *)(UINTN)(xsdt_addr + 36);

  for (UINTN i = 0; i < n_entries; i++) {
    UINT64 tbl = entries[i];
    if (!tbl) continue;
    CHAR8 *s = (CHAR8 *)(UINTN)tbl;
    if (s[0] == sig[0] && s[1] == sig[1] && s[2] == sig[2] && s[3] == sig[3])
      return tbl;
  }
  return 0;
}

static UINTN boot_ascii_len(const CHAR8 *s) {
  UINTN n = 0;
  if (!s)
    return 0;
  while (s[n] != '\0')
    n++;
  return n;
}

EFI_STATUS boot_collect_memory_map(
    AppContext *ctx,
    BootArgsState *state)
{
  EFI_STATUS status;
  UINTN map_size;
  EFI_MEMORY_DESCRIPTOR *tmp_map;
  UINTN key;
  UINTN desc_size;
  UINT32 desc_ver;
  LowMemBuffer low_map;

  if (!ctx || !state)
    return EFI_INVALID_PARAMETER;

  state->memory_map = NULL;
  state->memory_map_size = 0;
  state->memory_map_key = 0;
  state->descriptor_size = 0;
  state->descriptor_version = 0;
  state->memory_map_buf.ptr = NULL;
  state->memory_map_buf.phys = 0;
  state->memory_map_buf.size = 0;
  state->memory_map_buf.pages = 0;

  map_size = 0;
  tmp_map = NULL;
  key = 0;
  desc_size = 0;
  desc_ver = 0;

  status = uefi_call_wrapper(
      ctx->bs->GetMemoryMap,
      5,
      &map_size,
      tmp_map,
      &key,
      &desc_size,
      &desc_ver);

  if (status != EFI_BUFFER_TOO_SMALL)
    return status;

  map_size += desc_size * 8;

  status = app_alloc_pool(ctx, map_size, (VOID **)&tmp_map);
  if (EFI_ERROR(status))
    return status;

  status = uefi_call_wrapper(
      ctx->bs->GetMemoryMap,
      5,
      &map_size,
      tmp_map,
      &key,
      &desc_size,
      &desc_ver);
  if (EFI_ERROR(status)) {
    app_free_pool(ctx, tmp_map);
    return status;
  }

  status = lowmem_realloc_copy(
      ctx,
      tmp_map,
      map_size,
      EfiLoaderData,
      &low_map);

  app_free_pool(ctx, tmp_map);

  if (EFI_ERROR(status))
    return status;

  state->memory_map = (EFI_MEMORY_DESCRIPTOR *)low_map.ptr;
  state->memory_map_size = map_size;
  state->memory_map_key = key;
  state->descriptor_size = desc_size;
  state->descriptor_version = desc_ver;
  state->memory_map_buf = low_map;

  return EFI_SUCCESS;
}

EFI_STATUS boot_refresh_memory_map(
    AppContext *ctx,
    BootArgsState *state)
{
  EFI_STATUS status;
  UINTN map_size;
  UINTN key;
  UINTN desc_size;
  UINT32 desc_ver;

  if (!ctx || !state)
    return EFI_INVALID_PARAMETER;

  for (;;) {
    map_size = state->memory_map_buf.size;
    key = 0;
    desc_size = 0;
    desc_ver = 0;

    if (state->memory_map_buf.ptr == NULL) {
      LowMemBuffer new_buf;
      status = lowmem_alloc_pages(ctx, EFI_PAGE_SIZE * 4, EfiLoaderData, &new_buf);
      if (EFI_ERROR(status))
        return status;
      state->memory_map_buf = new_buf;
      state->memory_map = (EFI_MEMORY_DESCRIPTOR *)new_buf.ptr;
      state->memory_map_size = 0;
    }

    map_size = state->memory_map_buf.pages << EFI_PAGE_SHIFT;

    status = uefi_call_wrapper(
        ctx->bs->GetMemoryMap,
        5,
        &map_size,
        state->memory_map_buf.ptr,
        &key,
        &desc_size,
        &desc_ver);

    if (status == EFI_BUFFER_TOO_SMALL) {
      LowMemBuffer old_buf = state->memory_map_buf;
      LowMemBuffer new_buf;
      UINTN needed = map_size + desc_size * 8;

      status = lowmem_alloc_pages(ctx, needed, EfiLoaderData, &new_buf);
      if (EFI_ERROR(status))
        return status;

      state->memory_map_buf = new_buf;
      state->memory_map = (EFI_MEMORY_DESCRIPTOR *)new_buf.ptr;

      if (old_buf.ptr != NULL)
        lowmem_free(ctx, &old_buf);

      continue;
    }

    if (EFI_ERROR(status))
      return status;

    state->memory_map = (EFI_MEMORY_DESCRIPTOR *)state->memory_map_buf.ptr;
    state->memory_map_size = map_size;
    state->memory_map_key = key;
    state->descriptor_size = desc_size;
    state->descriptor_version = desc_ver;
    return EFI_SUCCESS;
  }
}

VOID boot_update_args_memory_map(AppContext *ctx, BootArgsState *state) {
  if (!state || !state->args)
    return;

  state->args->MemoryMap = (UINT32)state->memory_map_buf.phys;
  state->args->MemoryMapSize = (UINT32)state->memory_map_size;
  state->args->MemoryMapDescriptorSize = (UINT32)state->descriptor_size;
  state->args->MemoryMapDescriptorVersion = state->descriptor_version;
  state->args->PhysicalMemorySize = app_detect_physical_memory_size(ctx);
}

EFI_STATUS boot_set_command_line(BootArgsState *state, boot_args *args, const CHAR8 *cmdline) {
  if (!state || !args || !cmdline)
    return EFI_INVALID_PARAMETER;

  SetMem(args->CommandLine, BOOT_LINE_LENGTH, 0);

  UINTN len = boot_ascii_len(cmdline);
  if (len >= BOOT_LINE_LENGTH)
    len = BOOT_LINE_LENGTH - 1;

  CopyMem(args->CommandLine, cmdline, len);
  args->CommandLine[len] = '\0';

  return EFI_SUCCESS;
}

EFI_STATUS boot_build_args(
    AppContext *ctx,
    const CHAR8 *cmdline,
    MachoLoadResult *load_result,
    BootArgsState *state)
{
  EFI_STATUS status;
  LowMemBuffer args_buf;
  boot_args *args;

  if (!ctx || !load_result || !state || !state->memory_map)
    return EFI_INVALID_PARAMETER;

  if (state->args != NULL)
    return EFI_ALREADY_STARTED;

  {
    EFI_PHYSICAL_ADDRESS args_phys = XNU_BOOTARGS_PHYS;
    status = uefi_call_wrapper(ctx->bs->AllocatePages, 4,
        AllocateAddress, EfiLoaderData, 1, &args_phys);
    if (EFI_ERROR(status)) {
      log_error(L"boot_args: AllocateAddress(0x%lx) failed: %r\r\n",
                (UINT64)XNU_BOOTARGS_PHYS, status);
      return status;
    }
    args_buf.ptr = (VOID *)(UINTN)args_phys;
    args_buf.phys = args_phys;
    args_buf.size = sizeof(boot_args);
    args_buf.pages = 1;
  }

  args = (boot_args *)args_buf.ptr;
  SetMem(args, sizeof(boot_args), 0);

  state->args = args;
  state->args_buf = args_buf;

  status = boot_set_command_line(state, args, cmdline);
  if (EFI_ERROR(status))
    return status;

  /* Copy EFI tables to conventional memory before building the device tree
   * so state->rt_table_phys is valid when dt_build stores the runtime-services
   * table address in /efi/runtime-services/table.
   * OVMF places EFI_SYSTEM_TABLE and EFI_RUNTIME_SERVICES in runtime-services
   * pages (type 5/6) which XNU excludes from pmap_memory_regions. After
   * pmap_bootstrap the physmap doesn't cover those pages, so phys_to_kva()
   * produces KVAs that fault on access. Copy the structs to a pinned page in
   * conventional memory (always in the physmap) so XNU can read them. */
  {
    EFI_PHYSICAL_ADDRESS tbl_phys = XNU_EFITABLES_PHYS;
    if (!EFI_ERROR(uefi_call_wrapper(ctx->bs->AllocatePages, 4,
            AllocateAddress, EfiLoaderData, 1, &tbl_phys))) {
      UINT8 *page = (UINT8 *)(UINTN)tbl_phys;
      EFI_SYSTEM_TABLE *st_copy = (EFI_SYSTEM_TABLE *)page;
      EFI_RUNTIME_SERVICES *rt_copy = (EFI_RUNTIME_SERVICES *)(page + 0x200);
      state->rt_table_phys = (UINT64)(UINTN)rt_copy;

      XnuCopyMem(st_copy, ctx->st, sizeof(EFI_SYSTEM_TABLE));
      XnuCopyMem(rt_copy, ctx->st->RuntimeServices, sizeof(EFI_RUNTIME_SERVICES));
      /*
       * XNU's efi_set_tables_64() dereferences SystemTable->RuntimeServices
       * DIRECTLY (no ml_static_ptovirt), unlike the configuration-table walk.
       * So this pointer must already be the kernel physmap VA
       * (VM_MIN_KERNEL_ADDRESS | phys), not the raw physical address, or the
       * first access faults in the user VA range and crashes in vm_fault
       * (current_task() is still NULL this early).
       */
      st_copy->RuntimeServices =
          (EFI_RUNTIME_SERVICES *)(UINTN)(0xFFFFFF8000000000ULL |
                                          (UINT64)(UINTN)rt_copy);

      /* Also copy the EFI configuration table array to conventional memory
       * so XNU can walk it to find the ACPI RSDP.  The array pointer in the
       * original system table often points to EFI runtime/boot-services data
       * which is excluded from XNU's physmap after pmap_bootstrap.
       *
       * Layout: page+0x000=EFI_SYSTEM_TABLE, page+0x200=EFI_RUNTIME_SERVICES,
       *         page+0x400=EFI_CONFIGURATION_TABLE[n] */
      if (ctx->st->NumberOfTableEntries > 0 && ctx->st->ConfigurationTable) {
        EFI_CONFIGURATION_TABLE *cfg_copy =
            (EFI_CONFIGURATION_TABLE *)(page + 0x400);
        UINTN cfg_size =
            ctx->st->NumberOfTableEntries * sizeof(EFI_CONFIGURATION_TABLE);
        XnuCopyMem(cfg_copy, ctx->st->ConfigurationTable, cfg_size);
        st_copy->ConfigurationTable = cfg_copy;
        st_copy->NumberOfTableEntries = ctx->st->NumberOfTableEntries;
        log_info(L"EFI config table: %lu entries copied to 0x%lx\r\n",
                 ctx->st->NumberOfTableEntries, (UINT64)(UINTN)cfg_copy);
      }

      /* Recompute the EFI_SYSTEM_TABLE CRC32 after modifying RuntimeServices
       * and ConfigurationTable.  XNU verifies CRC32 in both efi_set_tables_64
       * and efi_get_cfgtbl_by_guid; a stale checksum makes both return early,
       * breaking ACPI RSDP lookup and EFI runtime services setup. */
      {
        UINT32 new_crc = 0;
        st_copy->Hdr.CRC32 = 0;
        uefi_call_wrapper(ctx->bs->CalculateCrc32, 3,
                          st_copy, st_copy->Hdr.HeaderSize, &new_crc);
        st_copy->Hdr.CRC32 = new_crc;
        log_info(L"EFI_SYSTEM_TABLE CRC32 recomputed: 0x%x\r\n", new_crc);
      }

      args->efiSystemTable = (UINT32)(UINTN)tbl_phys;
      log_info(L"EFI tables copied to 0x%lx (st) / 0x%lx (rt)\r\n",
               (UINT64)tbl_phys, (UINT64)(tbl_phys + 0x200));
    } else {
      args->efiSystemTable = (UINT32)(UINTN)ctx->st;
      log_info(L"EFI table copy failed, using original 0x%lx\r\n",
               (UINT64)(UINTN)ctx->st);
    }

    /* Record physical page range for runtime-flagged descriptors.
     * Virtual page start is filled in after SetVirtualAddressMap in
     * exit_boot_services_retry once we know the actual virtual addresses. */
    {
      UINT8 *map = (UINT8 *)state->memory_map;
      UINT32 desc_sz = (UINT32)state->descriptor_size;
      UINT32 map_sz = (UINT32)state->memory_map_size;
      UINT32 min_pg = 0xFFFFFFFFU;
      UINT32 max_pg = 0;

      for (UINT32 off = 0; off + desc_sz <= map_sz; off += desc_sz) {
        EFI_MEMORY_DESCRIPTOR *d = (EFI_MEMORY_DESCRIPTOR *)(map + off);
        if (!(d->Attribute & EFI_MEMORY_RUNTIME))
          continue;
        UINT32 pg_start = (UINT32)(d->PhysicalStart >> EFI_PAGE_SHIFT);
        UINT32 pg_end   = pg_start + (UINT32)d->NumberOfPages;
        if (pg_start < min_pg) min_pg = pg_start;
        if (pg_end   > max_pg) max_pg = pg_end;
      }

      if (min_pg < max_pg) {
        args->efiRuntimeServicesPageStart = min_pg;
        args->efiRuntimeServicesPageCount = max_pg - min_pg;
        /* efiRuntimeServicesVirtualPageStart is set after SVAM. */
      }
    }

    args->csrActiveConfig = 0;
  }

  if (state->device_tree == NULL) {
    status = dt_build(
        ctx,
        &state->device_tree,
        &state->device_tree_size,
        &state->device_tree_buf,
        cmdline,
        state->rt_table_phys);
    if (EFI_ERROR(status)) {
      lowmem_free(ctx, &args_buf);
      return status;
    }
  }

  /* boot.efi sets Revision=kBootArgsRevision=1 and Version=kBootArgsVersion2=2.
   * XNU checks Version to select struct layout; Version=2 enables efiMode/flags. */
  args->Revision = 1;
  args->Version  = 2;

  args->efiMode = 64;
  args->debugMode = 0;
  /* boot.efi always sets bit 0; additional bits come from boot-arg parsing.
   * We set bit 0 (kBootArgsFlagHiDPI baseline) to match. */
  args->flags = 1;

  args->MemoryMap = (UINT32)(UINTN)state->memory_map;
  args->MemoryMapSize = (UINT32)state->memory_map_size;
  args->MemoryMapDescriptorSize = (UINT32)state->descriptor_size;
  args->MemoryMapDescriptorVersion = state->descriptor_version;
  args->PhysicalMemorySize = app_detect_physical_memory_size(ctx);

  args->deviceTreeP = (UINT32)(UINTN)state->device_tree;
  args->deviceTreeLength = state->device_tree_size;

  args->kaddr = (UINT32)load_result->host_base;
  args->ksize = (UINT32)align_up_u64(load_result->image_size, EFI_PAGE_SIZE);

  args->KC_hdrs_vaddr = 0;
  args->kslide = ctx->kslide;

  /* FSBFrequency: Ivy Bridge-E (Mac Pro 6,1) uses 100 MHz BCLK.
   * XNU's tsc_init reads this from boot_args if the DT is unavailable. */
  args->FSBFrequency = 100000000ULL;

  /* pciConfigSpaceBaseAddress / StartBus / EndBus: from ACPI MCFG.
   * boot.efi reads MCFG[0] allocation structure at offset +44. */
  {
    UINT64 mcfg = acpi_find_table(ctx, "MCFG");
    if (mcfg) {
      /* MCFG allocation structure starts at offset 44 (36 header + 8 reserved).
       * BaseAddress[8] at +44, SegmentGroup[2] at +52, StartBus[1] at +54,
       * EndBus[1] at +55. */
      args->pciConfigSpaceBaseAddress   = *(UINT64 *)(UINTN)(mcfg + 44);
      args->pciConfigSpaceStartBusNumber = *(UINT8  *)(UINTN)(mcfg + 54);
      args->pciConfigSpaceEndBusNumber   = *(UINT8  *)(UINTN)(mcfg + 55);
      log_info(L"MCFG: PCI base=0x%lx bus=%u-%u\r\n",
               args->pciConfigSpaceBaseAddress,
               args->pciConfigSpaceStartBusNumber,
               args->pciConfigSpaceEndBusNumber);
    } else {
      log_info(L"MCFG: not found in ACPI\r\n");
    }
  }

  status = boot_fill_video(ctx, args);
  if (EFI_ERROR(status)) {
    lowmem_free(ctx, &args_buf);
    return status;
  }

  state->args = args;
  state->args_buf = args_buf;

#ifdef VERBOSE_BOOT
  log_info(L"sizeof(boot_args) = 0x%lx\r\n", (UINT64)sizeof(boot_args));
  log_info(L"boot_args low phys = 0x%lx\r\n", (UINT64)state->args_buf.phys);
  log_info(L"device_tree low phys = 0x%lx\r\n", (UINT64)state->device_tree_buf.phys);
  log_info(L"memory_map ptr  = 0x%lx\r\n", (UINT64)(UINTN)state->memory_map);
  log_info(L"memory_map phys = 0x%lx\r\n", (UINT64)state->memory_map_buf.phys);
#endif // VERBOSE_BOOT

  return EFI_SUCCESS;
}

EFI_STATUS boot_fill_video(
    AppContext *ctx,
    boot_args *args)
{
  if (!ctx || !args)
      return EFI_INVALID_PARAMETER;

  EFI_GRAPHICS_OUTPUT_PROTOCOL *gop = NULL;

  EFI_STATUS status = uefi_call_wrapper(
      ctx->bs->LocateProtocol,
      3,
      &gEfiGraphicsOutputProtocolGuid,
      NULL,
      (VOID **)&gop);

  /* No GOP (headless firmware, no display attached, UGA-only board, etc.):
   * leave args->Video/VideoV1 zeroed (v_display=0) and continue - XNU boots
   * fine on the serial console alone (serial=3 in the boot-args cmdline)
   * without a framebuffer. Only actual protocol/mode errors after a GOP was
   * located are treated as fatal, since those indicate a broken GOP rather
   * than its absence. */
  if (EFI_ERROR(status) || !gop || !gop->Mode || !gop->Mode->Info) {
    log_info(L"boot_fill_video: no usable GOP (%r), continuing headless\r\n", status);
    return EFI_SUCCESS;
  }

  /* XNU needs a LINEAR framebuffer.  The firmware's current GOP mode is
   * normally the native panel resolution with a linear framebuffer, so keep it
   * when it is RGBX(0)/BGRX(1).  Only if the current mode is BltOnly/bitmask
   * (no CPU-addressable framebuffer) do we search for the highest-resolution
   * linear mode and switch to it. */
  {
    EFI_GRAPHICS_PIXEL_FORMAT curfmt = gop->Mode->Info->PixelFormat;
    BOOLEAN cur_linear =
        (curfmt == PixelRedGreenBlueReserved8BitPerColor ||
         curfmt == PixelBlueGreenRedReserved8BitPerColor);

    if (!cur_linear) {
      UINT32 best_mode = gop->Mode->MaxMode; /* sentinel = none found */
      UINT64 best_px   = 0;
      for (UINT32 m = 0; m < gop->Mode->MaxMode; m++) {
        EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *mi = NULL;
        UINTN misz = 0;
        if (EFI_ERROR(uefi_call_wrapper(gop->QueryMode, 4, gop, m, &misz, &mi)) || !mi)
          continue;
        if (mi->PixelFormat != PixelRedGreenBlueReserved8BitPerColor &&
            mi->PixelFormat != PixelBlueGreenRedReserved8BitPerColor)
          continue;
        UINT64 px = (UINT64)mi->HorizontalResolution * mi->VerticalResolution;
        if (px > best_px) {
          best_px   = px;
          best_mode = m;
        }
      }
      if (best_mode == gop->Mode->MaxMode) {
        log_info(L"boot_fill_video: current mode not linear and no linear mode "
                 L"found, continuing headless\r\n");
        return EFI_SUCCESS;
      }
      log_info(L"boot_fill_video: current mode not linear, switching to mode %u\r\n",
               best_mode);
      status = uefi_call_wrapper(gop->SetMode, 2, gop, best_mode);
      if (EFI_ERROR(status)) {
        log_info(L"boot_fill_video: SetMode(%u) failed: %r, continuing headless\r\n",
                 best_mode, status);
        return EFI_SUCCESS;
      }
    }
  }

  UINT32 width = gop->Mode->Info->HorizontalResolution;
  UINT32 height = gop->Mode->Info->VerticalResolution;
  UINT32 stride = gop->Mode->Info->PixelsPerScanLine * 4;
  UINT64 fb_base  = gop->Mode->FrameBufferBase;

  UINT8 pixel_fmt = (UINT8)gop->Mode->Info->PixelFormat;

#ifdef VERBOSE_BOOT
  log_info(L"boot_fill_video: %ux%u stride=%u fb=0x%lx pixfmt=%u\r\n",
           width, height, stride, fb_base, (UINT32)pixel_fmt);
#endif // VERBOSE_BOOT

  /* This extended Boot_Video (offset 1192, 64-bit v_baseAddr at 1240) is the
   * struct XNU's PE_init_platform actually consumes.  v_rotate is the DISPLAY
   * ROTATION (0/1/2/3 = 0/90/180/270 deg), XNU swaps v_width/v_height when
   * it is 1 or 3.  It is NOT the pixel format; it MUST be 0 or the panel is
   * rotated and the geometry transposed. */
  args->Video.v_display  = GRAPHICS_MODE;
  args->Video.v_rowBytes = stride;
  args->Video.v_width    = width;
  args->Video.v_height   = height;
  args->Video.v_depth    = 32;
  args->Video.v_rotate   = 0;
  args->Video.v_baseAddr = fb_base;
  (void)pixel_fmt;

  /* VideoV1 (the struct XNU reads at boot_args+1048): base addr is 32-bit.
   * XNU's Boot_Video.v_baseAddr is 32-bit, so a framebuffer above 4GB cannot
   * be described, warn (some discrete GPUs place the FB high, but Macs and
   * most laptop iGPUs keep it below 4GB). */
  if (fb_base > 0xFFFFFFFFULL)
    log_error(L"boot_fill_video: WARNING framebuffer 0x%lx > 4GB, truncated\r\n",
              fb_base);
  args->VideoV1.v_baseAddr = (UINT32)fb_base;
  args->VideoV1.v_display  = GRAPHICS_MODE;
  args->VideoV1.v_rowBytes = stride;
  args->VideoV1.v_width    = width;
  args->VideoV1.v_height   = height;
  args->VideoV1.v_depth    = 32;

  return EFI_SUCCESS;
}

VOID boot_free_args(
    AppContext *ctx,
    BootArgsState *state)
{
  if (!ctx || !state)
    return;

  if (state->device_tree_buf.ptr != NULL)
    lowmem_free(ctx, &state->device_tree_buf);

  state->device_tree = NULL;
  state->device_tree_size = 0;

  if (state->args_buf.ptr != NULL)
    lowmem_free(ctx, &state->args_buf);

  state->args = NULL;

  if (state->memory_map_buf.ptr != NULL)
    lowmem_free(ctx, &state->memory_map_buf);

  state->memory_map = NULL;
  state->memory_map_size = 0;
  state->memory_map_key = 0;
  state->descriptor_size = 0;
  state->descriptor_version = 0;
}

VOID boot_log_args(BootArgsState *state) {
  if (!state || !state->args)
    return;

  log_info(L"boot_args @ 0x%lx\r\n", (UINT64)(UINTN)state->args);
  log_info(L"  Revision: %u\r\n", state->args->Revision);
  log_info(L"  Version: %u\r\n", state->args->Version);
  log_info(L"  deviceTreeP: 0x%x\r\n", state->args->deviceTreeP);
  log_info(L"  deviceTreeLength: 0x%x\r\n", state->args->deviceTreeLength);
  log_info(L"  Video base: 0x%x\r\n", state->args->Video.v_baseAddr);
  log_info(L"  Video %ux%u depth=%u rowBytes=%u\r\n",
      state->args->Video.v_width,
      state->args->Video.v_height,
      state->args->Video.v_depth,
      state->args->Video.v_rowBytes);
  log_info(L"  CommandLine: %a\r\n", state->args->CommandLine);
  log_info(L"  MemoryMap: 0x%x\r\n", state->args->MemoryMap);
  log_info(L"  MemoryMapSize: 0x%x\r\n", state->args->MemoryMapSize);
  log_info(L"  DescriptorSize: 0x%x\r\n", state->args->MemoryMapDescriptorSize);
  log_info(L"  kaddr: 0x%x\r\n", state->args->kaddr);
  log_info(L"  ksize: 0x%x\r\n", state->args->ksize);
  log_info(L"  efiSystemTable: 0x%x\r\n", state->args->efiSystemTable);
  log_info(L"  efiMode: %u\r\n", state->args->efiMode);
}


/*
 * Sort the EFI memory map ascending by PhysicalStart, in place.
 *
 * boot.efi calls SortMemoryMap around SetVirtualAddressMap; an ordered map is
 * what XNU's efi_init walks and lets us assign a single monotonically-increasing
 * packed virtual range.  Runs AFTER ExitBootServices, so it must not use any
 * BootServices call, CopyMem here is the loader's own XnuCopyMem.  descriptor
 * records are ~48 bytes; guard against anything larger than the temp buffer.
 */
static VOID boot_sort_memory_map(UINT8 *map, UINTN map_sz, UINTN desc_sz) {
  if (!map || desc_sz == 0)
    return;
  UINTN n = map_sz / desc_sz;
  UINT8 tmp[512];
  if (desc_sz > sizeof(tmp))
    return; /* would overflow tmp; leave unsorted rather than corrupt */

  for (UINTN i = 1; i < n; i++) {
    CopyMem(tmp, map + i * desc_sz, desc_sz);
    UINT64 key = ((EFI_MEMORY_DESCRIPTOR *)tmp)->PhysicalStart;
    INTN j = (INTN)i - 1;
    while (j >= 0 &&
           ((EFI_MEMORY_DESCRIPTOR *)(map + (UINTN)j * desc_sz))->PhysicalStart > key) {
      CopyMem(map + ((UINTN)j + 1) * desc_sz, map + (UINTN)j * desc_sz, desc_sz);
      j--;
    }
    CopyMem(map + ((UINTN)j + 1) * desc_sz, tmp, desc_sz);
  }
}

EFI_STATUS exit_boot_services_retry(
    AppContext *ctx,
    EFI_HANDLE image,
    BootArgsState *state)
{
  EFI_STATUS status;

  status = boot_refresh_memory_map(ctx, state);
  if (EFI_ERROR(status))
    return status;

  boot_update_args_memory_map(ctx, state);

  for (;;) {
    UINTN map_size = state->memory_map_buf.pages << EFI_PAGE_SHIFT;
    UINTN key = 0;
    UINTN desc_size = 0;
    UINT32 desc_ver = 0;

    /*
     * Disable hardware interrupts so the 8254 timer cannot fire between
     * GetMemoryMap capturing the key and ExitBootServices validating it.
     * OVMF DEBUG fires 14+ memory-map-change events inside GetMemoryMap
     * (debug callbacks that AllocatePool), so the key returned is already
     * "post-events". Without CLI the periodic timer interrupt fires
     * between GM return and EBS, incrementing the key one more time and
     * causing perpetual EFI_INVALID_PARAMETER in QEMU TCG where each
     * instruction takes longer in wall-clock time.
     */
    __asm__ volatile ("cli");

    status = uefi_call_wrapper(
        ctx->bs->GetMemoryMap,
        5,
        &map_size,
        state->memory_map_buf.ptr,
        &key,
        &desc_size,
        &desc_ver);

    if (EFI_ERROR(status)) {
      __asm__ volatile ("sti");
      return status;
    }

    /* Update args while interrupts are still off - no UEFI calls. */
    state->memory_map_key = key;
    state->memory_map_size = map_size;
    state->descriptor_size = desc_size;
    state->descriptor_version = desc_ver;
    state->args->MemoryMap = (UINT32)state->memory_map_buf.phys;
    state->args->MemoryMapSize = (UINT32)map_size;
    state->args->MemoryMapDescriptorSize = (UINT32)desc_size;
    state->args->MemoryMapDescriptorVersion = desc_ver;

    status = uefi_call_wrapper(
        ctx->bs->ExitBootServices,
        2,
        image,
        key);

    if (status == EFI_SUCCESS) {
      /*
       * Call SetVirtualAddressMap now that boot services are gone.
       *
       * Assign EFI runtime virtual addresses the way boot.efi does: a single
       * CONTIGUOUS, PACKED range placed just above the kernel image, in
       * ascending physical order.  This is the only scheme that satisfies BOTH
       * constraints imposed by XNU's efi_init (osfmk/i386/AT386/model_dep.c):
       *
       *   - efi_init maps each runtime descriptor with pmap_map_bd(), which
       *     requires the target VA to already have a page-table page and
       *     PANICS ("pmap_map_bd: Invalid kernel address") otherwise.  The
       *     bootstrap kernel pmap only has NKPT (=500) page tables, covering
       *     VA [KERNEL_BASE, KERNEL_BASE + 500*2MB) = up to +0x3E800000.  So a
       *     runtime VA must be BELOW 0x3E800000.  (A per-descriptor
       *     PhysicalStart & 0x3FFFFFFF map fails here: a runtime page at phys
       *     ~0x3eb3f000 lands just past the NKPT edge.)
       *   - efi_init OR-s low VAs into VM_MIN_KERNEL_ADDRESS and maps
       *     VirtualStart -> PhysicalStart in the kernel's own address space, so
       *     the VA must NOT overlap the kernel image [kaddr, kaddr+ksize) or it
       *     clobbers kernel text.
       *
       * The window (kaddr+ksize, 0x3E800000) has page tables (NKPT) with empty
       * PTEs, so packing there is both PT-backed and collision-free.  Pages are
       * not moved physically; only their VA mapping is assigned.
       */
      EFI_RUNTIME_SERVICES *rt = ctx->st->RuntimeServices;
      UINT8 *rmap = (UINT8 *)state->memory_map_buf.ptr;
      UINTN rdesc_sz = state->descriptor_size;
      UINTN rmap_sz  = state->memory_map_size;

      boot_sort_memory_map(rmap, rmap_sz, rdesc_sz);

      /*
       * Pack base = 2MB-aligned first VA above the kernel image, then EXTEND
       * ksize so XNU's physfree (= kaddr + ksize) covers the packed range.
       * XNU only keeps bootstrap page tables for [0, physfree), Idle_PTs_release
       * frees the rest before efi_init runs, so runtime VAs MUST live below
       * physfree.  By growing ksize we make the runtime region "part of the
       * kernel": its PT pages survive, its PTEs are identity-filled by pstart,
       * and efi_init remaps them to the real runtime physical pages.  This is
       * boot.efi's AllocKernelMem-region trick without physically moving pages.
       */
      /*
       * IMPORTANT: state->args->kaddr here is the STAGING host_base (high RAM,
       * ~0x7A8B4000) where segments were parked, NOT where XNU runs.  main.c
       * copies the image to the SLID base 0x100000 + kslide AFTER EBS and sets
       * args->kaddr to that.  XNU derives physfree and its bootstrap page
       * tables from that slid base, so the runtime VA window MUST be computed
       * relative to it, otherwise pmap_map_bd sees VAs with no page table.
       */
      UINT64 kaddr    = 0x100000ULL + ctx->kslide;
      /*
       * Pack runtime VAs above the boot-info block (XNU_RT_VA_BASE), not just
       * above the kernel image: the boot-info block [XNU_BOOTINFO_BASE,
       * XNU_BOOTINFO_END) holds boot_args/DT/EFI-tables/memmap and must not be
       * clobbered by efi_init's runtime PTE writes.
       */
      UINT64 va_base  = XNU_RT_VA_BASE;

      UINT64 va_cursor   = va_base;
      UINT32 rmin_virt_pg = 0xFFFFFFFFU;
      for (UINTN roff = 0; roff + rdesc_sz <= rmap_sz; roff += rdesc_sz) {
        EFI_MEMORY_DESCRIPTOR *d = (EFI_MEMORY_DESCRIPTOR *)(rmap + roff);
        if (!(d->Attribute & EFI_MEMORY_RUNTIME))
          continue;

        BOOLEAN pack = (d->Type == EfiRuntimeServicesCode ||
                d->Type == EfiRuntimeServicesData ||
                d->Type == EfiMemoryMappedIO ||
                d->Type == EfiMemoryMappedIOPortSpace);

        if (pack) {
          UINT64 sz = (UINT64)d->NumberOfPages << EFI_PAGE_SHIFT;
          d->VirtualStart = va_cursor;
          va_cursor += sz;
        } else {
          /* Identity: VA == PA. efi_init OR-s VM_MIN_KERNEL_ADDRESS on
          * any VA below it, landing safely within the NKPT window. */
          d->VirtualStart = d->PhysicalStart;
        }

        UINT32 vpg = (UINT32)(d->VirtualStart >> EFI_PAGE_SHIFT);
        if (vpg < rmin_virt_pg) rmin_virt_pg = vpg;
      }

      /* Grow ksize so physfree covers the packed runtime region (2MB-rounded). */
      UINT64 new_kend = (va_cursor + 0x1FFFFFULL) & ~0x1FFFFFULL;
      state->args->ksize = (UINT32)(new_kend - kaddr);

      rt->SetVirtualAddressMap(rmap_sz, rdesc_sz,
                               state->descriptor_version,
                               (EFI_MEMORY_DESCRIPTOR *)rmap);

      /* Update boot_args: virtual page start only.
       * efiSystemTable was already set to the conventional-memory copy
       * (tbl_phys) before EBS; that address is in XNU's physmap and
       * needs no adjustment after SVAM. */
      if (state->args) {
        if (rmin_virt_pg != 0xFFFFFFFFU)
          state->args->efiRuntimeServicesVirtualPageStart = rmin_virt_pg;
      }

      return EFI_SUCCESS;
    }

    __asm__ volatile ("sti");

    if (status != EFI_INVALID_PARAMETER)
      return status;
  }
}