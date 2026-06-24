#include "devtree.h"
#include "console.h"

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

// Allocate zeroed node
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

  // Copy name
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

/*
 * Fixed physical page for the XNU flat device tree.
 *
 * We pin the DT at a known physical address (0x91000) using AllocateAddress.
 * This avoids the OVMF DEBUG heap's 0xAF free-fill / 0xAB alloc-fill / SetMem
 * zero patterns that were observed corrupting dynamically allocated DT pages.
 * The page is allocated once and never freed (pages=0 in the LowMemBuffer
 * prevents lowmem_free from calling FreePages on it).
 *
 * 0x91000 is within EfiConventionalMemory (the OVMF memory map shows
 * type=7 from 0x0 to 0x9e000) and is safely below XNU's HIB at 0x100000.
 */
#define DT_FIXED_PHYS ((EFI_PHYSICAL_ADDRESS)0x91000ULL)

EFI_STATUS dt_build_minimal(
    AppContext *ctx,
    VOID **out_blob,
    UINT32 *out_size,
    LowMemBuffer *out_buf,
    const CHAR8 *boot_args)
{
  EFI_STATUS status;
  UINTN args_len;
  UINTN padded_len;
  UINT8 *p;
  UINT32 u32;
  EFI_PHYSICAL_ADDRESS dt_addr;

  if (!ctx || !out_blob || !out_size || !out_buf || !boot_args)
    return EFI_INVALID_PARAMETER;

  args_len   = dt_ascii_len(boot_args) + 1;
  padded_len = dt_align4(args_len);

  dt_addr = DT_FIXED_PHYS;
  status = uefi_call_wrapper(ctx->bs->AllocatePages, 4,
      AllocateAddress, EfiLoaderData, 1, &dt_addr);
  if (EFI_ERROR(status) && status != EFI_NOT_FOUND)
    return status;

  p = (UINT8 *)(UINTN)DT_FIXED_PHYS;
  SetMem(p, EFI_PAGE_SIZE, 0);

  /* root: 1 property (name="device-tree"), 2 children (chosen + efi) */
  u32 = 1; CopyMem(p, &u32, 4); p += 4;
  u32 = 2; CopyMem(p, &u32, 4); p += 4;
  /* root "name" property: value="device-tree\0" (13 bytes, padded to 16) */
  SetMem(p, DT_MAX_NAME, 0); CopyMem(p, "name", 4); p += DT_MAX_NAME;
  u32 = 13; CopyMem(p, &u32, 4); p += 4;
  SetMem(p, 16, 0); CopyMem(p, "device-tree", 11); p += 16;

  /* chosen: 3 properties (name, boot-args, random-seed), 0 children */
  u32 = 3; CopyMem(p, &u32, 4); p += 4;
  u32 = 0; CopyMem(p, &u32, 4); p += 4;

  /*
   * XNU's FindChild() locates child nodes by comparing their "name"
   * property to the path component. Without "name" = "chosen", the
   * SecureDTLookupEntry(NULL, "/chosen") call always returns kError
   * and PE_get_random_seed() returns 0, triggering the PRNG panic.
   */
  SetMem(p, DT_MAX_NAME, 0);
  CopyMem(p, "name", 4);
  p += DT_MAX_NAME;
  u32 = 7;                    /* strlen("chosen") + 1 */
  CopyMem(p, &u32, 4); p += 4;
  SetMem(p, 8, 0);            /* align4(7) = 8 bytes */
  CopyMem(p, "chosen", 6);
  p += 8;

  /* property name: "boot-args", zero-padded to DT_MAX_NAME */
  SetMem(p, DT_MAX_NAME, 0);
  CopyMem(p, "boot-args", 9);
  p += DT_MAX_NAME;

  /* property length (XNU counts the null terminator) */
  u32 = (UINT32)args_len;
  CopyMem(p, &u32, 4); p += 4;

  /* property value: cmdline bytes + null, zero-padded to 4-byte boundary */
  SetMem(p, padded_len, 0);
  CopyMem(p, boot_args, args_len);
  p += padded_len;

  /* property name: "random-seed", zero-padded to DT_MAX_NAME */
  SetMem(p, DT_MAX_NAME, 0);
  CopyMem(p, "random-seed", 11);
  p += DT_MAX_NAME;

  /* property length: exactly 64 bytes required by bootseed_init_bootloader() */
  u32 = 64;
  CopyMem(p, &u32, 4); p += 4;

  /* 64 non-null entropy bytes; PE_get_random_seed() rejects all-zero seeds */
  {
    UINT8 i;
    for (i = 0; i < 64; i++)
      p[i] = (UINT8)(0x01 + i);
  }
  p += 64;

  /*
   * /efi node: XNU's tsc_init calls EFI_get_frequency() which looks up
   * "/efi/platform" in the device tree to find FSBFrequency.  Without it,
   * tsc_init panics ("EFI not supported!") before pmap_bootstrap is called,
   * triggering a recursive panic cascade (kvtophys -> pmap_find_pa(NULL)).
   *
   * FSBFrequency = 133,333,333 Hz (133 MHz reference clock), matching
   * BASE_NHM_CLOCK_SOURCE. With QEMU Penryn returning tscGranularity=4
   * from MSR IA32_PERF_STS, this gives a computed TSC of ~533 MHz.
   */
  /* efi node: 1 property (name="efi"), 1 child (platform) */
  u32 = 1; CopyMem(p, &u32, 4); p += 4;
  u32 = 1; CopyMem(p, &u32, 4); p += 4;
  /* property: name="name", value="efi" */
  SetMem(p, DT_MAX_NAME, 0); CopyMem(p, "name", 4); p += DT_MAX_NAME;
  u32 = 4; CopyMem(p, &u32, 4); p += 4;          /* strlen("efi")+1 = 4 */
  SetMem(p, 4, 0); CopyMem(p, "efi", 3); p += 4; /* align4(4) = 4 */

  /* platform node: 2 properties (name="platform", FSBFrequency=133333333), 0 children */
  u32 = 2; CopyMem(p, &u32, 4); p += 4;
  u32 = 0; CopyMem(p, &u32, 4); p += 4;
  /* property: name="name", value="platform" */
  SetMem(p, DT_MAX_NAME, 0); CopyMem(p, "name", 4); p += DT_MAX_NAME;
  u32 = 9; CopyMem(p, &u32, 4); p += 4;                   /* strlen("platform")+1 = 9 */
  SetMem(p, 12, 0); CopyMem(p, "platform", 8); p += 12;   /* align4(9) = 12 */
  /* property: name="FSBFrequency", value=133333333ULL (uint64_t) */
  SetMem(p, DT_MAX_NAME, 0); CopyMem(p, "FSBFrequency", 12); p += DT_MAX_NAME;
  u32 = 8; CopyMem(p, &u32, 4); p += 4;
  {
    UINT64 fsb = 133333333ULL;
    CopyMem(p, &fsb, 8); p += 8;
  }

  *out_blob = (VOID *)(UINTN)DT_FIXED_PHYS;
  *out_size = (UINT32)(p - (UINT8 *)(UINTN)DT_FIXED_PHYS);

  out_buf->ptr = (VOID *)(UINTN)DT_FIXED_PHYS;
  out_buf->phys = DT_FIXED_PHYS;
  out_buf->size = *out_size;
  out_buf->pages = 0;

  return EFI_SUCCESS;
}