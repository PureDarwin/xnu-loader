#include "devtree.h"
#include "console.h"
#include <efiprot.h>

#ifndef EFI_RNG_PROTOCOL_GUID
#define EFI_RNG_PROTOCOL_GUID \
  { 0x3152bca5, 0xeade, 0x433d, { 0x86, 0x2e, 0xc0, 0x1c, 0xdc, 0x29, 0x1f, 0x44 }}
typedef struct _EFI_RNG_PROTOCOL {
  EFI_STATUS (EFIAPI *GetInfo)(struct _EFI_RNG_PROTOCOL *This,
      UINTN *AlgorithmListSize, EFI_GUID *AlgorithmList);
  EFI_STATUS (EFIAPI *GetRNG)(struct _EFI_RNG_PROTOCOL *This,
      EFI_GUID *Algorithm, UINTN ValueLength, UINT8 *Value);
} EFI_RNG_PROTOCOL;
#endif

static UINTN dt_align4(UINTN v) {
  return (v + 3) & ~((UINTN)3);
}

static UINTN dt_ascii_len(const CHAR8 *s) {
  UINTN n = 0;
  if (!s)
    return 0;
  while (s[n] != '\0')
    n++;
  return n;
}

static UINTN dt_wcs_len(const CHAR16 *s) {
  UINTN n = 0;
  if (!s)
    return 0;
  while (s[n] != 0)
    n++;
  return n;
}

DeviceTreeNode *dt_create_node(AppContext *ctx) {
  DeviceTreeNode *node = NULL;
  uefi_call_wrapper(ctx->bs->AllocatePool, 3, EfiBootServicesData, sizeof(DeviceTreeNode), (VOID **)&node);
  if (node)
    SetMem(node, sizeof(DeviceTreeNode), 0);
  return node;
}

DeviceTreeNodeProperty *dt_create_property(AppContext *ctx, const CHAR8 *name, VOID *data, UINT32 len) {
  DeviceTreeNodeProperty *prop = NULL;
  uefi_call_wrapper(ctx->bs->AllocatePool, 3, EfiBootServicesData, sizeof(DeviceTreeNodeProperty), (VOID **)&prop);
  if (!prop)
    return NULL;

  SetMem(prop, sizeof(DeviceTreeNodeProperty), 0);

  UINTN n = dt_ascii_len(name);
  if (n >= DT_MAX_NAME)
    n = DT_MAX_NAME - 1;
  CopyMem(prop->name, name, n);
  prop->name[n] = 0;

  prop->length = len;
  if (len && data) {
    uefi_call_wrapper(ctx->bs->AllocatePool, 3, EfiBootServicesData, len, (VOID **)&prop->value);
    if (prop->value)
      CopyMem(prop->value, data, len);
  }

  return prop;
}

VOID dt_add_property(AppContext *ctx, DeviceTreeNode *node, DeviceTreeNodeProperty *prop) {
  DeviceTreeNodeProperty **newProps = NULL;
  uefi_call_wrapper(ctx->bs->AllocatePool, 3, EfiBootServicesData, sizeof(DeviceTreeNodeProperty*) * (node->nProperties + 1), (VOID **)&newProps);
  if (!newProps)
    return;

  if (node->properties) {
    CopyMem(newProps, node->properties, sizeof(DeviceTreeNodeProperty*) * node->nProperties);
    uefi_call_wrapper(ctx->bs->FreePool, 1, node->properties);
  }

  newProps[node->nProperties++] = prop;
  node->properties = newProps;
}

VOID dt_add_child(AppContext *ctx, DeviceTreeNode *parent, DeviceTreeNode *child) {
  DeviceTreeNode **newChildren = NULL;
  uefi_call_wrapper(ctx->bs->AllocatePool, 3, EfiBootServicesData, sizeof(DeviceTreeNode*) * (parent->nChildren + 1), (VOID **)&newChildren);
  if (!newChildren)
    return;

  if (parent->children) {
    CopyMem(newChildren, parent->children, sizeof(DeviceTreeNode*) * parent->nChildren);
    uefi_call_wrapper(ctx->bs->FreePool, 1, parent->children);
  }

  newChildren[parent->nChildren++] = child;
  parent->children = newChildren;
}

UINT32 dt_flatten_node(DeviceTreeNode *node, UINT8 *buf) {
  UINT32 offset = 0;

  CopyMem(buf + offset, &node->nProperties, sizeof(UINT32)); offset += sizeof(UINT32);
  CopyMem(buf + offset, &node->nChildren, sizeof(UINT32)); offset += sizeof(UINT32);

  for (UINT32 i = 0; i < node->nProperties; i++) {
    DeviceTreeNodeProperty *p = node->properties[i];
    UINTN padded = dt_align4(p->length);
    CopyMem(buf + offset, p->name, DT_MAX_NAME); offset += DT_MAX_NAME;
    CopyMem(buf + offset, &p->length, sizeof(UINT32)); offset += sizeof(UINT32);
    if (p->length && p->value)
      CopyMem(buf + offset, p->value, p->length);
    SetMem(buf + offset + p->length, padded - p->length, 0);
    offset += padded;
  }

  for (UINT32 i = 0; i < node->nChildren; i++) {
    offset += dt_flatten_node(node->children[i], buf + offset);
  }

  return offset;
}

static void dt_prop(AppContext *ctx, DeviceTreeNode *node, const CHAR8 *name, VOID *data, UINT32 len) {
  DeviceTreeNodeProperty *p = dt_create_property(ctx, name, data, len);
  if (p)
    dt_add_property(ctx, node, p);
}

static void dt_prop_str(AppContext *ctx, DeviceTreeNode *node, const CHAR8 *name, const CHAR8 *val) {
  UINT32 len = (UINT32)(dt_ascii_len(val) + 1);
  dt_prop(ctx, node, name, (VOID *)val, len);
}

static void dt_prop_u32(AppContext *ctx, DeviceTreeNode *node, const CHAR8 *name, UINT32 val) {
  dt_prop(ctx, node, name, &val, 4);
}

static void dt_prop_u64(AppContext *ctx, DeviceTreeNode *node, const CHAR8 *name, UINT64 val) {
  dt_prop(ctx, node, name, &val, 8);
}

