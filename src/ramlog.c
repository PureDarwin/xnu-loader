/* The DRAM console described in ramlog.h. Runs before and after the MMU is on,
 * so it is built with -mstrict-align, and every store is cleaned to the point
 * of coherency at once: a reset does not write back dirty cache lines. */
#include "ramlog.h"

#if defined(XNU_LOADER_RAMLOG)

static BOOLEAN ramlog_ready;

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
  for (UINT32 i = 0; i < RAMLOG_COPIES; ++i) {
    volatile UINT32 *h = copy_base(i);
    UINT32 head = h[1];
    volatile UINT8 *slot = (volatile UINT8 *)h + RAMLOG_HEADER + head % (RAMLOG_COPY_SIZE - RAMLOG_HEADER);
    *slot = (UINT8)c;
    clean(slot);
    h[1] = head + 1;
    clean(h);
  }
  __asm__ volatile("dsb sy" : : : "memory");
}

#endif
