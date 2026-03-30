#include "macho.h"
#include "console.h"

static UINT64 align_up(UINT64 value, UINT64 align) {
  return (value + (align - 1)) & ~(align - 1);
}

static VOID macho_copy_segname(CHAR16 *out16, CHAR8 *in8) {
  UINTN i;

  for (i = 0; i < 16; i++)
    out16[i] = (CHAR16)in8[i];

  out16[16] = L'\0';
}

static VOID macho_copy_segname8(CHAR8 *out8, CHAR8 *in8) {
  UINTN i;

  for (i = 0; i < 16; i++)
    out8[i] = in8[i];

  out8[16] = '\0';
}

static EFI_STATUS macho_validate_load_command(
    MachoImage *image,
    UINT8 *p,
    macho_load_command **out_lc)
{
  macho_load_command *lc;
  UINTN offset;

  if (!image || !p || !out_lc)
    return EFI_INVALID_PARAMETER;

  offset = (UINTN)(p - (UINT8 *)image->data);
  if (offset + sizeof(macho_load_command) > image->size)
    return EFI_COMPROMISED_DATA;

  lc = (macho_load_command *)p;

  if (lc->cmdsize == 0)
    return EFI_COMPROMISED_DATA;

  if (offset + lc->cmdsize > image->size)
    return EFI_COMPROMISED_DATA;

  *out_lc = lc;
  return EFI_SUCCESS;
}

static EFI_STATUS macho_compute_vm_range(
    MachoImage *image,
    UINT64 *out_lowest,
    UINT64 *out_highest)
{
  macho_header_64 *hdr;
  macho_load_command *lc;
  UINT8 *p;
  UINT32 i;
  BOOLEAN found;
  UINT64 lowest;
  UINT64 highest;

  if (!image || !image->header || !out_lowest || !out_highest)
    return EFI_INVALID_PARAMETER;

  hdr = image->header;
  p = (UINT8 *)(hdr + 1);
  found = FALSE;
  lowest = 0;
  highest = 0;

  for (i = 0; i < hdr->ncmds; i++) {
    EFI_STATUS status;

    status = macho_validate_load_command(image, p, &lc);
    if (EFI_ERROR(status))
      return status;

    if (lc->cmd == LC_SEGMENT_64) {
      macho_segment_command_64 *seg = (macho_segment_command_64 *)p;
      UINT64 seg_end;

      if (seg->vmsize == 0) {
        p += lc->cmdsize;
        continue;
      }

      seg_end = seg->vmaddr + seg->vmsize;
      if (seg_end < seg->vmaddr)
        return EFI_COMPROMISED_DATA;

      if (!found) {
        lowest = seg->vmaddr;
        highest = seg_end;
        found = TRUE;
      } else {
        if (seg->vmaddr < lowest)
          lowest = seg->vmaddr;
        if (seg_end > highest)
          highest = seg_end;
      }
    }

    p += lc->cmdsize;
  }

  if (!found)
    return EFI_NOT_FOUND;

  *out_lowest = lowest;
  *out_highest = highest;
  return EFI_SUCCESS;
}

static VOID macho_copy_name16(CHAR16 *out16, CHAR8 *in8) {
  UINTN i;

  for (i = 0; i < 16; i++)
    out16[i] = (CHAR16)in8[i];

  out16[16] = L'\0';
}

static BOOLEAN macho_name16_equal(const CHAR8 *a, const CHAR8 *b) {
  UINTN i;

  for (i = 0; i < 16; i++) {
    CHAR8 ca = a[i];
    CHAR8 cb = b[i];

    if (ca != cb)
      return FALSE;

    if (ca == '\0')
      return TRUE;
  }

  return TRUE;
}

EFI_STATUS macho_parse(VOID *data, UINTN size, MachoImage *out_image) {
  macho_header_64 *hdr;

  if (!data || !out_image || size < sizeof(macho_header_64))
    return EFI_INVALID_PARAMETER;

  hdr = (macho_header_64 *)data;
  if (hdr->magic != MH_MAGIC_64)
    return EFI_LOAD_ERROR;

  out_image->data = data;
  out_image->size = size;
  out_image->header = hdr;
  return EFI_SUCCESS;
}