#if defined(__x86_64__)
static UINT64 rdtsc64_raw(void) {
  UINT32 lo, hi;
  __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
  return ((UINT64)hi << 32) | lo;
}
#elif defined(__aarch64__)
/* Real, not a stub: CNTVCT_EL0 is arm64's equivalent free-running counter
 * (its rate comes from CNTFRQ_EL0, unlike TSC's calibration dance below,
 * but the raw-read shape matches what callers here want). */
static UINT64 rdtsc64_raw(void) {
  UINT64 v;
  __asm__ volatile ("mrs %0, cntvct_el0" : "=r"(v));
  return v;
}
#else
#error "devtree.c: unsupported architecture"
#endif

static UINT64 estimate_tsc_frequency(AppContext *ctx, UINT64 *initial_tsc) {
  const UINTN usec = 20000;
  UINT64 start = rdtsc64_raw();
  uefi_call_wrapper(ctx->bs->Stall, 1, usec);
  UINT64 end = rdtsc64_raw();

  if (initial_tsc)
    *initial_tsc = start;
  if (end <= start)
    return 0;

  return ((end - start) * 1000000ULL) / usec;
}

/*
 * Fixed physical page for the XNU flat device tree.
 *
 * Pinned at a known physical address (below 0xF0000) using AllocateMaxAddress.
 * Must survive ExitBootServices and not be in OVMF's DEBUG-fill zone.
 */

/* Format a 16-byte EFI GUID (LE Data1/2/3, BE Data4) as a UUID string.
 * out37 must be at least 37 bytes. */
static void fmt_guid_uuid(const UINT8 *g, CHAR8 *out37) {
  static const CHAR8 H[] = "0123456789ABCDEF";
  UINTN i = 0;
  /* Data1: 4 LE bytes, printed big-endian */
  for (int b = 3; b >= 0; b--) { out37[i++]=H[g[b]>>4]; out37[i++]=H[g[b]&0xF]; }
  out37[i++] = '-';
  /* Data2: 2 LE bytes */
  out37[i++]=H[g[5]>>4]; out37[i++]=H[g[5]&0xF];
  out37[i++]=H[g[4]>>4]; out37[i++]=H[g[4]&0xF];
  out37[i++] = '-';
  /* Data3: 2 LE bytes */
  out37[i++]=H[g[7]>>4]; out37[i++]=H[g[7]&0xF];
  out37[i++]=H[g[6]>>4]; out37[i++]=H[g[6]&0xF];
  out37[i++] = '-';
  /* Data4[0..1] */
  out37[i++]=H[g[8]>>4]; out37[i++]=H[g[8]&0xF];
  out37[i++]=H[g[9]>>4]; out37[i++]=H[g[9]&0xF];
  out37[i++] = '-';
  /* Data4[2..7] */
  for (int b = 10; b < 16; b++) { out37[i++]=H[g[b]>>4]; out37[i++]=H[g[b]&0xF]; }
  out37[i] = '\0';
}

static BOOLEAN is_uuid_char(CHAR8 c) {
  return (c >= '0' && c <= '9') ||
         (c >= 'a' && c <= 'f') ||
         (c >= 'A' && c <= 'F') ||
         c == '-';
}

static BOOLEAN boot_arg_get_uuid(const CHAR8 *boot_args, const CHAR8 *key, CHAR8 uuid_str[37]) {
  UINTN key_len = 0;
  while (key[key_len]) key_len++;

  for (const CHAR8 *p = boot_args; p && *p; p++) {
    if (p != boot_args && p[-1] != ' ')
      continue;

    UINTN i = 0;
    while (i < key_len && p[i] == key[i]) i++;
    if (i != key_len || p[i] != '=')
      continue;

    p += key_len + 1;
    for (i = 0; i < 36; i++) {
      if (!is_uuid_char(p[i]))
        return FALSE;
      uuid_str[i] = p[i];
    }
    if (p[36] && p[36] != ' ')
      return FALSE;
    uuid_str[36] = '\0';
    return TRUE;
  }

  return FALSE;
}

static BOOLEAN boot_arg_get_value(const CHAR8 *boot_args, const CHAR8 *key,
                                  CHAR8 *out, UINTN out_size) {
  if (!boot_args || !key || !out || out_size == 0)
    return FALSE;

  UINTN key_len = 0;
  while (key[key_len]) key_len++;

  for (const CHAR8 *p = boot_args; *p; p++) {
    if (p != boot_args && p[-1] != ' ')
      continue;

    UINTN i = 0;
    while (i < key_len && p[i] == key[i]) i++;
    if (i != key_len || p[i] != '=')
      continue;

    p += key_len + 1;
    UINTN n = 0;
    while (p[n] && p[n] != ' ' && n + 1 < out_size) {
      out[n] = p[n];
      n++;
    }
    out[n] = '\0';
    return n != 0;
  }

  return FALSE;
}

typedef struct {
  UINT32 state[4];
  UINT64 bit_count;
  UINT8 buffer[64];
} Md5Context;

static UINT32 md5_rol(UINT32 x, UINT32 n) {
  return (x << n) | (x >> (32 - n));
}

static UINT32 rd32le(const UINT8 *p) {
  return (UINT32)p[0] | ((UINT32)p[1] << 8) |
         ((UINT32)p[2] << 16) | ((UINT32)p[3] << 24);
}

static UINT64 rd64le(const UINT8 *p) {
  return (UINT64)rd32le(p) | ((UINT64)rd32le(p + 4) << 32);
}

static UINT16 rd16be(const UINT8 *p) {
  return ((UINT16)p[0] << 8) | (UINT16)p[1];
}

static void wr32le(UINT8 *p, UINT32 v) {
  p[0] = (UINT8)v;
  p[1] = (UINT8)(v >> 8);
  p[2] = (UINT8)(v >> 16);
  p[3] = (UINT8)(v >> 24);
}

