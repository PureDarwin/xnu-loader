#ifndef MACHO_H
#define MACHO_H

#include "common.h"
#include "app.h"
#include <stdint.h>

#define MH_MAGIC_64 0xfeedfacf
#define LC_SEGMENT_64 0x19
#define LC_UNIXTHREAD 0x5
#define LC_MAIN 0x80000028

#define MACHO_MAX_SEGMENTS 32
#define EFI_PAGE_SHIFT 12

#define X86_THREAD_STATE64 4

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

typedef struct macho_section_64 {
  CHAR8 sectname[16];
  CHAR8 segname[16];
  UINT64 addr;
  UINT64 size;
  UINT32 offset;
  UINT32 align;
  UINT32 reloff;
  UINT32 nreloc;
  UINT32 flags;
  UINT32 reserved1;
  UINT32 reserved2;
  UINT32 reserved3;
} macho_section_64;

typedef struct macho_thread_command {
  UINT32 cmd;
  UINT32 cmdsize;
} macho_thread_command;

typedef struct x86_thread_state64 {
  UINT64 rax;
  UINT64 rbx;
  UINT64 rcx;
  UINT64 rdx;
  UINT64 rdi;
  UINT64 rsi;
  UINT64 rbp;
  UINT64 rsp;
  UINT64 r8;
  UINT64 r9;
  UINT64 r10;
  UINT64 r11;
  UINT64 r12;
  UINT64 r13;
  UINT64 r14;
  UINT64 r15;
  UINT64 rip;
  UINT64 rflags;
  UINT64 cs;
  UINT64 fs;
  UINT64 gs;
} x86_thread_state64;

typedef struct MachoImage {
  VOID *data;
  UINTN size;
  macho_header_64 *header;
} MachoImage;

typedef struct MachoLoadedSegment {
  CHAR8 name[17];
  UINT64 vmaddr;
  UINT64 vmsize;
  UINT64 fileoff;
  UINT64 filesize;
  VOID *host_addr;
  UINT64 host_offset;
} MachoLoadedSegment;

typedef struct MachoLoadResult {
  EFI_PHYSICAL_ADDRESS host_base;
  UINT64 lowest_vmaddr;
  UINT64 highest_vmaddr;
  UINT64 image_size;
  UINTN page_count;

  UINTN segment_count;
  MachoLoadedSegment segments[MACHO_MAX_SEGMENTS];
} MachoLoadResult;

EFI_STATUS macho_parse(
    VOID *data,
    UINTN size,
    MachoImage *out_image);

EFI_STATUS macho_dump(
    MachoImage *image);

EFI_STATUS macho_load_segments(
    AppContext *ctx,
    MachoImage *image,
    MachoLoadResult *out_result);

VOID macho_unload_segments(
    AppContext *ctx,
    MachoLoadResult *result);

EFI_STATUS macho_find_entry_vmaddr(
    MachoImage *image,
    UINT64 *out_entry_vmaddr);

EFI_STATUS macho_compute_host_entry(
    MachoLoadResult *load_result,
    UINT64 entry_vmaddr,
    VOID **out_host_entry);

EFI_STATUS macho_find_segment_for_vmaddr(
    MachoImage *image,
    UINT64 vmaddr,
    macho_segment_command_64 **out_seg);

EFI_STATUS macho_find_section_for_vmaddr(
    MachoImage *image,
    UINT64 vmaddr,
    macho_segment_command_64 **out_seg,
    macho_section_64 **out_sec);

VOID macho_log_entry_context(
    MachoImage *image,
    UINT64 entry_vmaddr);

VOID macho_dump_entry_bytes(
    VOID *host_entry,
    UINTN count);

EFI_STATUS macho_find_section_by_name(
    MachoImage *image,
    const CHAR8 *segname,
    const CHAR8 *sectname,
    macho_segment_command_64 **out_seg,
    macho_section_64 **out_sec);

EFI_STATUS macho_compute_host_address(
    MachoLoadResult *load_result,
    UINT64 vmaddr,
    VOID **out_host_addr);

EFI_STATUS macho_get_section_host_info(
    MachoImage *image,
    MachoLoadResult *load_result,
    const CHAR8 *segname,
    const CHAR8 *sectname,
    UINT64 *out_vmaddr,
    UINT64 *out_size,
    VOID **out_host_addr);

VOID macho_log_section_host_info(
    MachoImage *image,
    MachoLoadResult *load_result,
    const CHAR8 *segname,
    const CHAR8 *sectname);

#endif