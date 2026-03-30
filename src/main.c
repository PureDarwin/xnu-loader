#include "common.h"
#include "app.h"
#include "boot.h"
#include "console.h"
#include "fileio.h"
#include "macho.h"
#include "jump.h"

EFI_STATUS efi_main(EFI_HANDLE image, EFI_SYSTEM_TABLE *st) {
  AppContext ctx;
  FileBuffer kernel = {0};
  MachoImage image_info;
  MachoLoadResult load_result = {0};
  EFI_STATUS status;

  status = app_init(&ctx, image, st);
  if (EFI_ERROR(status))
    return status;

  log_info(L"XNU EFI loader start\r\n");

  EFI_HANDLE found_handle = NULL;

  status = file_read_all_from_any_volume(
      &ctx,
      L"\\EFI\\BOOT\\kernel",
      &kernel,
      &found_handle);

  if (EFI_ERROR(status)) {
    log_error(L"failed to read kernel from any volume: %r\r\n", status);
    return status;
  }

  log_info(L"kernel size: %lu bytes\r\n", kernel.size);

  status = macho_parse(kernel.data, kernel.size, &image_info);
  if (EFI_ERROR(status)) {
    log_error(L"failed to parse Mach-O: %r\r\n", status);
    file_free(&ctx, &kernel);
    return status;
  }

  status = macho_dump(&image_info);
  if (EFI_ERROR(status)) {
    log_error(L"failed to dump Mach-O: %r\r\n", status);
    file_free(&ctx, &kernel);
    return status;
  }

  status = macho_load_segments(&ctx, &image_info, &load_result);
  if (EFI_ERROR(status)) {
    log_error(L"failed to load segments: %r\r\n", status);
    file_free(&ctx, &kernel);
    return status;
  }

  log_info(
      L"staged image host=0x%lx vm_low=0x%lx span=0x%lx segments=%u\r\n",
      (UINT64)load_result.host_base,
      load_result.lowest_vmaddr,
      load_result.image_size,
      load_result.segment_count);

  UINT64 entry_vmaddr = 0;
  VOID *host_entry = NULL;

  status = macho_find_entry_vmaddr(&image_info, &entry_vmaddr);
  if (EFI_ERROR(status)) {
    log_error(L"failed to find entry vmaddr: %r\r\n", status);
    macho_unload_segments(&ctx, &load_result);
    file_free(&ctx, &kernel);
    return status;
  }

  status = macho_compute_host_entry(&load_result, entry_vmaddr, &host_entry);
  if (EFI_ERROR(status)) {
    log_error(L"failed to compute host entry: %r\r\n", status);
    macho_unload_segments(&ctx, &load_result);
    file_free(&ctx, &kernel);
    return status;
  }

  macho_log_entry_context(&image_info, entry_vmaddr);
  macho_log_section_host_info(&image_info, &load_result, "__HIB", "__bootPT");
  macho_dump_entry_bytes(host_entry, 32);

  log_info(L"entry vm=0x%lx -> host=0x%lx\r\n", entry_vmaddr, (UINT64)(UINTN)host_entry);

  BootArgsState boot_state = {0};

  status = boot_collect_memory_map(&ctx, &boot_state);
  if (EFI_ERROR(status)) {
    log_error(L"failed to collect memory map: %r\r\n", status);
    return status;
  }

  status = boot_build_args(&ctx, &load_result, &boot_state);
  if (EFI_ERROR(status)) {
    log_error(L"failed to build boot_args: %r\r\n", status);
    return status;
  }

  status = boot_set_command_line(&ctx, &boot_state, "-v debug=0x8");
  if (EFI_ERROR(status)) {
    log_error(L"failed to set command line: %r\r\n", status);
    return status;
  }

  EFI_PHYSICAL_ADDRESS stack_base = 0xFFFFFFFFULL;
  UINTN stack_pages = 16;

  status = uefi_call_wrapper(
      ctx.bs->AllocatePages,
      4,
      AllocateMaxAddress,
      EfiLoaderData,
      stack_pages,
      &stack_base);

  if (EFI_ERROR(status)) {
    log_error(L"failed to allocate stack: %r\r\n", status);
    return status;
  }

  VOID *stack_top =
      (VOID *)((UINT8 *)(UINTN)stack_base +
               (stack_pages << EFI_PAGE_SHIFT) - 16);

  boot_log_args(&boot_state);
  log_info(L"boot_args phys = 0x%lx\r\n", (UINT64)(UINTN)boot_state.args);
  log_info(L"stack_top = 0x%lx\r\n", (UINT64)(UINTN)stack_top);
  log_info(L"entry = 0x%lx\r\n", (UINT64)(UINTN)host_entry);

  /* no logging after this point */
  status = exit_boot_services_retry(&ctx, image, &boot_state);
  if (EFI_ERROR(status)) {
    log_error(L"ExitBootServices failed: %r\r\n", status);
    return status;
  }

  volatile UINT32 *fb = (UINT32 *)(UINTN)boot_state.args->Video.v_baseAddr;
  for (UINTN y = 0; y < boot_state.args->Video.v_height; y++) {
    for (UINTN x = 0; x < boot_state.args->Video.v_width; x++) {
      fb[y * (boot_state.args->Video.v_rowBytes / 4) + x] = 0x0000FF00; /* green */
    }
  }

  jump_to_xnu(
      host_entry,
      (UINT64)(UINTN)boot_state.args,
      stack_top);
}