EFI_STATUS macho_dump(MachoImage *image) {
  macho_header_64 *hdr;
  macho_load_command *lc;
  UINT8 *p;
  UINT32 i;

  if (!image || !image->header)
    return EFI_INVALID_PARAMETER;

  hdr = image->header;

  log_info(L"Mach-O 64-bit\r\n");
  log_info(L"  ncmds: %u\r\n", hdr->ncmds);
  log_info(L"  sizeofcmds: %u\r\n", hdr->sizeofcmds);
  log_info(L"  flags: 0x%x\r\n", hdr->flags);

  p = (UINT8 *)(hdr + 1);
  for (i = 0; i < hdr->ncmds; i++) {
    EFI_STATUS status;

    status = macho_validate_load_command(image, p, &lc);
    if (EFI_ERROR(status))
      return status;

    if (lc->cmd == LC_SEGMENT_64) {
      macho_segment_command_64 *seg = (macho_segment_command_64 *)p;
      CHAR16 name[17];

      macho_copy_segname(name, seg->segname);

      log_info(
          L"segment %-16s vm=0x%lx vmsz=0x%lx fileoff=0x%lx filesz=0x%lx\r\n",
          name,
          seg->vmaddr,
          seg->vmsize,
          seg->fileoff,
          seg->filesize);
    }

    p += lc->cmdsize;
  }

  return EFI_SUCCESS;
}

EFI_STATUS macho_load_segments(
    AppContext *ctx,
    MachoImage *image,
    MachoLoadResult *out_result)
{
  macho_header_64 *hdr;
  macho_load_command *lc;
  UINT8 *p;
  UINT32 i;
  EFI_STATUS status;
  UINT64 lowest_vmaddr;
  UINT64 highest_vmaddr;
  UINT64 image_size;
  UINTN page_count;
  EFI_PHYSICAL_ADDRESS host_base;
  UINT8 *base_ptr;
  UINT64 j;

  if (!ctx || !image || !image->header || !out_result)
    return EFI_INVALID_PARAMETER;

  out_result->host_base = 0;
  out_result->lowest_vmaddr = 0;
  out_result->highest_vmaddr = 0;
  out_result->image_size = 0;
  out_result->page_count = 0;
  out_result->segment_count = 0;

  status = macho_compute_vm_range(image, &lowest_vmaddr, &highest_vmaddr);
  if (EFI_ERROR(status))
    return status;

  image_size = highest_vmaddr - lowest_vmaddr;
  page_count = (UINTN)(align_up(image_size, EFI_PAGE_SIZE) >> EFI_PAGE_SHIFT);
  host_base = 0xFFFFFFFFULL;

  status = uefi_call_wrapper(
      ctx->bs->AllocatePages,
      4,
      AllocateMaxAddress,
      EfiLoaderData,
      page_count,
      &host_base);
  if (EFI_ERROR(status))
    return status;

  base_ptr = (UINT8 *)(UINTN)host_base;

  for (j = 0; j < ((UINT64)page_count << EFI_PAGE_SHIFT); j++)
    base_ptr[j] = 0;

  out_result->host_base = host_base;
  out_result->lowest_vmaddr = lowest_vmaddr;
  out_result->highest_vmaddr = highest_vmaddr;
  out_result->image_size = image_size;
  out_result->page_count = page_count;

  log_info(
      L"allocated staged image host=0x%lx vm_low=0x%lx vm_high=0x%lx span=0x%lx pages=%u\r\n",
      (UINT64)host_base,
      lowest_vmaddr,
      highest_vmaddr,
      image_size,
      page_count);

  hdr = image->header;
  p = (UINT8 *)(hdr + 1);

  for (i = 0; i < hdr->ncmds; i++) {
    status = macho_validate_load_command(image, p, &lc);
    if (EFI_ERROR(status))
      goto fail;

    if (lc->cmd == LC_SEGMENT_64) {
      macho_segment_command_64 *seg = (macho_segment_command_64 *)p;
      MachoLoadedSegment *loaded;
      UINT8 *src;
      UINT8 *dst;
      UINT64 seg_offset;
      CHAR16 name16[17];

      if (seg->vmsize == 0) {
        p += lc->cmdsize;
        continue;
      }

      if (out_result->segment_count >= MACHO_MAX_SEGMENTS) {
        status = EFI_BUFFER_TOO_SMALL;
        goto fail;
      }

      if (seg->fileoff > image->size) {
        status = EFI_COMPROMISED_DATA;
        goto fail;
      }

      if (seg->filesize > image->size) {
        status = EFI_COMPROMISED_DATA;
        goto fail;
      }

      if (seg->fileoff + seg->filesize > image->size) {
        status = EFI_COMPROMISED_DATA;
        goto fail;
      }

      if (seg->vmaddr < lowest_vmaddr) {
        status = EFI_COMPROMISED_DATA;
        goto fail;
      }

      seg_offset = seg->vmaddr - lowest_vmaddr;
      if (seg_offset + seg->vmsize > image_size) {
        status = EFI_COMPROMISED_DATA;
        goto fail;
      }

      dst = base_ptr + seg_offset;
      src = (UINT8 *)image->data + seg->fileoff;

      for (j = 0; j < seg->filesize; j++)
        dst[j] = src[j];

      loaded = &out_result->segments[out_result->segment_count++];
      macho_copy_segname8(loaded->name, seg->segname);
      loaded->vmaddr = seg->vmaddr;
      loaded->vmsize = seg->vmsize;
      loaded->fileoff = seg->fileoff;
      loaded->filesize = seg->filesize;
      loaded->host_addr = dst;
      loaded->host_offset = seg_offset;

      macho_copy_segname(name16, seg->segname);
      log_info(
          L"loaded  %-16s vm=0x%lx -> host=0x%lx off=0x%lx size=0x%lx\r\n",
          name16,
          loaded->vmaddr,
          (UINT64)(UINTN)loaded->host_addr,
          loaded->host_offset,
          loaded->vmsize);
    }

    p += lc->cmdsize;
  }

  return EFI_SUCCESS;

fail:
  macho_unload_segments(ctx, out_result);
  return status;
}

