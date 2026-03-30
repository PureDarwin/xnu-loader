#ifndef FILEIO_H
#define FILEIO_H

#include "common.h"
#include "app.h"

EFI_STATUS file_open(
    EFI_FILE_PROTOCOL *root,
    CONST CHAR16 *path,
    UINT64 mode,
    UINT64 attrs,
    EFI_FILE_PROTOCOL **out_file);

EFI_STATUS file_get_size(
    EFI_FILE_PROTOCOL *file,
    UINTN *out_size);

EFI_STATUS file_read_all(
    AppContext *ctx,
    EFI_FILE_PROTOCOL *root,
    CONST CHAR16 *path,
    FileBuffer *out_buf);

VOID file_free(
    AppContext *ctx,
    FileBuffer *buf);

EFI_STATUS file_read_all_from_any_volume(
    AppContext *ctx,
    CONST CHAR16 *path,
    FileBuffer *out_buf,
    EFI_HANDLE *out_handle);

#endif