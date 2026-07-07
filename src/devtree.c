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
  uefi_call_wrapper(ctx->bs->AllocatePool, 3, EfiBootServicesData, sizeof(DeviceTreeNode), &node);
  if (node)
    SetMem(node, sizeof(DeviceTreeNode), 0);
  return node;
}

DeviceTreeNodeProperty *dt_create_property(AppContext *ctx, const CHAR8 *name, VOID *data, UINT32 len) {
  DeviceTreeNodeProperty *prop = NULL;
  uefi_call_wrapper(ctx->bs->AllocatePool, 3, EfiBootServicesData, sizeof(DeviceTreeNodeProperty), &prop);
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
    uefi_call_wrapper(ctx->bs->AllocatePool, 3, EfiBootServicesData, len, &prop->value);
    if (prop->value)
      CopyMem(prop->value, data, len);
  }

  return prop;
}

VOID dt_add_property(AppContext *ctx, DeviceTreeNode *node, DeviceTreeNodeProperty *prop) {
  DeviceTreeNodeProperty **newProps = NULL;
  uefi_call_wrapper(ctx->bs->AllocatePool, 3, EfiBootServicesData, sizeof(DeviceTreeNodeProperty*) * (node->nProperties + 1), &newProps);
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
  uefi_call_wrapper(ctx->bs->AllocatePool, 3, EfiBootServicesData, sizeof(DeviceTreeNode*) * (parent->nChildren + 1), &newChildren);
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
    BOOLEAN got_boot_volume_uuid = FALSE;

    if (!got_uuid) {
      got_boot_volume_uuid = dp_get_partition_uuid(ctx, uuid_str);
    }

    if (!got_uuid && !got_boot_volume_uuid) {
      CHAR8 fallback[] = "dd5c6498-90d9-4b35-a95f-1944ebc01791";
      for (UINTN _i = 0; _i < 37; _i++) uuid_str[_i] = fallback[_i];
    }
    dt_prop_str(ctx, chosen, "boot-uuid", uuid_str);
    dt_prop_str(ctx, chosen, "apfs-preboot-uuid", uuid_str);

    /* root-matching: UUID-based only for an explicit root UUID.  Otherwise use
     * a generic whole-media match so we do not pin root discovery to the ESP. */
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
      if (!EFI_ERROR(rng->GetRNG(rng, NULL, 64, seed)))
        got_rng = TRUE;
    }

    if (!got_rng) {
      /* Fallback: xorshift64 seeded from TSC */
      UINT64 state;
      __asm__ volatile ("rdtsc; shlq $32,%%rdx; orq %%rdx,%%rax" : "=a"(state) :: "rdx");
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

  /* booter-build-info: from boot.efi 495.140.2 binary */
  {
    static const CHAR8 binfo[] =
      "BUILD-INFO[300]:{\"DisplayName\":\"boot.efi\","
      "\"DisplayVersion\":\"495.140.2~49\","
      "\"RecordUuid\":\"E1825ECA-9168-4C33-BEFA-0E978038B7D4\","
      "\"BuildTime\":\"2022-08-29T05:07:17-0700\","
      "\"ProjectName\":\"efiboot\","
      "\"ProductName\":\"boot.efi\","
      "\"SourceVersion\":\"495.140.2\","
      "\"BuildVersion\":\"49\","
      "\"BuildConfiguration\":\"Release\","
      "\"BuildType\":\"Official\"}";
    dt_prop_str(ctx, chosen, "booter-build-info", binfo);
  }

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

  /*
   * FSBFrequency: Ivy Bridge-E (Xeon E5 v2, Mac Pro 6,1) uses a 100 MHz BCLK.
   * XNU's tsc_init reads this and uses it with MSR_IA32_PERF_STATUS to compute
   * the TSC frequency.
   */
  dt_prop_u64(ctx, platform, "FSBFrequency", 100000000ULL);

  /*
   * TSCFrequency: used by our patched tsc_init as a fallback when
   * MSR_PLATFORM_INFO returns 0 (QEMU TCG does not emulate this MSR).
   * 3.0 GHz nominal for Xeon E5 v2 / Mac Pro 6,1; ignored on real
   * hardware where the MSR yields a non-zero ratio.
   */
  dt_prop_u64(ctx, platform, "TSCFrequency", 3000000000ULL);

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
  DeviceTreeNode *cfg_tbl = dt_create_node(ctx);
  dt_prop_str(ctx, cfg_tbl, "name", "configuration-table");

  static const EFI_GUID acpi20_guid = ACPI_20_TABLE_GUID;
  static const EFI_GUID acpi_guid   = ACPI_TABLE_GUID;

  for (UINTN ci = 0; ci < ctx->st->NumberOfTableEntries; ci++) {
    EFI_CONFIGURATION_TABLE *entry = &ctx->st->ConfigurationTable[ci];

    DeviceTreeNode *child = dt_create_node(ctx);

    /* name: GUID formatted as hex string (boot.efi uses sub_B055 for this) */
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
    /* Use the conventional-memory copy if boot.c already allocated it
     * (rt_table_phys = tbl_phys+0x200); fall back to the original pointer
     * only if the copy failed. XNU's physmap excludes EFI runtime pages after
     * pmap_bootstrap, so using the original causes a fault on first access. */
    UINT64 rt_phys = rt_table_phys
        ? rt_table_phys
        : (UINT64)(UINTN)(ctx->st->RuntimeServices);
    dt_prop_u64(ctx, rt_svcs, "table", rt_phys);
  }

  DeviceTreeNode *kcompat = dt_create_node(ctx);
  dt_prop_str(ctx, kcompat, "name", "kernel-compatibility");
  dt_prop_u32(ctx, kcompat, "x86_64", 1);

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

  UINT32 used = dt_flatten_node(root, dt_base);
  if (used > dt_size) {
    log_info(L"DT: overflow %u > %u\n", used, (UINT32)dt_size);
    uefi_call_wrapper(ctx->bs->FreePages, 2, dt_addr, 2);
    return EFI_BUFFER_TOO_SMALL;
  }

  log_info(L"DT: built %u bytes\n", used);

  *out_blob = (VOID *)(UINTN)dt_addr;
  *out_size = used;

  out_buf->ptr   = (VOID *)(UINTN)dt_addr;
  out_buf->phys  = dt_addr;
  out_buf->size  = used;
  out_buf->pages = 2;

  return EFI_SUCCESS;
}