VOID macho_unload_segments(
    AppContext *ctx,
    MachoLoadResult *result)
{
  if (!ctx || !result)
    return;

  if (result->host_base != 0) {
    uefi_call_wrapper(
        ctx->bs->FreePages,
        2,
        result->host_base,
        result->page_count);
  }

  result->host_base = 0;
  result->lowest_vmaddr = 0;
  result->highest_vmaddr = 0;
  result->image_size = 0;
  result->page_count = 0;
  result->segment_count = 0;
}

EFI_STATUS macho_find_entry_vmaddr(
    MachoImage *image,
    UINT64 *out_entry_vmaddr) {
  macho_header_64 *hdr;
  macho_load_command *lc;
  UINT8 *p;
  UINT32 i;

  if (!image || !image->header || !out_entry_vmaddr)
    return EFI_INVALID_PARAMETER;

  hdr = image->header;
  p = (UINT8 *)(hdr + 1);

  for (i = 0; i < hdr->ncmds; i++) {
    EFI_STATUS status;

    status = macho_validate_load_command(image, p, &lc);
    if (EFI_ERROR(status))
      return status;

    if (lc->cmd == LC_UNIXTHREAD) {
      UINT8 *q = p + sizeof(macho_thread_command);
      UINT8 *end = p + lc->cmdsize;

      while (q + sizeof(UINT32) * 2 <= end) {
        UINT32 flavor = *(UINT32 *)q;
        UINT32 count  = *(UINT32 *)(q + sizeof(UINT32));
        UINT8 *state  = q + sizeof(UINT32) * 2;
        UINTN state_size = (UINTN)count * sizeof(UINT32);

        if (state + state_size > end)
          return EFI_COMPROMISED_DATA;

        if (flavor == X86_THREAD_STATE64) {
          if (state_size < sizeof(x86_thread_state64))
            return EFI_COMPROMISED_DATA;

          x86_thread_state64 *ts = (x86_thread_state64 *)state;
          *out_entry_vmaddr = ts->rip;
          return EFI_SUCCESS;
        }

        q = state + state_size;
      }

      return EFI_NOT_FOUND;
    }

    p += lc->cmdsize;
  }

  return EFI_NOT_FOUND;
}

EFI_STATUS macho_compute_host_entry(
    MachoLoadResult *load_result,
    UINT64 entry_vmaddr,
    VOID **out_host_entry)
{
  return macho_compute_host_address(load_result, entry_vmaddr, out_host_entry);
}

