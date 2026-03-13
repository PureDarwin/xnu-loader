#ifndef MACHO_H
#define MACHO_H

#include "common.h"
#include <stdint.h>

#define MH_MAGIC_64 0xfeedfacf
#define LC_SEGMENT_64 0x19
#define LC_UNIXTHREAD 0x5
#define LC_MAIN 0x80000028

typedef struct macho_header_64 {
  UINT32 magic;
  INT32 cputype;
  INT32 cpusubtype;
  UINT32 filetype;
  UINT32 ncmds;
  UINT32 sizeofcmds;
  UINT32 flags;
  UINT32 reserved;
} macho_header_64;

typedef struct macho_load_command {
  UINT32 cmd;
  UINT32 cmdsize;
} macho_load_command;

typedef struct macho_segment_command_64 {
  UINT32 cmd;
  UINT32 cmdsize;
  CHAR8 segname[16];
  UINT64 vmaddr;
  UINT64 vmsize;
  UINT64 fileoff;
  UINT64 filesize;
  INT32 maxprot;
  INT32 initprot;
  UINT32 nsects;
  UINT32 flags;
} macho_segment_command_64;

typedef struct MachoImage {
  VOID *data;
  UINTN size;
  macho_header_64 *header;
} MachoImage;

EFI_STATUS macho_parse(VOID *data, UINTN size, MachoImage *out_image);
EFI_STATUS macho_dump(MachoImage *image);

#endif