static void md5_transform(UINT32 state[4], const UINT8 block[64]) {
  static const UINT32 k[64] = {
    0xd76aa478U,0xe8c7b756U,0x242070dbU,0xc1bdceeeU,
    0xf57c0fafU,0x4787c62aU,0xa8304613U,0xfd469501U,
    0x698098d8U,0x8b44f7afU,0xffff5bb1U,0x895cd7beU,
    0x6b901122U,0xfd987193U,0xa679438eU,0x49b40821U,
    0xf61e2562U,0xc040b340U,0x265e5a51U,0xe9b6c7aaU,
    0xd62f105dU,0x02441453U,0xd8a1e681U,0xe7d3fbc8U,
    0x21e1cde6U,0xc33707d6U,0xf4d50d87U,0x455a14edU,
    0xa9e3e905U,0xfcefa3f8U,0x676f02d9U,0x8d2a4c8aU,
    0xfffa3942U,0x8771f681U,0x6d9d6122U,0xfde5380cU,
    0xa4beea44U,0x4bdecfa9U,0xf6bb4b60U,0xbebfbc70U,
    0x289b7ec6U,0xeaa127faU,0xd4ef3085U,0x04881d05U,
    0xd9d4d039U,0xe6db99e5U,0x1fa27cf8U,0xc4ac5665U,
    0xf4292244U,0x432aff97U,0xab9423a7U,0xfc93a039U,
    0x655b59c3U,0x8f0ccc92U,0xffeff47dU,0x85845dd1U,
    0x6fa87e4fU,0xfe2ce6e0U,0xa3014314U,0x4e0811a1U,
    0xf7537e82U,0xbd3af235U,0x2ad7d2bbU,0xeb86d391U
  };
  static const UINT8 s[64] = {
    7,12,17,22, 7,12,17,22, 7,12,17,22, 7,12,17,22,
    5, 9,14,20, 5, 9,14,20, 5, 9,14,20, 5, 9,14,20,
    4,11,16,23, 4,11,16,23, 4,11,16,23, 4,11,16,23,
    6,10,15,21, 6,10,15,21, 6,10,15,21, 6,10,15,21
  };
  UINT32 m[16];
  UINT32 a = state[0], b = state[1], c = state[2], d = state[3];

  for (UINTN i = 0; i < 16; i++)
    m[i] = rd32le(block + i * 4);

  for (UINTN i = 0; i < 64; i++) {
    UINT32 f, g;
    if (i < 16) {
      f = (b & c) | ((~b) & d);
      g = (UINT32)i;
    } else if (i < 32) {
      f = (d & b) | ((~d) & c);
      g = (UINT32)((5 * i + 1) & 15);
    } else if (i < 48) {
      f = b ^ c ^ d;
      g = (UINT32)((3 * i + 5) & 15);
    } else {
      f = c ^ (b | (~d));
      g = (UINT32)((7 * i) & 15);
    }
    UINT32 tmp = d;
    d = c;
    c = b;
    b = b + md5_rol(a + f + k[i] + m[g], s[i]);
    a = tmp;
  }

  state[0] += a;
  state[1] += b;
  state[2] += c;
  state[3] += d;
}

static void md5_init(Md5Context *ctx) {
  ctx->bit_count = 0;
  ctx->state[0] = 0x67452301U;
  ctx->state[1] = 0xefcdab89U;
  ctx->state[2] = 0x98badcfeU;
  ctx->state[3] = 0x10325476U;
  SetMem(ctx->buffer, sizeof(ctx->buffer), 0);
}

static void md5_update(Md5Context *ctx, const UINT8 *data, UINTN len) {
  UINTN index = (UINTN)((ctx->bit_count >> 3) & 63);
  ctx->bit_count += (UINT64)len << 3;

  UINTN part_len = 64 - index;
  UINTN i = 0;
  if (len >= part_len) {
    CopyMem(&ctx->buffer[index], data, part_len);
    md5_transform(ctx->state, ctx->buffer);
    for (i = part_len; i + 63 < len; i += 64)
      md5_transform(ctx->state, data + i);
    index = 0;
  }
  if (i < len)
    CopyMem(&ctx->buffer[index], data + i, len - i);
}

static void md5_final(Md5Context *ctx, UINT8 digest[16]) {
  UINT8 bits[8];
  UINT8 pad[64];
  UINTN index = (UINTN)((ctx->bit_count >> 3) & 63);
  UINTN pad_len = (index < 56) ? (56 - index) : (120 - index);

  for (UINTN i = 0; i < 8; i++)
    bits[i] = (UINT8)(ctx->bit_count >> (8 * i));
  SetMem(pad, sizeof(pad), 0);
  pad[0] = 0x80;

  md5_update(ctx, pad, pad_len);
  md5_update(ctx, bits, sizeof(bits));

  for (UINTN i = 0; i < 4; i++)
    wr32le(digest + i * 4, ctx->state[i]);
}

static void fmt_uuid_bytes(const UINT8 uuid[16], CHAR8 *out37) {
  static const CHAR8 H[] = "0123456789ABCDEF";
  UINTN j = 0;
  for (UINTN i = 0; i < 16; i++) {
    if (i == 4 || i == 6 || i == 8 || i == 10)
      out37[j++] = '-';
    out37[j++] = H[uuid[i] >> 4];
    out37[j++] = H[uuid[i] & 0xF];
  }
  out37[j] = '\0';
}

static void hfs_uuid_from_finder_info(const UINT8 hfs_uuid[8], CHAR8 uuid_str[37]) {
  static const UINT8 fs_uuid_namespace[16] = {
    0xB3,0xE2,0x0F,0x39,0xF2,0x92,0x11,0xD6,
    0x97,0xA4,0x00,0x30,0x65,0x43,0xEC,0xAC
  };
  UINT8 uuid[16];
  Md5Context md5;

  if ((hfs_uuid[0] == 0 && hfs_uuid[1] == 0 && hfs_uuid[2] == 0 && hfs_uuid[3] == 0) ||
      (hfs_uuid[4] == 0 && hfs_uuid[5] == 0 && hfs_uuid[6] == 0 && hfs_uuid[7] == 0)) {
    SetMem(uuid, sizeof(uuid), 0);
  } else {
    md5_init(&md5);
    md5_update(&md5, fs_uuid_namespace, sizeof(fs_uuid_namespace));
    md5_update(&md5, hfs_uuid, 8);
    md5_final(&md5, uuid);
    uuid[6] = (uuid[6] & 0x0F) | 0x30;
    uuid[8] = (uuid[8] & 0x3F) | 0x80;
  }

  fmt_uuid_bytes(uuid, uuid_str);
}

