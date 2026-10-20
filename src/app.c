#include "app.h"
#include "console.h"
#include "serial.h"
#include "platform.h"

#if defined(__aarch64__) || defined(__riscv)
#include "fileio.h"

/// When a dtb file is found here it, installed via `InstallConfigurationTable`
#define ESP_DTB_PATH L"\\EFI\\BOOT\\board.dtb"

/// the dtb header value (4 bytes) at `p`
/// offset 0: magic (0xd00dfeed), 4: size
static UINT32 dtb_be32(CONST UINT8 *p) {
  return ((UINT32)p[0] << 24) | ((UINT32)p[1] << 16) | ((UINT32)p[2] << 8) | p[3];
}

/// Load and install a dtb file when present at `ESP_DTB_PATH`
static EFI_STATUS load_esp_dtb(AppContext *ctx, UINTN *out_size) {
  static EFI_GUID dtb_guid = { 0xb1b621d5, 0xf19c, 0x41a5,
                               { 0x83, 0x0b, 0xd9, 0x15, 0x2c, 0x69, 0xaa, 0xe0 } };
  EFI_FILE_PROTOCOL *root = NULL;
  FileBuffer file = {0};
  EFI_PHYSICAL_ADDRESS phys = 0;
  EFI_STATUS status;

  status = app_open_self_volume(ctx, &root);
  if (EFI_ERROR(status))
    return status;
  status = file_read_all(ctx, root, ESP_DTB_PATH, &file);
  uefi_call_wrapper(root->Close, 1, root);
  if (EFI_ERROR(status))
    return status;

  CONST UINT8 *h = (CONST UINT8 *)file.data;
  UINTN size = file.size >= 8 ? dtb_be32(h + 4) : 0;
  if (file.size < 40 || dtb_be32(h) != 0xd00dfeedU || size < 40 || size > file.size) {
    file_free(ctx, &file);
    return EFI_COMPROMISED_DATA;
  }

  // the spec's home for a device tree table, the firmware keeps it out of loader allocations
  status = uefi_call_wrapper(ctx->bs->AllocatePages, 4, AllocateAnyPages, EfiACPIReclaimMemory,
                             EFI_SIZE_TO_PAGES(size), &phys);
  if (!EFI_ERROR(status))
    CopyMem((VOID *)(UINTN)phys, file.data, size);
  file_free(ctx, &file);
  if (EFI_ERROR(status))
    return status;

  // replace/add firmware dtb entry
  status = uefi_call_wrapper(ctx->bs->InstallConfigurationTable, 2, &dtb_guid, (VOID *)(UINTN)phys);
  if (EFI_ERROR(status)) {
    uefi_call_wrapper(ctx->bs->FreePages, 2, phys, EFI_SIZE_TO_PAGES(size));
    return status;
  }
  *out_size = size;
  return EFI_SUCCESS;
}
#endif

EFI_STATUS app_init(AppContext *ctx, EFI_HANDLE image, EFI_SYSTEM_TABLE *st) {
  if (!st)
    return EFI_INVALID_PARAMETER;

  if (!ctx || st->Hdr.Signature != EFI_SYSTEM_TABLE_SIGNATURE)
    return EFI_ABORTED;

  // ensure there's no leftover data. this caused a bug on some machines
  // to falsely report having a RAMDisk when it did not.
  SetMem(ctx, sizeof(*ctx), 0);

  ctx->image_handle = image;
  ctx->st = st;
  ctx->bs = st->BootServices;
  ctx->rt = st->RuntimeServices;

  InitializeLib(image, st);

#if defined(__aarch64__) || defined(__riscv)
  // load/install dtb from ESP_DTB_PATH before calling fdt_board_parse. logged after serial_init
  UINTN esp_dtb_size = 0;
  EFI_STATUS esp_dtb_status = load_esp_dtb(ctx, &esp_dtb_size);
#endif

#if defined(__aarch64__) && defined(XNU_LOADER_PLATFORM_GENERIC)
  // the console and the rest of the board come from the firmware's device tree, else the build's defaults
  fdt_board_parse(fdt_board_find(st));
#endif

  /* Bring up the serial console before any logging so serial captures the full
   * boot on real hardware (where there is no EFI console to read). */
  serial_init();

#if defined(__aarch64__) || defined(__riscv)
  if (!EFI_ERROR(esp_dtb_status))
    log_info(L"Used board.dtb: %lu bytes\r\n", (UINT64)esp_dtb_size);
#endif

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

static UINT64 app_detect_memory(AppContext *ctx, UINT64 base, BOOLEAN extent) {
  UINTN map_size = 0, key = 0, desc_size = 0;
  UINT32 desc_ver = 0;
  EFI_MEMORY_DESCRIPTOR *mm = NULL;
  UINT64 total = 0, end = base;

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
      UINT64 bytes = d->NumberOfPages << EFI_PAGE_SHIFT;
      total += bytes;
      if (d->PhysicalStart >= base && bytes <= ~0ULL - d->PhysicalStart &&
          d->PhysicalStart + bytes > end)
        end = d->PhysicalStart + bytes;
    }
  }

  uefi_call_wrapper(ctx->bs->FreePool, 1, mm);
  return extent ? end - base : total;
}

UINT64 app_detect_physical_memory_size(AppContext *ctx) {
  return app_detect_memory(ctx, 0, FALSE);
}

UINT64 app_detect_physical_memory_extent(AppContext *ctx, UINT64 base) {
  return app_detect_memory(ctx, base, TRUE);
}
