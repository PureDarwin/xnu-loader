#include "devtree.h"

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

static UINT8 *dt_write_prop(
    UINT8 *p,
    const CHAR8 *name,
    const VOID *data,
    UINT32 len)
{
  DeviceTreeNodeProperty *prop = (DeviceTreeNodeProperty *)p;
  UINTN i;

  for (i = 0; i < DT_MAX_NAME; i++) {
    prop->name[i] = name[i];
    if (name[i] == '\0')
      break;
  }
  for (; i < DT_MAX_NAME; i++)
    prop->name[i] = '\0';

  prop->length = len;
  p += sizeof(*prop);

  if (len != 0)
    CopyMem(p, data, len);

  p += dt_align4(len);
  return p;
}

EFI_STATUS dt_build_minimal(
    AppContext *ctx,
    VOID **out_blob,
    UINT32 *out_size,
    LowMemBuffer *out_buf,
    const CHAR8 *boot_args)
{
  EFI_STATUS status;
  UINT8 *buf;
  UINT8 *p;
  UINTN boot_args_len;
  UINTN total_size;

  static const CHAR8 root_name[] = "/";
  static const CHAR8 chosen_name[] = "chosen";
  static const CHAR8 defaults_name[] = "defaults";
  static const CHAR8 memory_map_name[] = "memory-map";
  static const CHAR8 empty_string[] = "";

  if (!ctx || !out_blob || !out_size || !out_buf)
    return EFI_INVALID_PARAMETER;

  if (!boot_args)
    boot_args = empty_string;

  boot_args_len = dt_ascii_len(boot_args) + 1;

  total_size =
      1024 +
      dt_align4(boot_args_len);

  status = lowmem_alloc_pages(
      ctx,
      total_size,
      EfiLoaderData,
      out_buf);
  if (EFI_ERROR(status))
    return status;

  buf = (UINT8 *)out_buf->ptr;
  p = buf;

  {
    DeviceTreeNode *node = (DeviceTreeNode *)p;
    node->nProperties = 1;
    node->nChildren = 2;
    p += sizeof(*node);

    p = dt_write_prop(p, "name", root_name, sizeof(root_name));
  }

  {
    DeviceTreeNode *node = (DeviceTreeNode *)p;
    node->nProperties = 2;
    node->nChildren = 1;
    p += sizeof(*node);

    p = dt_write_prop(p, "name", chosen_name, sizeof(chosen_name));
    p = dt_write_prop(p, "boot-args", boot_args, (UINT32)boot_args_len);
  }

  {
    DeviceTreeNode *node = (DeviceTreeNode *)p;
    node->nProperties = 1;
    node->nChildren = 0;
    p += sizeof(*node);

    p = dt_write_prop(p, "name", memory_map_name, sizeof(memory_map_name));
  }

  {
    DeviceTreeNode *node = (DeviceTreeNode *)p;
    node->nProperties = 1;
    node->nChildren = 0;
    p += sizeof(*node);

    p = dt_write_prop(p, "name", defaults_name, sizeof(defaults_name));
  }

  *out_blob = buf;
  *out_size = (UINT32)(p - buf);
  return EFI_SUCCESS;
}