typedef struct {
  UINT32 Revision;
  EFI_BLOCK_IO_MEDIA *MediaInfo;
  EFI_STATUS (EFIAPI *Reset)(VOID *This, BOOLEAN ExtendedVerification);
  EFI_STATUS (EFIAPI *ReadBlocks)(VOID *This, UINT32 MediaId, EFI_LBA LBA,
                                  UINTN BufferSize, VOID *Buffer);
  EFI_STATUS (EFIAPI *WriteBlocks)(VOID *This, UINT32 MediaId, EFI_LBA LBA,
                                   UINTN BufferSize, VOID *Buffer);
  EFI_STATUS (EFIAPI *FlushBlocks)(VOID *This);
} XnuBlockIoProtocol;

static BOOLEAN guid_eq_raw(const UINT8 *a, const UINT8 *b) {
  for (UINTN i = 0; i < 16; i++) {
    if (a[i] != b[i])
      return FALSE;
  }
  return TRUE;
}

static BOOLEAN read_hfs_uuid_from_blockio(XnuBlockIoProtocol *bio, CHAR8 uuid_str[37]) {
  static const UINT8 apple_hfs_guid[16] = {
    0x00,0x53,0x46,0x48,0x00,0x00,0xAA,0x11,
    0xAA,0x11,0x00,0x30,0x65,0x43,0xEC,0xAC
  };
  UINT8 block[512];
  UINT8 hfs_uuid[8];
  UINT64 part_lba = 0;
  UINT64 entries_lba;
  UINT32 entry_size;
  UINT32 entry_count;
  UINT32 media_id;

  if (!bio || !bio->MediaInfo || bio->MediaInfo->BlockSize != 512)
    return FALSE;

  media_id = bio->MediaInfo->MediaId;
  if (EFI_ERROR(uefi_call_wrapper(bio->ReadBlocks, 5,
                                  bio, media_id, 1, sizeof(block), block)))
    return FALSE;
  if (CompareMem(block, "EFI PART", 8) != 0)
    return FALSE;

  entries_lba = rd64le(block + 72);
  entry_count = rd32le(block + 80);
  entry_size = rd32le(block + 84);
  if (entry_size < 128 || entry_size > 512 || entry_count == 0)
    return FALSE;

  for (UINT32 idx = 0; idx < entry_count && idx < 128; idx++) {
    UINT64 lba = entries_lba + ((UINT64)idx * entry_size) / 512;
    UINTN off = (UINTN)(((UINT64)idx * entry_size) % 512);
    if (EFI_ERROR(uefi_call_wrapper(bio->ReadBlocks, 5,
                                    bio, media_id, lba, sizeof(block), block)))
      return FALSE;
    if (!guid_eq_raw(block + off, apple_hfs_guid))
      continue;
    part_lba = rd64le(block + off + 32);
    break;
  }

  if (part_lba == 0)
    return FALSE;

  if (EFI_ERROR(uefi_call_wrapper(bio->ReadBlocks, 5,
                                  bio, media_id, part_lba + 2, sizeof(block), block)))
    return FALSE;
  if (rd16be(block) != 0x482B && rd16be(block) != 0x4858)
    return FALSE;

  CopyMem(hfs_uuid, block + 104, sizeof(hfs_uuid));
  hfs_uuid_from_finder_info(hfs_uuid, uuid_str);
  log_info(L"DT: found HFS boot UUID %a at GPT LBA %lu\r\n", uuid_str, part_lba);
  return TRUE;
}

static BOOLEAN find_hfs_boot_uuid(AppContext *ctx, CHAR8 uuid_str[37]) {
  static EFI_GUID block_io_guid =
      { 0x964e5b21, 0x6459, 0x11d2, { 0x8e,0x39,0x00,0xa0,0xc9,0x69,0x72,0x3b } };
  EFI_HANDLE *handles = NULL;
  UINTN count = 0;
  EFI_STATUS status;

  status = uefi_call_wrapper(ctx->bs->LocateHandleBuffer, 5,
                             ByProtocol, &block_io_guid, NULL, &count, &handles);
  if (EFI_ERROR(status))
    return FALSE;

  for (UINTN i = 0; i < count; i++) {
    XnuBlockIoProtocol *bio = NULL;
    if (EFI_ERROR(uefi_call_wrapper(ctx->bs->HandleProtocol, 3,
                                    handles[i], &block_io_guid, (VOID **)&bio)))
      continue;
    if (read_hfs_uuid_from_blockio(bio, uuid_str)) {
      uefi_call_wrapper(ctx->bs->FreePool, 1, handles);
      return TRUE;
    }
  }

  uefi_call_wrapper(ctx->bs->FreePool, 1, handles);
  return FALSE;
}

/* Read the ext4 superblock UUID from the first Linux-filesystem GPT partition
 * on this block device. ext4's superblock lives at byte offset 1024 (LBA 2 of
 * the partition); s_magic (le16) is at offset 0x38 and s_uuid (a full 16-byte
 * UUID) at 0x68. The kernel-side Ext4FileSystemDriver reads the same s_uuid and
 * publishes boot-uuid-media when it equals this. */
static BOOLEAN read_ext4_uuid_from_blockio(XnuBlockIoProtocol *bio, CHAR8 uuid_str[37]) {
  static const UINT8 linux_fs_guid[16] = {
    0xAF,0x3D,0xC6,0x0F,0x83,0x84,0x72,0x47,
    0x8E,0x79,0x3D,0x69,0xD8,0x47,0x7D,0xE4
  };
  UINT8 block[512];
  UINT64 entries_lba;
  UINT32 entry_size, entry_count, media_id;
  UINT64 part_lba = 0;

  if (!bio || !bio->MediaInfo || bio->MediaInfo->BlockSize != 512)
    return FALSE;

  media_id = bio->MediaInfo->MediaId;
  if (EFI_ERROR(uefi_call_wrapper(bio->ReadBlocks, 5,
                                  bio, media_id, 1, sizeof(block), block)))
    return FALSE;
  if (CompareMem(block, "EFI PART", 8) != 0)
    return FALSE;

  entries_lba = rd64le(block + 72);
  entry_count = rd32le(block + 80);
  entry_size = rd32le(block + 84);
  if (entry_size < 128 || entry_size > 512 || entry_count == 0)
    return FALSE;

  for (UINT32 idx = 0; idx < entry_count && idx < 128; idx++) {
    UINT64 lba = entries_lba + ((UINT64)idx * entry_size) / 512;
    UINTN off = (UINTN)(((UINT64)idx * entry_size) % 512);
    if (EFI_ERROR(uefi_call_wrapper(bio->ReadBlocks, 5,
                                    bio, media_id, lba, sizeof(block), block)))
      return FALSE;
    if (!guid_eq_raw(block + off, linux_fs_guid))
      continue;
    part_lba = rd64le(block + off + 32);
    break;
  }

  if (part_lba == 0)
    return FALSE;

  /* superblock at partition byte 1024 == LBA part_lba + 2 */
  if (EFI_ERROR(uefi_call_wrapper(bio->ReadBlocks, 5,
                                  bio, media_id, part_lba + 2, sizeof(block), block)))
    return FALSE;
  if ((UINT16)(block[0x38] | ((UINT16)block[0x39] << 8)) != 0xEF53)
    return FALSE;

  fmt_uuid_bytes(block + 0x68, uuid_str);
  log_info(L"DT: found ext4 boot UUID %a at GPT LBA %lu\r\n", uuid_str, part_lba);
  return TRUE;
}

