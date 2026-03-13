#include "macho.h"
#include "console.h"

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

  log_info(L"Mach-O 64-bit\n");
  log_info(L"  ncmds: %u\n", hdr->ncmds);
  log_info(L"  sizeofcmds: %u\n", hdr->sizeofcmds);
  log_info(L"  flags: 0x%x\n", hdr->flags);

  p = (UINT8 *)(hdr + 1);
  for (i = 0; i < hdr->ncmds; i++) {
    lc = (macho_load_command *)p;

    if ((UINTN)(p + sizeof(*lc) - (UINT8 *)image->data) > image->size)
      return EFI_COMPROMISED_DATA;

    if (lc->cmd == LC_SEGMENT_64) {
      macho_segment_command_64 *seg = (macho_segment_command_64 *)p;
      CHAR16 name[17];
      UINTN j;

      for (j = 0; j < 16; j++)
        name[j] = (CHAR16)seg->segname[j];
      name[16] = L'\0';

      log_info(L"segment %-16s vm=0x%lx vmsz=0x%lx fileoff=0x%lx filesz=0x%lx\n",
               name, seg->vmaddr, seg->vmsize, seg->fileoff, seg->filesize);
    }

    if (lc->cmdsize == 0)
      return EFI_COMPROMISED_DATA;

    p += lc->cmdsize;
  }

  return EFI_SUCCESS;
}