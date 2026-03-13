#include "app.h"
#include "console.h"

EFI_STATUS app_init(AppContext *ctx, EFI_HANDLE image, EFI_SYSTEM_TABLE *st) {
  if (!ctx || !st || st->Hdr.Signature != EFI_SYSTEM_TABLE_SIGNATURE)
    return EFI_INVALID_PARAMETER;

  ctx->image_handle = image;
  ctx->st = st;
  ctx->bs = st->BootServices;
  ctx->rt = st->RuntimeServices;

  InitializeLib(image, st);

  if (!st->BootServices) {
    log_info(L"No BootServices -> not real UEFI runtime\n");
    return EFI_INVALID_PARAMETER;
  }

  log_info(L"UEFI revision: %d.%02d\n", st->Hdr.Revision >> 16, st->Hdr.Revision & 0xffff);

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