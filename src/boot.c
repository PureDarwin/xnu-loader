#include "boot.h"
#include "console.h"
#include "devtree.h"
#include <efiprot.h>

static UINT64 align_up_u64(UINT64 v, UINT64 a) {
  return (v + (a - 1)) & ~(a - 1);
}

static UINTN boot_ascii_len(const CHAR8 *s) {
  UINTN n = 0;
  if (!s)
    return 0;
  while (s[n] != '\0')
    n++;
  return n;
}

static VOID boot_fill_physical_memory_size(BootArgsState *state) {
  if (!state || !state->args)
    return;

  /* TEMP: hardcode until memmap is trusted */
  state->args->PhysicalMemorySize = 512ULL * 1024 * 1024; // 512MB
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

VOID boot_update_args_memory_map(BootArgsState *state) {
  if (!state || !state->args)
    return;

  state->args->MemoryMap = (UINT32)(UINTN)state->memory_map;
  state->args->MemoryMapSize = (UINT32)state->memory_map_size;
  state->args->MemoryMapDescriptorSize = (UINT32)state->descriptor_size;
  state->args->MemoryMapDescriptorVersion = state->descriptor_version;

  boot_fill_physical_memory_size(state);
}

EFI_STATUS boot_set_command_line(
    AppContext *ctx,
    BootArgsState *state,
    const CHAR8 *cmdline)
{
  EFI_STATUS status;

  if (!ctx || !state || !state->args || !cmdline)
    return EFI_INVALID_PARAMETER;

  SetMem(state->args->CommandLine, BOOT_LINE_LENGTH, 0);

  {
    UINTN len = boot_ascii_len(cmdline);
    if (len >= BOOT_LINE_LENGTH)
      len = BOOT_LINE_LENGTH - 1;
    CopyMem(state->args->CommandLine, cmdline, len);
    state->args->CommandLine[len] = '\0';
  }

  if (state->device_tree_buf.ptr != NULL) {
    lowmem_free(ctx, &state->device_tree_buf);
    state->device_tree = NULL;
    state->device_tree_size = 0;
  }

  status = dt_build_minimal(
      ctx,
      &state->device_tree,
      &state->device_tree_size,
      &state->device_tree_buf,
      state->args->CommandLine);
  if (EFI_ERROR(status))
    return status;

  state->args->deviceTreeP = (UINT32)(UINTN)state->device_tree;
  state->args->deviceTreeLength = state->device_tree_size;

  return EFI_SUCCESS;
}

EFI_STATUS boot_build_args(
    AppContext *ctx,
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

  status = lowmem_alloc_pages(
      ctx,
      sizeof(boot_args),
      EfiLoaderData,
      &args_buf);
  if (EFI_ERROR(status))
    return status;

  args = (boot_args *)args_buf.ptr;
  SetMem(args, sizeof(boot_args), 0);

  if (state->device_tree == NULL) {
    status = dt_build_minimal(
        ctx,
        &state->device_tree,
        &state->device_tree_size,
        &state->device_tree_buf,
        args->CommandLine);
    if (EFI_ERROR(status)) {
      lowmem_free(ctx, &args_buf);
      return status;
    }
  }

  args->Revision = 0;
  args->Version  = 2;

  args->efiMode   = 64;
  args->debugMode = 0;
  args->flags     = 0;

  args->MemoryMap = (UINT32)(UINTN)state->memory_map;
  args->MemoryMapSize = (UINT32)state->memory_map_size;
  args->MemoryMapDescriptorSize = (UINT32)state->descriptor_size;
  args->MemoryMapDescriptorVersion = state->descriptor_version;

  args->deviceTreeP = (UINT32)(UINTN)state->device_tree;
  args->deviceTreeLength = state->device_tree_size;

  args->kaddr = (UINT32)load_result->host_base;
  args->ksize = (UINT32)align_up_u64(load_result->image_size, EFI_PAGE_SIZE);

  args->efiSystemTable = (UINT32)(UINTN)ctx->st;

  args->KC_hdrs_vaddr = 0;
  args->kslide = 0;

  status = boot_fill_video(ctx, args);
  if (EFI_ERROR(status)) {
    lowmem_free(ctx, &args_buf);
    return status;
  }

  state->args = args;
  state->args_buf = args_buf;

  boot_fill_physical_memory_size(state);

  log_info(L"sizeof(boot_args) = 0x%lx\r\n", (UINT64)sizeof(boot_args));
  log_info(L"boot_args low phys = 0x%lx\r\n", (UINT64)state->args_buf.phys);
  log_info(L"device_tree low phys = 0x%lx\r\n", (UINT64)state->device_tree_buf.phys);
  log_info(L"memory_map low phys = 0x%lx\r\n", (UINT64)state->memory_map_buf.phys);

  return EFI_SUCCESS;
}

EFI_STATUS boot_fill_video(
    AppContext *ctx,
    boot_args *args)
{
  EFI_STATUS status;
  EFI_GRAPHICS_OUTPUT_PROTOCOL *gop;

  if (!ctx || !args)
    return EFI_INVALID_PARAMETER;

  status = uefi_call_wrapper(
      ctx->bs->LocateProtocol,
      3,
      &gEfiGraphicsOutputProtocolGuid,
      NULL,
      (VOID **)&gop);

  if (EFI_ERROR(status))
    return status;

  args->Video.v_display  = GRAPHICS_MODE;
  args->Video.v_rowBytes = gop->Mode->Info->PixelsPerScanLine * 4;
  args->Video.v_width    = gop->Mode->Info->HorizontalResolution;
  args->Video.v_height   = gop->Mode->Info->VerticalResolution;
  args->Video.v_depth    = 32;
  args->Video.v_rotate   = 0;
  args->Video.v_baseAddr = gop->Mode->FrameBufferBase;

  args->VideoV1.v_baseAddr = (UINT32)gop->Mode->FrameBufferBase;
  args->VideoV1.v_display  = GRAPHICS_MODE;
  args->VideoV1.v_rowBytes = args->Video.v_rowBytes;
  args->VideoV1.v_width    = args->Video.v_width;
  args->VideoV1.v_height   = args->Video.v_height;
  args->VideoV1.v_depth    = args->Video.v_depth;

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

EFI_STATUS exit_boot_services_retry(
    AppContext *ctx,
    EFI_HANDLE image,
    BootArgsState *state)
{
  EFI_STATUS status;

  for (;;) {
    status = boot_refresh_memory_map(ctx, state);
    if (EFI_ERROR(status))
      return status;

    boot_update_args_memory_map(state);

    status = uefi_call_wrapper(
        ctx->bs->ExitBootServices,
        2,
        image,
        state->memory_map_key);

    if (status == EFI_SUCCESS)
      return EFI_SUCCESS;

    if (status != EFI_INVALID_PARAMETER)
      return status;

    /* retry: memory map changed */
  }
}