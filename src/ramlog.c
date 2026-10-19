/* The DRAM console described in ramlog.h. Runs before and after the MMU is on,
 * so it is built with -mstrict-align, and every store is cleaned to the point
 * of coherency at once: a reset does not write back dirty cache lines. */
#include "ramlog.h"

#if defined(XNU_LOADER_RAMLOG)

static BOOLEAN ramlog_ready;
// the head is kept here and the ring takes whole aligned words only:
// tegra210 DRAM with the MMU off gave back a corrupted head after byte stores
static UINT32 ramlog_head, ramlog_word;

static volatile UINT32 *copy_base(UINT32 i) {
  return (volatile UINT32 *)(UINTN)(XNU_LOADER_RAMLOG + (UINT64)i * RAMLOG_COPY_SIZE);
}

static VOID clean(volatile VOID *p) {
  __asm__ volatile("dc civac, %0" : : "r"(p) : "memory");
}

VOID ramlog_init(VOID) {
  /* the boot count survives only as well as DRAM does: a hint, not a record */
  UINT32 boots = copy_base(0)[0] == RAMLOG_MAGIC ? copy_base(0)[3] + 1 : 1;

  for (UINT32 i = 0; i < RAMLOG_COPIES; ++i) {
    volatile UINT32 *h = copy_base(i);
    h[0] = RAMLOG_MAGIC;
    h[1] = 0;
    h[2] = RAMLOG_COPY_SIZE - RAMLOG_HEADER;
    h[3] = boots;
    clean(h);
  }
  ramlog_head = 0;
  __asm__ volatile("dsb sy" : : : "memory");
  ramlog_ready = TRUE;
}

VOID ramlog_mark(CHAR8 c) {
  for (UINT32 i = 0; i < RAMLOG_COPIES; ++i) {
    volatile UINT32 *m = (volatile UINT32 *)(UINTN)(XNU_LOADER_RAMLOG + RAMLOG_MARKS_OFFSET +
                                                    (UINT64)i * RAMLOG_MARK_STRIDE);
    *m = (UINT8)c;
    clean(m);
  }
  __asm__ volatile("dsb sy" : : : "memory");
}

VOID ramlog_putc(CHAR8 c) {
  if (!ramlog_ready)
    return;
  UINT32 off = ramlog_head % (RAMLOG_COPY_SIZE - RAMLOG_HEADER);
  if ((off & 3) == 0)
    ramlog_word = 0;
  ramlog_word |= (UINT32)(UINT8)c << ((off & 3) * 8);
  ++ramlog_head;
  for (UINT32 i = 0; i < RAMLOG_COPIES; ++i) {
    volatile UINT32 *h = copy_base(i);
    volatile UINT32 *slot = h + (RAMLOG_HEADER + (off & ~3U)) / 4;
    *slot = ramlog_word;
    clean(slot);
    h[1] = ramlog_head;
    clean(h);
  }
  __asm__ volatile("dsb sy" : : : "memory");
}

#endif
