#ifndef COMMON_H
#define COMMON_H

#include <efi.h>
#include <efilib.h>

typedef struct AppContext {
  EFI_HANDLE image_handle;
  EFI_SYSTEM_TABLE *st;
  EFI_BOOT_SERVICES *bs;
  EFI_RUNTIME_SERVICES *rt;
} AppContext;

typedef struct FileBuffer {
  VOID *data;
  UINTN size;
} FileBuffer;

#endif