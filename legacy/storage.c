#include "firmware.h"
#include <efilib.h>

#define FAT_LBA 2048U
#define ATA_DATA 0x1f0
#define ATA_STATUS 0x1f7
#define ATA_DRQ 0x08
#define ATA_ERR 0x01

typedef struct __attribute__((packed)) {
  UINT8 jump[3], oem[8];
  UINT16 bytes_sector;
  UINT8 sectors_cluster;
  UINT16 reserved;
  UINT8 fats;
  UINT16 root_entries, total16;
  UINT8 media;
  UINT16 fat16, sectors_track, heads;
  UINT32 hidden, total32, fat32;
  UINT16 flags, version;
  UINT32 root_cluster;
} FatBpb;

typedef struct {
  EFI_FILE_PROTOCOL proto;
  UINT32 cluster;
  UINT32 size;
  UINT32 position;
  BOOLEAN directory;
  BOOLEAN allocated;
} LegacyFile;

static FatBpb bpb;
static EFI_SIMPLE_FILE_SYSTEM_PROTOCOL fs;
static EFI_BLOCK_IO_PROTOCOL block_io;
static EFI_BLOCK_IO_MEDIA block_media;
static LegacyFile root;
static LegacyFile files[12];
static EFI_HANDLE volume_handle = (EFI_HANDLE)(UINTN)0x46415432;
static EFI_GUID fs_guid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
static EFI_GUID block_io_guid = EFI_BLOCK_IO_PROTOCOL_GUID;
static EFI_GUID file_info_guid = EFI_FILE_INFO_ID;

static BOOLEAN guid_equal_local(const EFI_GUID *a, const EFI_GUID *b) {
  const UINT64 *x = (const UINT64 *)a, *y = (const UINT64 *)b;
  return x[0] == y[0] && x[1] == y[1];
}

static UINT8 in8(UINT16 p) {
  UINT8 v;
  __asm__ volatile("inb %1,%0" : "=a"(v) : "Nd"(p));
  return v;
}

static void out8(UINT16 p, UINT8 v) {
  __asm__ volatile("outb %0,%1" ::"a"(v), "Nd"(p));
}

