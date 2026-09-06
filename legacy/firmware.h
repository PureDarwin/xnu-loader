#ifndef XNU_LEGACY_FIRMWARE_H
#define XNU_LEGACY_FIRMWARE_H

#include <efi.h>

typedef struct __attribute__((packed)) {
  UINT64 base;
  UINT64 length;
  UINT32 type;
  UINT32 attributes;
} LegacyE820Entry;

typedef struct __attribute__((packed)) {
  UINT64 base;
  UINT32 width;
  UINT32 height;
  UINT32 pixels_per_scanline;
  UINT8 bits_per_pixel;
  UINT8 red_position;
  UINT8 blue_position;
  UINT8 valid;
} LegacyFramebuffer;

void legacy_firmware_main(LegacyE820Entry *map, UINT32 count,
                          UINT32 boot_drive, LegacyFramebuffer *framebuffer);
EFI_STATUS legacy_storage_init(UINT32 boot_drive);
EFI_STATUS legacy_storage_protocol(EFI_GUID *guid, VOID **out);
EFI_HANDLE legacy_storage_handle(void);
void legacy_runtime_fixup(EFI_RUNTIME_SERVICES *runtime_copy);

#endif
