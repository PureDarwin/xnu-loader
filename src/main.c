#include "common.h"
#include "app.h"
#include "fileio.h"
#include "macho.h"
#include "console.h"

EFI_STATUS efi_main(EFI_HANDLE image, EFI_SYSTEM_TABLE *st) {
  AppContext ctx;
  EFI_FILE_PROTOCOL *root = NULL;
  FileBuffer kernel = {0};
  MachoImage image_info;
  EFI_STATUS status;

  status = app_init(&ctx, image, st);
  if (EFI_ERROR(status))
    return status;

  log_info(L"XNU EFI loader start\n");

  status = app_open_self_volume(&ctx, &root);
  if (EFI_ERROR(status)) {
    log_error(L"failed to open boot volume: %r\n", status);
    return status;
  }

  status = file_read_all(&ctx, root, L"\\EFI\\BOOT\\kernel", &kernel);
  if (EFI_ERROR(status)) {
    log_error(L"failed to read kernel: %r\n", status);
    return status;
  }

  log_info(L"kernel size: %lu bytes\n", kernel.size);

  status = macho_parse(kernel.data, kernel.size, &image_info);
  if (EFI_ERROR(status)) {
    log_error(L"failed to parse Mach-O: %r\n", status);
    file_free(&ctx, &kernel);
    return status;
  }

  status = macho_dump(&image_info);
  if (EFI_ERROR(status))
    log_error(L"failed to dump Mach-O: %r\n", status);

  file_free(&ctx, &kernel);
  return status;
}