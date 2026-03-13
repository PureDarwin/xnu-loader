#include "fileio.h"
#include "app.h"

EFI_STATUS file_open(
    EFI_FILE_PROTOCOL *root,
    CONST CHAR16 *path,
    UINT64 mode,
    UINT64 attrs,
    EFI_FILE_PROTOCOL **out_file) {
  return uefi_call_wrapper(root->Open, 5, root, out_file, (CHAR16 *)path, mode, attrs);
}

EFI_STATUS file_get_size(EFI_FILE_PROTOCOL *file, UINTN *out_size) {
  EFI_STATUS status;
  UINTN info_size = SIZE_OF_EFI_FILE_INFO + 256;
  EFI_FILE_INFO *info = NULL;

  status = uefi_call_wrapper(BS->AllocatePool, 3, EfiLoaderData, info_size, (VOID **)&info);
  if (EFI_ERROR(status))
    return status;

  status = uefi_call_wrapper(
      file->GetInfo,
      4,
      file,
      &gEfiFileInfoGuid,
      &info_size,
      info);
  if (!EFI_ERROR(status))
    *out_size = (UINTN)info->FileSize;

  uefi_call_wrapper(BS->FreePool, 1, info);
  return status;
}

EFI_STATUS file_read_all(
    AppContext *ctx,
    EFI_FILE_PROTOCOL *root,
    CONST CHAR16 *path,
    FileBuffer *out_buf) {
  EFI_STATUS status;
  EFI_FILE_PROTOCOL *file = NULL;
  UINTN size = 0;
  VOID *buffer = NULL;
  UINTN read_size;

  if (!out_buf)
    return EFI_INVALID_PARAMETER;

  out_buf->data = NULL;
  out_buf->size = 0;

  status = file_open(root, path, EFI_FILE_MODE_READ, 0, &file);
  if (EFI_ERROR(status))
    return status;

  status = file_get_size(file, &size);
  if (EFI_ERROR(status))
    goto done;

  status = app_alloc_pool(ctx, size, &buffer);
  if (EFI_ERROR(status))
    goto done;

  read_size = size;
  status = uefi_call_wrapper(file->Read, 3, file, &read_size, buffer);
  if (EFI_ERROR(status))
    goto done;

  out_buf->data = buffer;
  out_buf->size = read_size;
  buffer = NULL;

done:
  if (buffer)
    app_free_pool(ctx, buffer);
  if (file)
    uefi_call_wrapper(file->Close, 1, file);
  return status;
}

VOID file_free(AppContext *ctx, FileBuffer *buf) {
  if (!buf)
    return;
  if (buf->data)
    app_free_pool(ctx, buf->data);
  buf->data = NULL;
  buf->size = 0;
}