#include "app.h"
#include "console.h"

EFI_STATUS app_init(AppContext *ctx, EFI_HANDLE image, EFI_SYSTEM_TABLE *st) {
  if (!st)
    return EFI_INVALID_PARAMETER;

  if (!ctx || st->Hdr.Signature != EFI_SYSTEM_TABLE_SIGNATURE)
    return EFI_ABORTED;

  ctx->image_handle = image;
  ctx->st = st;
  ctx->bs = st->BootServices;
  ctx->rt = st->RuntimeServices;

  InitializeLib(image, st);

  return EFI_SUCCESS;
}

EFI_STATUS app_get_loaded_image(AppContext *ctx, EFI_LOADED_IMAGE **loaded_image) {
  return uefi_call_wrapper(
      ctx->bs->HandleProtocol,
      3,
      ctx->image_handle,
      &LoadedImageProtocol,
      (VOID **)loaded_image);
}

EFI_STATUS app_open_self_volume(AppContext *ctx, EFI_FILE_PROTOCOL **root) {
  EFI_STATUS status;
  EFI_LOADED_IMAGE *loaded_image = NULL;
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs = NULL;

  status = app_get_loaded_image(ctx, &loaded_image);
  if (EFI_ERROR(status))
    return status;

  status = uefi_call_wrapper(
      ctx->bs->HandleProtocol,
      3,
      loaded_image->DeviceHandle,
      &gEfiSimpleFileSystemProtocolGuid,
      (VOID **)&fs);
  if (EFI_ERROR(status))
    return status;

  return uefi_call_wrapper(fs->OpenVolume, 2, fs, root);
}

EFI_STATUS app_alloc_pool(AppContext *ctx, UINTN size, VOID **ptr) {
  return uefi_call_wrapper(
      ctx->bs->AllocatePool,
      3,
      EfiLoaderData,
      size,
      ptr);
}

VOID app_free_pool(AppContext *ctx, VOID *ptr) {
  if (ptr)
    uefi_call_wrapper(ctx->bs->FreePool, 1, ptr);
}

uint64_t app_detect_physical_memory_size(AppContext *ctx) {
  if (!ctx || !ctx->bs)
    return 0;

  EFI_STATUS status;
  UINTN map_size = 0;
  UINTN map_key = 0;
  UINTN desc_size = 0;
  UINT32 desc_ver = 0;

  // query required buffer size
  status = uefi_call_wrapper(
    ctx->bs->GetMemoryMap,
    5,
    &map_size,
    NULL,
    &map_key,
    &desc_size,
    &desc_ver
  );

  if (status != EFI_BUFFER_TOO_SMALL)
    return 0;

  EFI_MEMORY_DESCRIPTOR *map = NULL;

  status = uefi_call_wrapper(
    ctx->bs->AllocatePool,
    3,
    EfiLoaderData,
    map_size,
    (void **)&map
  );

  if (EFI_ERROR(status))
    return 0;

  // actually fetch memory map
  status = uefi_call_wrapper(
    ctx->bs->GetMemoryMap,
    5,
    &map_size,
    map,
    &map_key,
    &desc_size,
    &desc_ver
  );

  if (EFI_ERROR(status)) {
    uefi_call_wrapper(ctx->bs->FreePool, 1, map);
    return 0;
  }

  uint64_t total = 0;
  for (UINTN off = 0; off < map_size; off += desc_size) {
    EFI_MEMORY_DESCRIPTOR *d = (EFI_MEMORY_DESCRIPTOR *)((UINT8 *)map + off);

    if (d->Type == EfiConventionalMemory) {
      total += (uint64_t)d->NumberOfPages * 4096ULL;
    }
  }

  uefi_call_wrapper(ctx->bs->FreePool, 1, map);

  return total;
}