static BOOLEAN find_ext4_boot_uuid(AppContext *ctx, CHAR8 uuid_str[37]) {
  static EFI_GUID block_io_guid =
      { 0x964e5b21, 0x6459, 0x11d2, { 0x8e,0x39,0x00,0xa0,0xc9,0x69,0x72,0x3b } };
  EFI_HANDLE *handles = NULL;
  UINTN count = 0;

  if (EFI_ERROR(uefi_call_wrapper(ctx->bs->LocateHandleBuffer, 5,
                                  ByProtocol, &block_io_guid, NULL, &count, &handles)))
    return FALSE;

  for (UINTN i = 0; i < count; i++) {
    XnuBlockIoProtocol *bio = NULL;
    if (EFI_ERROR(uefi_call_wrapper(ctx->bs->HandleProtocol, 3,
                                    handles[i], &block_io_guid, (VOID **)&bio)))
      continue;
    if (read_ext4_uuid_from_blockio(bio, uuid_str)) {
      uefi_call_wrapper(ctx->bs->FreePool, 1, handles);
      return TRUE;
    }
  }

  uefi_call_wrapper(ctx->bs->FreePool, 1, handles);
  return FALSE;
}

/* Try to extract a GPT partition UUID from the boot volume's device path.
 * Looks for a HARDDRIVE_DEVICE_PATH (Type=4, SubType=1) with SignatureType=2.
 * Returns TRUE and fills uuid_str[37] on success. */
static BOOLEAN dp_get_partition_uuid(AppContext *ctx, CHAR8 uuid_str[37]) {
  if (!ctx->boot_volume) return FALSE;
  static EFI_GUID dp_guid = EFI_DEVICE_PATH_PROTOCOL_GUID;
  EFI_DEVICE_PATH *dp = NULL;
  if (EFI_ERROR(uefi_call_wrapper(ctx->bs->HandleProtocol, 3,
                                   ctx->boot_volume, &dp_guid, (VOID **)&dp)))
    return FALSE;
  EFI_DEVICE_PATH *n = dp;
  while (!(n->Type == 0x7F && n->SubType == 0xFF)) {
    UINT32 nlen = (UINT32)(n->Length[0] | ((UINT32)n->Length[1] << 8));
    /* HARDDRIVE_DEVICE_PATH: Type=4, SubType=1, Length=42 */
    if (n->Type == 4 && n->SubType == 1 && nlen >= 42) {
      UINT8 *hd = (UINT8 *)n;
      UINT8 sig_type = hd[41];
      if (sig_type == 2) { /* GPT GUID at hd[24..39] */
        fmt_guid_uuid(hd + 24, uuid_str);
        return TRUE;
      }
    }
    n = (EFI_DEVICE_PATH *)((UINT8 *)n + nlen);
  }
  return FALSE;
}

