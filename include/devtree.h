#ifndef DEVTREE_H
#define DEVTREE_H

#include "common.h"
#include "app.h"
#include "lowmem.h"

#define DT_MAX_NAME 32

typedef struct DeviceTreeNode {
  UINT32 nProperties;
  UINT32 nChildren;
} DeviceTreeNode;

typedef struct DeviceTreeNodeProperty {
  CHAR8  name[DT_MAX_NAME];
  UINT32 length;
} DeviceTreeNodeProperty;

EFI_STATUS dt_build_minimal(
    AppContext *ctx,
    VOID **out_blob,
    UINT32 *out_size,
    LowMemBuffer *out_buf,
    const CHAR8 *boot_args);

#endif