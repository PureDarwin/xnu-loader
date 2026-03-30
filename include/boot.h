#ifndef BOOT_H
#define BOOT_H

#include "common.h"
#include "macho.h"
#include "lowmem.h"

#define BOOT_LINE_LENGTH        1024
#define BOOT_STRING_LEN         BOOT_LINE_LENGTH

struct Boot_VideoV1 {
  uint32_t        v_baseAddr;
  uint32_t        v_display;
  uint32_t        v_rowBytes;
  uint32_t        v_width;
  uint32_t        v_height;
  uint32_t        v_depth;
};
typedef struct Boot_VideoV1 Boot_VideoV1;

struct Boot_Video {
  uint32_t        v_display;
  uint32_t        v_rowBytes;
  uint32_t        v_width;
  uint32_t        v_height;
  uint32_t        v_depth;
  uint8_t         v_rotate;
  uint8_t         v_resv_byte[3];
  uint32_t        v_resv[6];
  uint64_t        v_baseAddr;
};
typedef struct Boot_Video Boot_Video;

#define GRAPHICS_MODE 1
#define FB_TEXT_MODE  2

typedef struct boot_args {
  uint16_t Revision;
  uint16_t Version;

  uint8_t  efiMode;
  uint8_t  debugMode;
  uint16_t flags;

  char CommandLine[BOOT_LINE_LENGTH];

  uint32_t MemoryMap;
  uint32_t MemoryMapSize;
  uint32_t MemoryMapDescriptorSize;
  uint32_t MemoryMapDescriptorVersion;

  Boot_VideoV1 VideoV1;

  uint32_t deviceTreeP;
  uint32_t deviceTreeLength;

  uint32_t kaddr;
  uint32_t ksize;

  uint32_t efiRuntimeServicesPageStart;
  uint32_t efiRuntimeServicesPageCount;
  uint64_t efiRuntimeServicesVirtualPageStart;

  uint32_t efiSystemTable;
  uint32_t kslide;

  uint32_t performanceDataStart;
  uint32_t performanceDataSize;

  uint32_t keyStoreDataStart;
  uint32_t keyStoreDataSize;

  uint64_t bootMemStart;
  uint64_t bootMemSize;
  uint64_t PhysicalMemorySize;

  uint64_t FSBFrequency;
  uint64_t pciConfigSpaceBaseAddress;

  uint32_t pciConfigSpaceStartBusNumber;
  uint32_t pciConfigSpaceEndBusNumber;

  uint32_t csrActiveConfig;
  uint32_t csrCapabilities;
  uint32_t boot_SMC_plimit;

  uint16_t bootProgressMeterStart;
  uint16_t bootProgressMeterEnd;

  Boot_Video Video;

  uint32_t apfsDataStart;
  uint32_t apfsDataSize;

  uint64_t KC_hdrs_vaddr;

  uint64_t arvRootHashStart;
  uint64_t arvRootHashSize;

  uint64_t arvManifestStart;
  uint64_t arvManifestSize;

  uint64_t bsARVRootHashStart;
  uint64_t bsARVRootHashSize;

  uint64_t bsARVManifestStart;
  uint64_t bsARVManifestSize;

  uint32_t __reserved4[692];
} boot_args;

typedef struct BootArgsState {
  boot_args *args;
  LowMemBuffer args_buf;

  VOID *device_tree;
  UINT32 device_tree_size;
  LowMemBuffer device_tree_buf;

  EFI_MEMORY_DESCRIPTOR *memory_map;
  UINTN memory_map_size;
  UINTN memory_map_key;
  UINTN descriptor_size;
  UINT32 descriptor_version;
  LowMemBuffer memory_map_buf;
} BootArgsState;

EFI_STATUS boot_collect_memory_map(
    AppContext *ctx,
    BootArgsState *state);

EFI_STATUS boot_refresh_memory_map(
    AppContext *ctx,
    BootArgsState *state);

VOID boot_update_args_memory_map(BootArgsState *state);

EFI_STATUS boot_set_command_line(
    AppContext *ctx,
    BootArgsState *state,
    const CHAR8 *cmdline);

EFI_STATUS boot_build_args(
    AppContext *ctx,
    MachoLoadResult *load_result,
    BootArgsState *state);

EFI_STATUS boot_fill_video(
    AppContext *ctx,
    boot_args *args);

VOID boot_free_args(
    AppContext *ctx,
    BootArgsState *state);

VOID boot_log_args(BootArgsState *state);

EFI_STATUS exit_boot_services_retry(AppContext *ctx, EFI_HANDLE image, BootArgsState *state);

#endif