static EFI_STATUS ata_read(UINT32 lba, UINT8 *dst) {
  for (UINTN spin = 0; spin < 1000000; ++spin)
    if (!(in8(ATA_STATUS) & 0x80))
      break;
  out8(0x1f6, 0xe0 | ((lba >> 24) & 0x0f));
  out8(0x1f2, 1);
  out8(0x1f3, lba);
  out8(0x1f4, lba >> 8);
  out8(0x1f5, lba >> 16);
  out8(ATA_STATUS, 0x20);
  UINT8 status = 0;
  for (UINTN spin = 0; spin < 1000000; ++spin) {
    status = in8(ATA_STATUS);
    if (status & ATA_ERR)
      return EFI_DEVICE_ERROR;
    if (!(status & 0x80) && (status & ATA_DRQ))
      break;
  }
  if (!(status & ATA_DRQ))
    return EFI_TIMEOUT;
  UINTN words = 256;
  __asm__ volatile("cld; rep insw"
                   : "+D"(dst), "+c"(words)
                   : "d"((UINT16)ATA_DATA)
                   : "memory");
  return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI block_reset(EFI_BLOCK_IO_PROTOCOL *self,
                                     BOOLEAN extended_verification) {
  (void)self;
  (void)extended_verification;
  return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI block_read(EFI_BLOCK_IO_PROTOCOL *self,
                                    UINT32 media_id, EFI_LBA lba,
                                    UINTN size, VOID *buffer) {
  if (!self || !buffer || media_id != block_media.MediaId)
    return EFI_INVALID_PARAMETER;
  if ((size % 512) != 0 || lba > block_media.LastBlock ||
      size / 512 > block_media.LastBlock - lba + 1)
    return EFI_BAD_BUFFER_SIZE;

  UINT8 *dst = buffer;
  while (size) {
    EFI_STATUS status = ata_read((UINT32)lba, dst);
    if (EFI_ERROR(status))
      return status;
    ++lba;
    dst += 512;
    size -= 512;
  }
  return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI block_write(EFI_BLOCK_IO_PROTOCOL *self,
                                     UINT32 media_id, EFI_LBA lba,
                                     UINTN size, VOID *buffer) {
  (void)self;
  (void)media_id;
  (void)lba;
  (void)size;
  (void)buffer;
  return EFI_WRITE_PROTECTED;
}

static EFI_STATUS EFIAPI block_flush(EFI_BLOCK_IO_PROTOCOL *self) {
  (void)self;
  return EFI_SUCCESS;
}

static UINT32 first_data(void) {
  return FAT_LBA + bpb.reserved + bpb.fats * bpb.fat32;
}

static UINT32 cluster_lba(UINT32 c) {
  return first_data() + (c - 2) * bpb.sectors_cluster;
}

static UINT32 next_cluster(UINT32 c) {
  UINT8 sec[512];
  UINT32 off = c * 4;
  if (EFI_ERROR(ata_read(FAT_LBA + bpb.reserved + off / 512, sec)))
    return 0x0fffffff;
  return (*(UINT32 *)(sec + off % 512)) & 0x0fffffff;
}

static void short_name(CONST CHAR16 *component, UINTN len, CHAR8 out[11]) {
  for (UINTN i = 0; i < 11; ++i)
    out[i] = ' ';
  /* Stable aliases installed by the image builder for names that need LFNs. */
  if (len == 13) {
    CONST CHAR16 alias[] = L"BOOTARGS.TXT";
    component = alias;
    len = 12;
  }
  UINTN at = 0, ext = 8;
  for (UINTN i = 0; i < len; ++i) {
    CHAR16 c = component[i];
    if (c == '.') {
      at = ext;
      continue;
    }
    if (at >= 11)
      continue;
    if (c >= 'a' && c <= 'z')
      c -= 32;
    out[at++] = (CHAR8)c;
  }
}

static EFI_STATUS find_entry(UINT32 dir_cluster, CONST CHAR16 *name, UINTN len,
                             UINT32 *cluster, UINT32 *size, BOOLEAN *directory) {
  CHAR8 wanted[11];
  short_name(name, len, wanted);
  UINT8 sec[512];
  for (UINT32 c = dir_cluster; c >= 2 && c < 0x0ffffff8; c = next_cluster(c)) {
    for (UINTN s = 0; s < bpb.sectors_cluster; ++s) {
      EFI_STATUS st = ata_read(cluster_lba(c) + s, sec);
      if (EFI_ERROR(st))
        return st;
      for (UINTN off = 0; off < 512; off += 32) {
        UINT8 *e = sec + off;
        if (e[0] == 0)
          return EFI_NOT_FOUND;
        if (e[0] == 0xe5 || e[11] == 0x0f || (e[11] & 8))
          continue;
        BOOLEAN match = TRUE;
        for (UINTN i = 0; i < 11; ++i)
          if ((CHAR8)e[i] != wanted[i])
            match = FALSE;
        if (!match)
          continue;
        *cluster = ((UINT32) * (UINT16 *)(e + 20) << 16) | *(UINT16 *)(e + 26);
        *size = *(UINT32 *)(e + 28);
        *directory = (e[11] & 0x10) != 0;
        return EFI_SUCCESS;
      }
    }
  }
  return EFI_NOT_FOUND;
}

static LegacyFile *new_file(void) {
  for (UINTN i = 0; i < 12; ++i)
    if (!files[i].allocated) {
      files[i].allocated = TRUE;
      return &files[i];
    }
  return NULL;
}

static EFI_STATUS EFIAPI file_close(EFI_FILE_PROTOCOL *p) {
  LegacyFile *f = (LegacyFile *)p;
  if (f != &root)
    f->allocated = FALSE;
  return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI file_open(EFI_FILE_PROTOCOL *p, EFI_FILE_PROTOCOL **out,
                                   CHAR16 *path, UINT64 mode, UINT64 attrs) {
  (void)p;
  (void)attrs;
  if (!out || mode != EFI_FILE_MODE_READ)
    return EFI_UNSUPPORTED;
  UINT32 c = bpb.root_cluster, size = 0;
  BOOLEAN dir = TRUE;
  CHAR16 *q = path;
  while (*q == '\\' || *q == '/')
    ++q;
  while (*q) {
    CHAR16 *start = q;
    while (*q && *q != '\\' && *q != '/')
      ++q;
    EFI_STATUS s = find_entry(c, start, q - start, &c, &size, &dir);
    if (EFI_ERROR(s))
      return s;
    while (*q == '\\' || *q == '/')
      ++q;
  }
  LegacyFile *f = new_file();
  if (!f)
    return EFI_OUT_OF_RESOURCES;
  f->cluster = c;
  f->size = size;
  f->position = 0;
  f->directory = dir;
  *out = &f->proto;
  return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI file_read(EFI_FILE_PROTOCOL *p, UINTN *amount, VOID *buffer) {
  LegacyFile *f = (LegacyFile *)p;
  if (!amount || !buffer || f->directory)
    return EFI_UNSUPPORTED;
  UINTN want = *amount;
  if (want > f->size - f->position)
    want = f->size - f->position;
  UINTN done = 0;
  UINT8 sec[512];
  UINT32 c = f->cluster;
  UINT32 skip = f->position / (512 * bpb.sectors_cluster);
  while (skip--)
    c = next_cluster(c);
  UINTN within = f->position % (512 * bpb.sectors_cluster);
  while (done < want && c < 0x0ffffff8) {
    for (UINTN s = within / 512; s < bpb.sectors_cluster && done < want; ++s) {
      EFI_STATUS st = ata_read(cluster_lba(c) + s, sec);
      if (EFI_ERROR(st))
        return st;
      UINTN o = within % 512, n = 512 - o;
      if (n > want - done)
        n = want - done;
      for (UINTN i = 0; i < n; ++i)
        ((UINT8 *)buffer)[done + i] = sec[o + i];
      done += n;
      within = 0;
    }
    c = next_cluster(c);
  }
  f->position += done;
  *amount = done;
  return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI file_get_info(EFI_FILE_PROTOCOL *p, EFI_GUID *type,
                                       UINTN *amount, VOID *buffer) {
  LegacyFile *f = (LegacyFile *)p;
  if (!guid_equal_local(type, &file_info_guid))
    return EFI_UNSUPPORTED;
  UINTN need = SIZE_OF_EFI_FILE_INFO + 2;
  if (!buffer || *amount < need) {
    *amount = need;
    return EFI_BUFFER_TOO_SMALL;
  }
  EFI_FILE_INFO *i = buffer;
  for (UINTN n = 0; n < need; ++n)
    ((UINT8 *)i)[n] = 0;
  i->Size = need;
  i->FileSize = f->size;
  i->PhysicalSize = f->size;
  i->Attribute = f->directory ? EFI_FILE_DIRECTORY : EFI_FILE_READ_ONLY;
  *amount = need;
  return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI open_volume(EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *self,
                                     EFI_FILE_PROTOCOL **out) {
  (void)self;
  root.position = 0;
  *out = &root.proto;
  return EFI_SUCCESS;
}

static void init_proto(LegacyFile *f) {
  f->proto.Revision = EFI_FILE_PROTOCOL_REVISION;
  f->proto.Open = file_open;
  f->proto.Close = file_close;
  f->proto.Read = file_read;
  f->proto.GetInfo = file_get_info;
}

EFI_STATUS legacy_storage_init(UINT32 drive) {
  (void)drive;
  UINT8 sec[512];
  EFI_STATUS s = ata_read(1, sec);
  if (EFI_ERROR(s))
    return s;
  if (CompareMem(sec, "EFI PART", 8) != 0)
    return EFI_UNSUPPORTED;

  UINT64 last_block = 0;
  for (UINTN i = 0; i < 8; ++i)
    last_block |= (UINT64)sec[32 + i] << (i * 8);

  SetMem(&block_media, sizeof(block_media), 0);
  block_media.MediaId = 1;
  block_media.MediaPresent = TRUE;
  block_media.ReadOnly = TRUE;
  block_media.BlockSize = 512;
  block_media.IoAlign = 1;
  block_media.LastBlock = last_block;

  SetMem(&block_io, sizeof(block_io), 0);
  block_io.Revision = EFI_BLOCK_IO_PROTOCOL_REVISION;
  block_io.Media = &block_media;
  block_io.Reset = block_reset;
  block_io.ReadBlocks = block_read;
  block_io.WriteBlocks = block_write;
  block_io.FlushBlocks = block_flush;

  s = ata_read(FAT_LBA, sec);
  if (EFI_ERROR(s))
    return s;
  for (UINTN i = 0; i < sizeof(bpb); ++i)
    ((UINT8 *)&bpb)[i] = sec[i];
  if (bpb.bytes_sector != 512 || bpb.fat32 == 0)
    return EFI_UNSUPPORTED;
  fs.Revision = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_REVISION;
  fs.OpenVolume = open_volume;
  init_proto(&root);
  root.cluster = bpb.root_cluster;
  root.directory = TRUE;
  root.allocated = TRUE;
  for (UINTN i = 0; i < 12; ++i)
    init_proto(&files[i]);
  return EFI_SUCCESS;
}

EFI_HANDLE legacy_storage_handle(void) {
  return volume_handle;
}

EFI_STATUS legacy_storage_protocol(EFI_GUID *guid, VOID **out) {
  if (!guid || !out)
    return EFI_INVALID_PARAMETER;
  if (guid_equal_local(guid, &fs_guid)) {
    *out = &fs;
    return EFI_SUCCESS;
  }
  if (guid_equal_local(guid, &block_io_guid)) {
    *out = &block_io;
    return EFI_SUCCESS;
  }
  return EFI_UNSUPPORTED;
}