EFI_STATUS dt_build(
    AppContext *ctx,
    VOID **out_blob,
    UINT32 *out_size,
    LowMemBuffer *out_buf,
    const CHAR8 *boot_args,
    UINT64 rt_table_phys)
{
  if (!ctx || !out_blob || !out_size || !out_buf || !boot_args)
    return EFI_INVALID_PARAMETER;

  /* Allocate 8 KB for the DT blob in the fixed boot-info block (>= 0x100000 so
   * it survives XNU's pmap_lowmem_finalize; see boot.h XNU_BOOTINFO_BASE). */
  EFI_PHYSICAL_ADDRESS dt_addr = XNU_DEVTREE_PHYS;
  EFI_STATUS status = uefi_call_wrapper(ctx->bs->AllocatePages, 4,
      AllocateAddress,
      EfiLoaderData,
      2,
      &dt_addr);
  if (EFI_ERROR(status)) {
    log_info(L"DT: AllocatePages failed: %r\n", status);
    return status;
  }

  UINTN dt_size = EFI_PAGE_SIZE * 2;
  UINT8 *dt_base = (UINT8 *)(UINTN)dt_addr;
  SetMem(dt_base, dt_size, 0);

  DeviceTreeNode *chosen = dt_create_node(ctx);

  dt_prop_str(ctx, chosen, "name", "chosen");
  dt_prop_str(ctx, chosen, "boot-args", boot_args);

  /* boot-uuid / apfs-preboot-uuid: prefer an explicit root UUID.  The EFI
   * image is loaded from the ESP, so the boot-volume device path is usually
   * not the Darwin root volume and must not become a strict root match. */
  {
    CHAR8 uuid_str[37];
    BOOLEAN got_uuid = boot_arg_get_uuid(boot_args, "boot-uuid", uuid_str) ||
                       boot_arg_get_uuid(boot_args, "root-uuid", uuid_str);
    BOOLEAN got_hfs_uuid = FALSE;
    BOOLEAN got_ext4_uuid = FALSE;
    BOOLEAN got_boot_volume_uuid = FALSE;

    if (!got_uuid) {
      got_hfs_uuid = find_hfs_boot_uuid(ctx, uuid_str);
    }

    /* No HFS volume found: try an ext4 root (Ext4FileSystemDriver publishes
     * boot-uuid-media for it, mirroring AppleFileSystemDriver for HFS). */
    if (!got_uuid && !got_hfs_uuid) {
      got_ext4_uuid = find_ext4_boot_uuid(ctx, uuid_str);
    }

    if (!got_uuid && !got_hfs_uuid && !got_ext4_uuid) {
      got_boot_volume_uuid = dp_get_partition_uuid(ctx, uuid_str);
    }

    if (!got_uuid && !got_hfs_uuid && !got_ext4_uuid && !got_boot_volume_uuid) {
      CHAR8 fallback[] = "dd5c6498-90d9-4b35-a95f-1944ebc01791";
      for (UINTN _i = 0; _i < 37; _i++) uuid_str[_i] = fallback[_i];
    }
    dt_prop_str(ctx, chosen, "boot-uuid", uuid_str);
    dt_prop_str(ctx, chosen, "apfs-preboot-uuid", uuid_str);

    /* root-matching: UUID-based only for an explicit root UUID.  For an HFS
     * filesystem UUID discovered from disk, let IOKitBSDInit publish boot-uuid
     * and let AppleFileSystemDriver publish boot-uuid-media after it verifies
     * the filesystem UUID. IOMedia's UUID is the GPT partition UUID, not the
     * HFS filesystem UUID. */
    if (got_uuid) {
      CHAR8 rm[256];
      CHAR8 *p = rm;
      /* Build: <dict><key>IOProviderClass</key><string>IOMedia</string>
                <key>IOPropertyMatch</key><dict><key>UUID</key><string>UUID</string></dict></dict> */
      static const CHAR8 rm_a[] = "<dict><key>IOProviderClass</key><string>IOMedia</string>"
                                   "<key>IOPropertyMatch</key><dict><key>UUID</key><string>";
      static const CHAR8 rm_b[] = "</string></dict></dict>";
      UINTN la = 0; while (rm_a[la]) la++;
      UINTN lb = 0; while (rm_b[lb]) lb++;
      for (UINTN _i = 0; _i < la; _i++) *p++ = rm_a[_i];
      for (UINTN _i = 0; _i < 36; _i++) *p++ = uuid_str[_i];
      for (UINTN _i = 0; _i < lb; _i++) *p++ = rm_b[_i];
      *p++ = '\0';
      dt_prop_str(ctx, chosen, "root-matching", rm);
    } else {
      dt_prop_str(ctx, chosen, "root-matching",
          "<dict><key>IOProviderClass</key><string>IOMedia</string>"
          "<key>IOPropertyMatch</key><dict><key>Whole Media</key></dict></dict>");
    }
  }

  {
    CHAR8 volgroup_uuid[37];
    if (boot_arg_get_uuid(boot_args, "associated-volume-group", volgroup_uuid) ||
        boot_arg_get_uuid(boot_args, "volgroup-uuid", volgroup_uuid)) {
      dt_prop_str(ctx, chosen, "associated-volume-group", volgroup_uuid);
    }
  }

  {
    CHAR8 boot_objects_path[256];
    if (boot_arg_get_value(boot_args, "boot-objects-path",
                           boot_objects_path, sizeof(boot_objects_path))) {
      dt_prop_str(ctx, chosen, "boot-objects-path", boot_objects_path);
    }
  }

  /* boot-file: path to the kernel on the boot volume. */
  dt_prop_str(ctx, chosen, "boot-file", "\\EFI\\BOOT\\kernel");

  /* boot-device-path: full serialized EFI device path of the boot volume.
   * boot-file-path:   same path + FilePath media node for the kernel file.
   * Fall back to a 4-byte end-of-path node if the volume handle is absent. */
  {
    static const UINT8 devpath_end[4] = { 0x7F, 0xFF, 0x04, 0x00 };
    static EFI_GUID dp_guid = EFI_DEVICE_PATH_PROTOCOL_GUID;

    EFI_DEVICE_PATH *vol_dp = NULL;
    UINT32 vol_dp_len = 0;   /* bytes up to and including end node */

    if (ctx->boot_volume &&
        !EFI_ERROR(uefi_call_wrapper(ctx->bs->HandleProtocol, 3,
                                     ctx->boot_volume, &dp_guid,
                                     (VOID **)&vol_dp)) && vol_dp) {
      /* Measure length: walk nodes until end node, add 4 for the end node */
      EFI_DEVICE_PATH *n = vol_dp;
      while (!(n->Type == 0x7F && n->SubType == 0xFF)) {
        UINT32 nlen = (UINT32)(n->Length[0] | ((UINT32)n->Length[1] << 8));
        vol_dp_len += nlen;
        n = (EFI_DEVICE_PATH *)((UINT8 *)n + nlen);
      }
      vol_dp_len += 4; /* include end node */
    }

    /* boot-device-path */
    if (vol_dp_len >= 4) {
      dt_prop(ctx, chosen, "boot-device-path", (VOID *)vol_dp, vol_dp_len);
    } else {
      dt_prop(ctx, chosen, "boot-device-path", (VOID *)devpath_end, 4);
    }

    /* boot-file-path: volume path (minus its end node) + FilePath node + end */
    {
      static const CHAR16 kpath[] = L"\\EFI\\BOOT\\kernel";
      /* kpath has 16 chars + null = 17 * 2 = 34 bytes; node = 4 + 34 = 38 */
      UINT32 fp_node_len = (UINT32)(4 + (16 + 1) * 2);
      UINT32 vol_prefix  = (vol_dp_len >= 4) ? (vol_dp_len - 4) : 0;
      UINT32 file_dp_len = vol_prefix + fp_node_len + 4;

      UINT8 *file_dp_buf = NULL;
      if (!EFI_ERROR(uefi_call_wrapper(ctx->bs->AllocatePool, 3,
                                       EfiBootServicesData, file_dp_len,
                                       (VOID **)&file_dp_buf))) {
        /* Copy volume path (without its end node) */
        if (vol_prefix)
          CopyMem(file_dp_buf, vol_dp, vol_prefix);

        /* Build FilePath node at offset vol_prefix */
        UINT8 *fp = file_dp_buf + vol_prefix;
        fp[0] = 4;   /* Type: Media */
        fp[1] = 4;   /* SubType: File Path */
        fp[2] = (UINT8)(fp_node_len & 0xFF);
        fp[3] = (UINT8)(fp_node_len >> 8);
        CopyMem(fp + 4, kpath, (16 + 1) * 2);

        /* End node */
        UINT8 *ep = fp + fp_node_len;
        ep[0] = 0x7F; ep[1] = 0xFF; ep[2] = 0x04; ep[3] = 0x00;

        dt_prop(ctx, chosen, "boot-file-path", (VOID *)file_dp_buf, file_dp_len);
        uefi_call_wrapper(ctx->bs->FreePool, 1, file_dp_buf);
      } else {
        dt_prop(ctx, chosen, "boot-file-path", (VOID *)devpath_end, 4);
      }
    }
  }

  /* boot-kernelcache-adler32: Adler-32 of the prelinked kernel.
   * Zero when booting without a prelinked kernel cache. */
  {
    UINT32 adler = 0;
    dt_prop(ctx, chosen, "boot-kernelcache-adler32", &adler, 4);
  }

  /* 64 entropy bytes for PE_get_random_seed().
   * Use EFI_RNG_PROTOCOL if present, else mix TSC with a simple xorshift. */
  {
    UINT8 seed[64];
    static EFI_GUID rng_guid = EFI_RNG_PROTOCOL_GUID;
    EFI_RNG_PROTOCOL *rng = NULL;
    BOOLEAN got_rng = FALSE;

    if (!EFI_ERROR(uefi_call_wrapper(ctx->bs->LocateProtocol, 3,
                                     &rng_guid, NULL, (VOID **)&rng)) && rng) {
      /* MUST go through uefi_call_wrapper: firmware protocol methods use the
       * MS x64 ABI, our code is SysV. A direct rng->GetRNG(...) call passes
       * args in the wrong registers -> firmware faults. QEMU/OVMF has no RNG
       * protocol so this path only ever ran on real hardware (where it crashed
       * right after "found HFS boot UUID"). */
      if (!EFI_ERROR(uefi_call_wrapper(rng->GetRNG, 4, rng, NULL, 64, seed)))
        got_rng = TRUE;
    }

    if (!got_rng) {
      /* Fallback: xorshift64 seeded from the free-running counter
       * (TSC on x86_64, CNTVCT_EL0 on arm64 - see rdtsc64_raw() above). */
      UINT64 state = rdtsc64_raw();
      if (!state)
        state = 0xDEADBEEFCAFEBABEULL;
      for (UINTN si = 0; si < 64; si++) {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        seed[si] = (UINT8)state;
      }
    }

    dt_prop(ctx, chosen, "random-seed", seed, 64);
  }

  /* booter-name: boot.efi writes "boot.efi" (9 bytes incl null) */
  dt_prop_str(ctx, chosen, "booter-name", "boot.efi");

  static const CHAR8 binfo[] = "boot.efi";
  dt_prop_str(ctx, chosen, "booter-build-info", binfo);

  /* machine-signature: 4 bytes from FACS.HardwareSignature.
   * boot.efi reads this from the FACP ACPI table's FACS pointer.
   * OVMF's FACS hardware signature is 0. */
  {
    UINT32 sig = 0;
    dt_prop_u32(ctx, chosen, "machine-signature", sig);
  }

  /* IOScreenLockState / IOFDEUserMatched: boot.efi sets these on /chosen in
   * its handoff path from the CoreStorage/FileVault (FDE) unlock state.
   * IOKit (IOHibernateSystemPostWake / login window) reads them.  For a plain
   * un-encrypted boot with no FileVault the faithful values are
   * kIOScreenLockNoLock (1) and "no user matched" (0). */
  {
    UINT32 io_screen_lock_state = 1; /* kIOScreenLockNoLock */
    UINT32 io_fde_user_matched  = 0;
    dt_prop_u32(ctx, chosen, "IOScreenLockState", io_screen_lock_state);
    dt_prop_u32(ctx, chosen, "IOFDEUserMatched", io_fde_user_matched);
  }

  DeviceTreeNode *platform = dt_create_node(ctx);
  dt_prop_str(ctx, platform, "name", "platform");

  UINT64 initial_tsc = 0;
  UINT64 tsc_frequency = estimate_tsc_frequency(ctx, &initial_tsc);
  if (tsc_frequency == 0)
    tsc_frequency = 1500000000ULL;

  /*
   * FSBFrequency: Ivy Bridge-E (Xeon E5 v2, Mac Pro 6,1) uses a 100 MHz BCLK.
   * XNU's tsc_init reads this and uses it with MSR_IA32_PERF_STATUS to compute
   * the TSC frequency.
   */
  dt_prop_u64(ctx, platform, "FSBFrequency", 100000000ULL);

  /*
   * TSCFrequency/InitialTSC: used by PureDarwin's tsc_init on CPUs or
   * virtualized firmware paths where the legacy ratio MSRs are missing or
   * unsafe to touch.
   */
  dt_prop_u64(ctx, platform, "TSCFrequency", tsc_frequency);
  dt_prop_u64(ctx, platform, "InitialTSC", initial_tsc);
  log_info(L"TSC: initial=0x%lx estimated frequency=%lu Hz\r\n",
           initial_tsc, tsc_frequency);

  /*
   * system-id: 16-byte hardware UUID. Boot.efi reads this from the DT
   * (Apple firmware pre-populates it) or from EFI variable "p".
   * Use a plausible fixed UUID for QEMU (Mac Pro 6,1 style).
   */
  {
    UINT8 uuid[16] = {
      0xDE, 0xAD, 0xBE, 0xEF, 0xCA, 0xFE, 0x00, 0x01,
      0x80, 0x00, 0x00, 0x26, 0xB9, 0x58, 0x4C, 0x4A
    };
    dt_prop(ctx, platform, "system-id", uuid, 16);
  }

  /*
   * boot.efi iterates EFI_SYSTEM_TABLE.ConfigurationTable and creates one
   * child node per entry with "guid" (16 bytes) and "table" (8 bytes).
   * ACPI_20 and ACPI 1.0 entries also get an "alias" property.
   * XNU uses this to locate ACPI without scanning memory.
   */
  log_info(L"DT: config-table loop (%lu entries)\r\n",
           (UINT64)ctx->st->NumberOfTableEntries);
  DeviceTreeNode *cfg_tbl = dt_create_node(ctx);
  dt_prop_str(ctx, cfg_tbl, "name", "configuration-table");

  static const EFI_GUID acpi20_guid = ACPI_20_TABLE_GUID;
  static const EFI_GUID acpi_guid   = ACPI_TABLE_GUID;

  for (UINTN ci = 0; ci < ctx->st->NumberOfTableEntries; ci++) {
    EFI_CONFIGURATION_TABLE *entry = &ctx->st->ConfigurationTable[ci];

    DeviceTreeNode *child = dt_create_node(ctx);

    {
      CHAR8 gname[37];
      EFI_GUID *g = &entry->VendorGuid;
      UINT8 *b = (UINT8 *)g;
      /* Format as xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx */
      UINTN gi = 0;
      /* Data1 (4 bytes, big-endian display) */
      for (int j = 3; j >= 0; j--) { gname[gi++] = "0123456789abcdef"[b[j]>>4]; gname[gi++] = "0123456789abcdef"[b[j]&0xf]; }
      gname[gi++] = '-';
      for (int j = 5; j >= 4; j--) { gname[gi++] = "0123456789abcdef"[b[j]>>4]; gname[gi++] = "0123456789abcdef"[b[j]&0xf]; }
      gname[gi++] = '-';
      for (int j = 7; j >= 6; j--) { gname[gi++] = "0123456789abcdef"[b[j]>>4]; gname[gi++] = "0123456789abcdef"[b[j]&0xf]; }
      gname[gi++] = '-';
      for (int j = 8; j <= 9; j++) { gname[gi++] = "0123456789abcdef"[b[j]>>4]; gname[gi++] = "0123456789abcdef"[b[j]&0xf]; }
      gname[gi++] = '-';
      for (int j = 10; j <= 15; j++) { gname[gi++] = "0123456789abcdef"[b[j]>>4]; gname[gi++] = "0123456789abcdef"[b[j]&0xf]; }
      gname[gi] = '\0';
      dt_prop_str(ctx, child, "name", gname);
    }

    dt_prop(ctx, child, "guid", &entry->VendorGuid, 16);
    UINT64 tbl_addr = (UINT64)(UINTN)entry->VendorTable;
    dt_prop(ctx, child, "table", &tbl_addr, 8);

    if (CompareMem(&entry->VendorGuid, &acpi20_guid, 16) == 0)
      dt_prop_str(ctx, child, "alias", "ACPI_20");
    else if (CompareMem(&entry->VendorGuid, &acpi_guid, 16) == 0)
      dt_prop_str(ctx, child, "alias", "ACPI");

    dt_add_child(ctx, cfg_tbl, child);
  }
  log_info(L"DT: config-table done\r\n");

  DeviceTreeNode *rt_svcs = dt_create_node(ctx);
  dt_prop_str(ctx, rt_svcs, "name", "runtime-services");

  /*
   * table: physical address of the EFI runtime services table copy in
   * conventional memory (boot.c copies RT to tbl_phys+0x200 before EBS).
   * We don't know tbl_phys here, so store the original RT physical address,
   * XNU reads this to set up runtime calls and will use the SVAM-adjusted VA.
   * After SVAM: virtual = phys & 0x3FFFFFFF for runtime-flagged pages.
   */
  {
    UINT64 rt_phys = rt_table_phys
        ? rt_table_phys
        : (UINT64)(UINTN)(ctx->st->RuntimeServices);
    dt_prop_u64(ctx, rt_svcs, "table", rt_phys);
  }

  DeviceTreeNode *kcompat = dt_create_node(ctx);
  dt_prop_str(ctx, kcompat, "name", "kernel-compatibility");
#if defined(__x86_64__)
  dt_prop_u32(ctx, kcompat, "x86_64", 1);
#elif defined(__aarch64__)
  /* a real arm64/BCM2837 boot needs its own tree (FDT-derived board info,
   * mailbox-queried memory layout, no ACPI at all). This just lets the
   * kcompat marker match the arch actually compiling. */
  dt_prop_u32(ctx, kcompat, "arm64", 1);
#else
#error "devtree.c: unsupported architecture"
#endif

  DeviceTreeNode *efi = dt_create_node(ctx);
  dt_prop_str(ctx, efi, "name", "efi");

  /*
   * firmware-vendor: UTF-16 string from EFI System Table.
   * Length = (wcslen + 1) * 2 bytes (includes null terminator).
   */
  {
    CHAR16 *vendor = ctx->st->FirmwareVendor;
    UINTN wlen = dt_wcs_len(vendor);
    UINT32 blen = (UINT32)((wlen + 1) * 2);
    dt_prop(ctx, efi, "firmware-vendor", vendor, blen);
  }

  /* firmware-revision: 4-byte UINT32 from EFI System Table */
  dt_prop_u32(ctx, efi, "firmware-revision", ctx->st->FirmwareRevision);

  /* firmware-abi: "EFI64" for 64-bit mode (efiMode=64 in boot_args) */
  dt_prop_str(ctx, efi, "firmware-abi", "EFI64");

  dt_add_child(ctx, efi, platform);
  dt_add_child(ctx, efi, rt_svcs);
  dt_add_child(ctx, efi, kcompat);

  DeviceTreeNode *root = dt_create_node(ctx);
  dt_prop_str(ctx, root, "name", "device-tree");
  /* IOKit matches the platform driver (AppleI386GenericPlatform) on these two
   * properties being present on the root node. Without them the entire IOKit
   * driver cascade fails to start and XNU hangs waiting for the root device. */
  dt_prop_str(ctx, root, "compatible", "ACPI");
  dt_prop_str(ctx, root, "model", "ACPI");
  dt_add_child(ctx, root, chosen);
  dt_add_child(ctx, root, efi);

  log_info(L"DT: flattening into %lu-byte buffer\r\n", (UINT64)dt_size);
  UINT32 used = dt_flatten_node(root, dt_base);
  if (used > dt_size) {
    log_info(L"DT: overflow %u > %u\n", used, (UINT32)dt_size);
    uefi_call_wrapper(ctx->bs->FreePages, 2, dt_addr, 2);
    return EFI_BUFFER_TOO_SMALL;
  }

  log_info(L"DT: built %u bytes\n", used);

  *out_blob = (VOID *)(UINTN)dt_addr;
  *out_size = used;

  out_buf->ptr = (VOID *)(UINTN)dt_addr;
  out_buf->phys = dt_addr;
  out_buf->size = used;
  out_buf->pages = 2;

  return EFI_SUCCESS;
}
