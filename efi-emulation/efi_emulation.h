#ifndef XNU_EFI_EMULATION_H
#define XNU_EFI_EMULATION_H

#include <efi.h>

/* Everything a boot protocol hands us, normalised so the EFI shim does not
 * care whether it came from Multiboot2, the BIOS stages or (later) a DTB. */

#define EFIEMU_MAX_MEMORY_RANGES 128
#define EFIEMU_MAX_MODULES 16

typedef enum {
  EfiEmuMemoryUsable = 1,
  EfiEmuMemoryReserved = 2,
  EfiEmuMemoryAcpiReclaim = 3,
  EfiEmuMemoryAcpiNvs = 4,
  EfiEmuMemoryBad = 5,
} EfiEmuMemoryType;

typedef struct {
  UINT64 base;
  UINT64 length;
  UINT32 type; /* EfiEmuMemoryType, E820 numbering */
} EfiEmuMemoryRange;

typedef struct {
  UINT64 base;
  UINT32 width;
  UINT32 height;
  UINT32 pixels_per_scanline;
  UINT8 bits_per_pixel;
  UINT8 red_position;
  UINT8 blue_position;
  UINT8 valid;
} EfiEmuFramebuffer;

typedef struct {
  UINT64 start;
  UINT64 size;
  CONST CHAR8 *name; /* path the loader opens, e.g. "/EFI/BOOT/kernel" */
} EfiEmuModule;

typedef struct {
  EfiEmuMemoryRange memory[EFIEMU_MAX_MEMORY_RANGES];
  UINT32 memory_count;
  EfiEmuFramebuffer framebuffer;
  /* RSDP supplied by the boot protocol (copied in place), or NULL to scan. */
  VOID *rsdp;
  UINT32 rsdp_length;
  CONST CHAR8 *cmdline;
  EfiEmuModule modules[EFIEMU_MAX_MODULES];
  UINT32 module_count;
  /* The real firmware's EFI_SYSTEM_TABLE when booted from UEFI (after
   * ExitBootServices): its configuration tables still locate ACPI/SMBIOS. */
  UINT64 efi_system_table;
  /* Boot-protocol data to keep out of the allocator (e.g. the MBI). */
  UINT64 protocol_data_base;
  UINT64 protocol_data_size;
} EfiEmuBootInfo;

void efiemu_main(EfiEmuBootInfo *info) __attribute__((noreturn));

/* Module-backed volume (preferred) and the FAT32/ATA disk fallback. */
EFI_STATUS efiemu_modfs_init(EfiEmuBootInfo *info);
EFI_STATUS efiemu_modfs_protocol(EFI_GUID *guid, VOID **out);
EFI_HANDLE efiemu_modfs_handle(void);

/* Reads count sectors (<= EFIEMU_BIOS_SECTORS) from the BIOS boot drive into
 * EFIEMU_BIOS_BOUNCE; returns 0 or the INT 13h status. */
#define EFIEMU_BIOS_BOUNCE 0x18000UL
#define EFIEMU_BIOS_SECTORS 64U
typedef UINT32 (__attribute__((sysv_abi)) *EfiEmuBiosRead)(UINT64 lba, UINT32 count);
void efiemu_bios_disk_set(UINT32 drive, EfiEmuBiosRead read);
EFI_STATUS efiemu_disk_init(void);
EFI_STATUS efiemu_disk_protocol(EFI_GUID *guid, VOID **out);
EFI_HANDLE efiemu_disk_handle(void);

void efiemu_exceptions_install(void);
void efiemu_debug_string(const char *s);
void efiemu_debug_hex(UINT64 value);

/* Called by src/boot.c (LEGACY_BIOS) after SetVirtualAddressMap. */
void legacy_runtime_fixup(EFI_RUNTIME_SERVICES *runtime_copy);

#endif
