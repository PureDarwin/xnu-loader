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

  {
    EFI_LOADED_IMAGE *li = NULL;
    if (!EFI_ERROR(app_get_loaded_image(&ctx, &li)))
      log_info(L"loader image base=0x%lx size=0x%lx\r\n",
               (UINT64)(UINTN)li->ImageBase, (UINT64)li->ImageSize);
  }

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

  /* Dump memory map around 0x100000 before attempting AllocateAddress there */
  {
    UINTN map_size = 0, key = 0, desc_size = 0;
    UINT32 desc_ver = 0;
    EFI_MEMORY_DESCRIPTOR *mm = NULL;
    uefi_call_wrapper(ctx.bs->GetMemoryMap, 5, &map_size, mm, &key, &desc_size, &desc_ver);
    map_size += desc_size * 4;
    uefi_call_wrapper(ctx.bs->AllocatePool, 3, EfiLoaderData, map_size, (VOID **)&mm);
    if (mm) {
      uefi_call_wrapper(ctx.bs->GetMemoryMap, 5, &map_size, mm, &key, &desc_size, &desc_ver);
      UINTN nm = map_size / desc_size;
      for (UINTN mi = 0; mi < nm; mi++) {
        EFI_MEMORY_DESCRIPTOR *d = (EFI_MEMORY_DESCRIPTOR *)((UINT8 *)mm + mi * desc_size);
        UINT64 s = d->PhysicalStart;
        UINT64 e = s + ((UINT64)d->NumberOfPages << EFI_PAGE_SHIFT);
        if (e >= 0x80000 && s <= 0x2000000)
          log_info(L"memmap: type=%u 0x%lx-0x%lx pages=%lu\r\n",
                   d->Type, s, e, d->NumberOfPages);
      }
      uefi_call_wrapper(ctx.bs->FreePool, 1, mm);
    }
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

  log_info(L"[A] before boot_collect_memory_map\r\n");

  BootArgsState boot_state = {0};

  status = boot_collect_memory_map(&ctx, &boot_state);
  if (EFI_ERROR(status)) {
    log_error(L"failed to collect memory map: %r\r\n", status);
    return status;
  }

  /* Dump memory map entries near 0x100000 to diagnose AllocateAddress failure */
  {
    UINTN n = boot_state.memory_map_size / boot_state.descriptor_size;
    UINTN i;
    for (i = 0; i < n; i++) {
      EFI_MEMORY_DESCRIPTOR *d = (EFI_MEMORY_DESCRIPTOR *)((UINT8 *)boot_state.memory_map + i * boot_state.descriptor_size);
      UINT64 start = d->PhysicalStart;
      UINT64 end = start + ((UINT64)d->NumberOfPages << EFI_PAGE_SHIFT);
      if (end < 0x100000 || start > 0x300000)
        continue;
      log_info(L"memmap: type=%u phys=0x%lx-0x%lx pages=%lu attr=0x%lx\r\n", d->Type, start, end, d->NumberOfPages, d->Attribute);
    }
  }

  log_info(L"[B] before boot_build_args\r\n");

  status = boot_build_args(&ctx, &load_result, &boot_state);
  if (EFI_ERROR(status)) {
    log_error(L"failed to build boot_args: %r\r\n", status);
    return status;
  }

  log_info(L"[C] before boot_set_command_line\r\n");

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

  VOID *stack_top = (VOID *)((UINT8 *)(UINTN)stack_base + (stack_pages << EFI_PAGE_SHIFT) - 16);

  boot_log_args(&boot_state);

  /*
   * pin boot_args at a fixed low physical address before ExitBootServices.
   * The DT is already pinned at 0x91000 by dt_build_minimal (AllocateAddress + never freed), so deviceTreeP is already correct.
   *
   * 0x90000 = boot_args (1 page, safely below XNU's HIB at 0x100000)
   */
  {
    EFI_PHYSICAL_ADDRESS ba_fixed = 0x90000;

    status = uefi_call_wrapper(ctx.bs->AllocatePages, 4,
        AllocateAddress, EfiLoaderData, 1, &ba_fixed);
    if (EFI_ERROR(status)) {
        log_error(L"reloc: failed to alloc ba page at 0x90000: %r\r\n", status);
        return status;
    }

    boot_args *old = boot_state.args;
    boot_args *new = (boot_args *)(UINTN)ba_fixed;

    SetMem(new, sizeof(boot_args), 0);

    CopyMem(new, old, sizeof(*old));
    new->Revision = old->Revision;
    new->Version = old->Version;

    UINTN i;
    for (i = 0; i < BOOT_LINE_LENGTH - 1 && old->CommandLine[i]; i++) {
      new->CommandLine[i] = old->CommandLine[i];
    }

    new->CommandLine[i] = '\0';

    new->deviceTreeP = old->deviceTreeP;
    new->MemoryMap = old->MemoryMap;

    boot_state.args = new;
  }

  /* Reserve 2 pages at 0x92000 for the EFI memory map relocation.
   * exit_boot_services_retry overwrites boot_args->MemoryMap with the final
   * GetMemoryMap buffer address (high OVMF pool ~0x7e057000); we copy it here
   * after EBS returns, in the workarounds block below. */
  EFI_PHYSICAL_ADDRESS mm_fixed = 0x92000;
  status = uefi_call_wrapper(ctx.bs->AllocatePages, 4,
      AllocateAddress, EfiLoaderData, 2, &mm_fixed);
  if (EFI_ERROR(status)) {
    log_error(L"reloc: failed to alloc mmap pages at 0x92000: %r\r\n", status);
    return status;
  }

  log_info(L"[D] before exit_boot_services\r\n");
  log_info(L"boot_args phys = 0x%lx\r\n", (UINT64)(UINTN)boot_state.args);
  log_info(L"stack_top = 0x%lx\r\n", (UINT64)(UINTN)stack_top);
  log_info(L"entry = 0x%lx\r\n", (UINT64)(UINTN)host_entry);

  // Update boot_args to reflect physical load address
  boot_state.args->kaddr = (UINT32)load_result.lowest_vmaddr;
  boot_state.args->kslide = 0;

  log_info(L"kaddr=0x%x kslide=0x%x\r\n", boot_state.args->kaddr, boot_state.args->kslide);

  // no logging after this point
  status = exit_boot_services_retry(&ctx, image, &boot_state);
  if (EFI_ERROR(status)) {
    log_error(L"ExitBootServices failed: %r\r\n", status);
    return status;
  }

  /*
   * After ExitBootServices all boot-services memory is ours.
   * Copy each segment from its staging area to its physical target address.
   * Physical address = low 32 bits of vmaddr (strips 0xFFFFFF80 kernel bias).
   */
  for (UINT32 si = 0; si < load_result.segment_count; si++) {
    MachoLoadedSegment *seg = &load_result.segments[si];
    UINT64 *dst = (UINT64 *)(UINTN)(UINT32)seg->vmaddr;
    UINT64 *src = (UINT64 *)seg->host_addr;
    UINT64 fw = seg->filesize >> 3;
    UINT64 tail = seg->filesize &  7;
    UINT64 j;

    for (j = 0; j < fw; j++)
      dst[j] = src[j];

    // handle trailing bytes (filesize not a multiple of 8)
    UINT8 *db = (UINT8 *)(dst + fw);
    UINT8 *sb = (UINT8 *)(src + fw);
    for (j = 0; j < tail; j++)
      db[j] = sb[j];

    // zero vmsize - filesize
    UINT8 *zb   = (UINT8 *)(UINTN)(UINT32)seg->vmaddr + seg->filesize;
    UINT64 zlen = seg->vmsize - seg->filesize;
    UINT64 *z64 = (UINT64 *)zb;
    UINT64 zw   = zlen >> 3;
    for (j = 0; j < zw; j++)
      z64[j] = 0;
    zb = (UINT8 *)(z64 + zw);
    for (j = 0; j < (zlen & 7); j++)
      zb[j] = 0;
  }

  // Vali: any patches below were cheating using a LLM, so do take it with a grain of salt.

  // Physical entry = low 32 bits of entry vmaddr
  VOID *phys_entry = (VOID *)(UINTN)(UINT32)entry_vmaddr;

  /*
   * Boot CPU workarounds - applied after segment copy, before kernel entry.
   *
   * scdatas[0] is in __DATA at physical 0xa04e80.
   *
   * cpu_int_stack_top redirect (phys 0xa04ec0 = scdatas[0]+0x40):
   *    The static initializer points to low_eintstack (phys 0x19b000), only
   *    4KB above idt64_hndl_table0 (0x196000).  The i386_init call chain
   *    consumes ~250KB, overflowing into the IDT and corrupting trap dispatch.
   *    Redirect to phys 0x80000 (VA 0xffffff8000080000); the stack grows DOWN
   *    from there, away from 0x196000, with 384KB of headroom.
   *
   * cpu_active_thread (phys 0xa04e90 = scdatas[0]+0x10):
   *    vm_fault_internal reads %gs:0x10 early in its prologue.  With the
   *    default NULL it accesses 0x41c(NULL) = user-space VA 0x41c, which
   *    faults after Idle_PTs_init installs PML4[0]=0, causing infinite
   *    page-fault recursion.  Point at a zeroed fake-thread page (phys 0x6000,
   *    VA 0xffffff8000006000) inside KPTphys coverage so the dereference
   *    returns 0 instead of faulting.
   *
   * fake_thread+0x6a8 (phys 0x66a8):
   *    vm_fault_internal computes rdi = 0x390 + *(cpu_active_thread+0x6a8)
   *    before the second counter_incPPy call.  With the zeroed fake thread,
   *    rdi = 0x390, a user-space address that faults.  Pre-load 0x6a8 with
   *    0xffffff8000a00ea0 so rdi = 0xffffff8000a01230, the same mapped kernel
   *    address used by the first counter_incPPy call.
   */

  /* Relocate the EFI memory map to physical 0x92000.
   * exit_boot_services_retry set boot_args->MemoryMap to the final GetMemoryMap
   * buffer (~0x7e057000), which is above XNU's bootPT coverage (~6MB).  Copy to
   * the pre-allocated pages at 0x92000 and patch the pointer.  Both addresses
   * are reachable via UEFI's identity-mapped page tables still active in CR3. */
  {
    UINT64 *src = (UINT64 *)(UINTN)boot_state.args->MemoryMap;
    UINT64 *dst = (UINT64 *)(UINTN)0x92000ULL;
    UINTN sz  = (UINTN)boot_state.args->MemoryMapSize;
    UINTN wi;
    for (wi = 0; wi < (sz + 7) / 8; wi++)
      dst[wi] = src[wi];
    boot_state.args->MemoryMap = (UINT32)0x92000;
  }

  /* PhysicalMemorySize: app_detect_physical_memory_size() returns 0 because the
   * first GetMemoryMap(NULL) call doesn't return EFI_BUFFER_TOO_SMALL on this
   * OVMF build.  XNU's pmap_bootstrap() uses mem_size to allocate pmap arrays;
   * with 0 it allocates nothing and zones immediately run out of bitmap space.
   * Compute the correct value from the final memory map we just copied to 0x92000.
   * Count conventional + reclaimable types (1=LoaderCode, 2=LoaderData,
   * 3=BSCode, 4=BSData, 7=Conventional, 9=ACPIReclaim, 10=ACPINVS). */
  {
    UINT32  desc_sz = boot_state.args->MemoryMapDescriptorSize;
    UINT32  map_sz  = boot_state.args->MemoryMapSize;
    UINT8  *map     = (UINT8 *)(UINTN)0x92000ULL;
    UINT64  total   = 0;
    UINT32  off;
    for (off = 0; off + desc_sz <= map_sz; off += desc_sz) {
      UINT32 typ = *(UINT32 *)(map + off); // EFI_MEMORY_DESCRIPTOR.Type
      UINT64 npg = *(UINT64 *)(map + off + 24); // .NumberOfPages
      if (typ == 1 || typ == 2 || typ == 3 || typ == 4 ||  typ == 7 || typ == 9 || typ == 10)
        total += npg << EFI_PAGE_SHIFT;
    }
    boot_state.args->PhysicalMemorySize = total;
  }

  // Zero the fake-thread page (phys 0x1a0000-0x1a0fff).
  {
    volatile UINT64 *p = (volatile UINT64 *)(UINTN)0x1a0000ULL;
    UINTN i;
    for (i = 0; i < 512; i++)
      p[i] = 0ULL;
  }

  // cpu_int_stack_top: scdatas[0]+0x40 = phys 0xa04ec0
  *((volatile UINT64 *)(UINTN)0xa04ec0ULL) = 0xffffff8000080000ULL;

  /* cpu_active_thread: scdatas[0]+0x10 = phys 0xa04e90.
   * Use 0x1a0000 (just past __HIB end at 0x19ffff) - this VA is NOT freed
   * by XNU's low-memory cleanup (which removes 0x0000-0xffff), so the
   * fake thread remains accessible throughout boot and in the panic handler.
   */
  *((volatile UINT64 *)(UINTN)0xa04e90ULL) = 0xffffff80001a0000ULL;

  /* fake_thread+0x6a8 at phys 0x1a06a8.
   * Stack never reaches 0x1a0000 (stack top = 0x19b000 < 0x1a0000) so this
   * value survives until counter_incPPy reads it. */
  *((volatile UINT64 *)(UINTN)0x1a06a8ULL) = 0xffffff8000a00ea0ULL;

  /* max_cpus_from_firmware: XNU panics in percpu_size() if this is 0.
   * Normally set by ACPI MADT parsing (lapic_init). Without proper ACPI
   * the MADT walker never runs. Set to 1 (single BSP) post-segment-copy
   * so percpu initialization proceeds. Physical 0xbc2a20 (in __DATA). */
  *((volatile UINT32 *)(UINTN)0xbc2a20ULL) = 1UL;

  /* cpu_preemption_level remains 0 (default).
   * Previously set to 1 to bypass vm_fault_internal's slow path (which would
   * deadlock on the vm_map lock held by the "Hiding local relocations" caller
   * during the spin-lock acquisition fault at 0x38064f).  That fault was a
   * secondary effect of the EFI memory map being at an unmapped high address.
   * With MemoryMap now relocated to 0x92000, the deadlock path is not reached.
   * XNU uses lazy mapping, kernel pages are demand-faulted in by vm_fault_internal.
   * Keeping preemption_level=0 lets vm_fault_internal handle these normally. */

  jump_to_xnu(
      phys_entry,
      (UINT64)(UINTN)boot_state.args,
      stack_top);
}