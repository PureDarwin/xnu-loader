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

UINT64 app_detect_physical_memory_size(AppContext *ctx) {
  UINTN map_size = 0, key = 0, desc_size = 0;
  UINT32 desc_ver = 0;
  EFI_MEMORY_DESCRIPTOR *mm = NULL;
  UINT64 total = 0;

  uefi_call_wrapper(ctx->bs->GetMemoryMap, 5,
      &map_size, mm, &key, &desc_size, &desc_ver);
  map_size += desc_size * 4;

  EFI_STATUS s = uefi_call_wrapper(ctx->bs->AllocatePool, 3,
      EfiLoaderData, map_size, (VOID **)&mm);
  if (EFI_ERROR(s) || !mm)
    return 0;

  s = uefi_call_wrapper(ctx->bs->GetMemoryMap, 5,
      &map_size, mm, &key, &desc_size, &desc_ver);
  if (EFI_ERROR(s)) {
    uefi_call_wrapper(ctx->bs->FreePool, 1, mm);
    return 0;
  }

  UINTN n = map_size / desc_size;
  for (UINTN i = 0; i < n; i++) {
    EFI_MEMORY_DESCRIPTOR *d =
        (EFI_MEMORY_DESCRIPTOR *)((UINT8 *)mm + i * desc_size);
    if (d->Type == EfiLoaderCode         ||
        d->Type == EfiLoaderData         ||
        d->Type == EfiBootServicesCode   ||
        d->Type == EfiBootServicesData   ||
        d->Type == EfiRuntimeServicesCode ||
        d->Type == EfiRuntimeServicesData ||
        d->Type == EfiConventionalMemory ||
        d->Type == EfiACPIReclaimMemory  ||
        d->Type == EfiACPIMemoryNVS      ||
        d->Type == EfiPalCode) {
      total += d->NumberOfPages << EFI_PAGE_SHIFT;
    }
  }

  uefi_call_wrapper(ctx->bs->FreePool, 1, mm);
  return total;
}