EFI_STATUS macho_find_segment_for_vmaddr(
    MachoImage *image,
    UINT64 vmaddr,
    macho_segment_command_64 **out_seg)
{
  macho_header_64 *hdr;
  macho_load_command *lc;
  UINT8 *p;
  UINT32 i;

  if (!image || !image->header || !out_seg)
    return EFI_INVALID_PARAMETER;

  hdr = image->header;
  p = (UINT8 *)(hdr + 1);

  for (i = 0; i < hdr->ncmds; i++) {
    EFI_STATUS status;

    status = macho_validate_load_command(image, p, &lc);
    if (EFI_ERROR(status))
      return status;

    if (lc->cmd == LC_SEGMENT_64) {
      macho_segment_command_64 *seg = (macho_segment_command_64 *)p;

      if (seg->vmsize != 0 &&
          vmaddr >= seg->vmaddr &&
          vmaddr < seg->vmaddr + seg->vmsize) {
        *out_seg = seg;
        return EFI_SUCCESS;
      }
    }

    p += lc->cmdsize;
  }

  return EFI_NOT_FOUND;
}

EFI_STATUS macho_find_section_for_vmaddr(
    MachoImage *image,
    UINT64 vmaddr,
    macho_segment_command_64 **out_seg,
    macho_section_64 **out_sec)
{
  macho_header_64 *hdr;
  macho_load_command *lc;
  UINT8 *p;
  UINT32 i;

  if (!image || !image->header || !out_seg || !out_sec)
    return EFI_INVALID_PARAMETER;

  hdr = image->header;
  p = (UINT8 *)(hdr + 1);

  for (i = 0; i < hdr->ncmds; i++) {
    EFI_STATUS status;

    status = macho_validate_load_command(image, p, &lc);
    if (EFI_ERROR(status))
      return status;

    if (lc->cmd == LC_SEGMENT_64) {
      macho_segment_command_64 *seg = (macho_segment_command_64 *)p;
      macho_section_64 *sec;
      UINT32 j;

      if (seg->vmsize == 0) {
        p += lc->cmdsize;
        continue;
      }

      if (!(vmaddr >= seg->vmaddr && vmaddr < seg->vmaddr + seg->vmsize)) {
        p += lc->cmdsize;
        continue;
      }

      sec = (macho_section_64 *)(seg + 1);
      for (j = 0; j < seg->nsects; j++) {
        if (vmaddr >= sec[j].addr && vmaddr < sec[j].addr + sec[j].size) {
          *out_seg = seg;
          *out_sec = &sec[j];
          return EFI_SUCCESS;
        }
      }

      *out_seg = seg;
      *out_sec = NULL;
      return EFI_SUCCESS;
    }

    p += lc->cmdsize;
  }

  return EFI_NOT_FOUND;
}

VOID macho_log_entry_context(
    MachoImage *image,
    UINT64 entry_vmaddr)
{
  EFI_STATUS status;
  macho_segment_command_64 *seg = NULL;
  macho_section_64 *sec = NULL;
  CHAR16 segname[17];
  CHAR16 sectname[17];

  status = macho_find_section_for_vmaddr(image, entry_vmaddr, &seg, &sec);
  if (EFI_ERROR(status)) {
    log_error(L"failed to locate entry segment/section: %r\r\n", status);
    return;
  }

  macho_copy_name16(segname, seg->segname);

  if (sec) {
    macho_copy_name16(sectname, sec->sectname);
    log_info(
        L"entry is in segment %s, section %s, seg_off=0x%lx sec_off=0x%lx\r\n",
        segname,
        sectname,
        entry_vmaddr - seg->vmaddr,
        entry_vmaddr - sec->addr);
  } else {
    log_info(
        L"entry is in segment %s, no section match, seg_off=0x%lx\r\n",
        segname,
        entry_vmaddr - seg->vmaddr);
  }
}

VOID macho_dump_entry_bytes(
    VOID *host_entry,
    UINTN count)
{
  UINT8 *p;
  UINTN i;

  if (!host_entry || count == 0)
    return;

  p = (UINT8 *)host_entry;

  log_info(L"entry bytes:");
  for (i = 0; i < count; i++) {
    if ((i % 16) == 0)
      log_info(L"\r\n  ");
    log_info(L"%02x ", p[i]);
  }
  log_info(L"\r\n");
}

EFI_STATUS macho_find_section_by_name(
    MachoImage *image,
    const CHAR8 *segname,
    const CHAR8 *sectname,
    macho_segment_command_64 **out_seg,
    macho_section_64 **out_sec)
{
  macho_header_64 *hdr;
  macho_load_command *lc;
  UINT8 *p;
  UINT32 i;

  if (!image || !image->header || !segname || !sectname || !out_seg || !out_sec)
    return EFI_INVALID_PARAMETER;

  hdr = image->header;
  p = (UINT8 *)(hdr + 1);

  for (i = 0; i < hdr->ncmds; i++) {
    EFI_STATUS status;

    status = macho_validate_load_command(image, p, &lc);
    if (EFI_ERROR(status))
      return status;

    if (lc->cmd == LC_SEGMENT_64) {
      macho_segment_command_64 *seg = (macho_segment_command_64 *)p;
      macho_section_64 *sec = (macho_section_64 *)(seg + 1);
      UINT32 j;

      if (!macho_name16_equal(seg->segname, segname)) {
        p += lc->cmdsize;
        continue;
      }

      for (j = 0; j < seg->nsects; j++) {
        if (macho_name16_equal(sec[j].sectname, sectname)) {
          *out_seg = seg;
          *out_sec = &sec[j];
          return EFI_SUCCESS;
        }
      }
    }

    p += lc->cmdsize;
  }

  return EFI_NOT_FOUND;
}

EFI_STATUS macho_compute_host_address(
    MachoLoadResult *load_result,
    UINT64 vmaddr,
    VOID **out_host_addr)
{
  UINT64 offset;

  if (!load_result || !out_host_addr)
    return EFI_INVALID_PARAMETER;

  if (vmaddr < load_result->lowest_vmaddr)
    return EFI_COMPROMISED_DATA;

  offset = vmaddr - load_result->lowest_vmaddr;
  if (offset >= load_result->image_size)
    return EFI_COMPROMISED_DATA;

  *out_host_addr = (VOID *)((UINT8 *)(UINTN)load_result->host_base + offset);
  return EFI_SUCCESS;
}

EFI_STATUS macho_get_section_host_info(
    MachoImage *image,
    MachoLoadResult *load_result,
    const CHAR8 *segname,
    const CHAR8 *sectname,
    UINT64 *out_vmaddr,
    UINT64 *out_size,
    VOID **out_host_addr)
{
  EFI_STATUS status;
  macho_segment_command_64 *seg = NULL;
  macho_section_64 *sec = NULL;
  VOID *host_addr = NULL;

  if (!image || !load_result || !segname || !sectname ||
      !out_vmaddr || !out_size || !out_host_addr)
    return EFI_INVALID_PARAMETER;

  status = macho_find_section_by_name(image, segname, sectname, &seg, &sec);
  if (EFI_ERROR(status))
    return status;

  status = macho_compute_host_address(load_result, sec->addr, &host_addr);
  if (EFI_ERROR(status))
    return status;

  *out_vmaddr = sec->addr;
  *out_size = sec->size;
  *out_host_addr = host_addr;
  return EFI_SUCCESS;
}

VOID macho_log_section_host_info(
    MachoImage *image,
    MachoLoadResult *load_result,
    const CHAR8 *segname,
    const CHAR8 *sectname)
{
  EFI_STATUS status;
  UINT64 vmaddr = 0;
  UINT64 size = 0;
  VOID *host_addr;
  CHAR16 seg16[17];
  CHAR16 sec16[17];

  status = macho_get_section_host_info(
      image,
      load_result,
      segname,
      sectname,
      &vmaddr,
      &size,
      &host_addr);

  macho_copy_name16(seg16, (CHAR8 *)segname);
  macho_copy_name16(sec16, (CHAR8 *)sectname);

  if (EFI_ERROR(status)) {
    log_error(L"failed to find section %s,%s: %r\r\n", seg16, sec16, status);
    return;
  }

  log_info(
      L"section %s,%s vm=0x%lx host=0x%lx size=0x%lx\r\n",
      seg16,
      sec16,
      vmaddr,
      (UINT64)(UINTN)host_addr,
      size);
}