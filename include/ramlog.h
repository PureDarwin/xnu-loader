#ifndef XNU_LOADER_RAMLOG_H
#define XNU_LOADER_RAMLOG_H

#include "common.h"

/*
 * A console for boards with no UART anyone can reach: every byte the loader
 * prints also goes into a ring in DRAM that survives a warm reset (PSCI
 * SYSTEM_RESET or a watchdog bite), and is read back from the next boot.
 * DRAM loses a few bits across the reset, so the ring is kept in
 * RAMLOG_COPIES copies and a reader takes the bitwise majority.
 *
 * Built in with -DXNU_LOADER_RAMLOG=<phys base>.
 *
 * Each copy, RAMLOG_COPY_SIZE bytes:
 *   u32 magic RAMLOG_MAGIC, u32 head (bytes ever written), u32 data size,
 *   u32 boot count, then the data ring: byte n lives at data[n % data size].
 */
#define RAMLOG_MAGIC     0x4c524450U /* "PDRL" */
#define RAMLOG_COPIES    8
#define RAMLOG_COPY_SIZE 0x10000
#define RAMLOG_HEADER    16
#define RAMLOG_SIZE      (RAMLOG_COPIES * RAMLOG_COPY_SIZE)

/* Progress marks: RAMLOG_COPIES u32 slots, RAMLOG_MARK_STRIDE apart, after the
 * rings. The entry stub writes 'P' '1' 'B' 'R'; kernel_arm64_main the later ones. */
#define RAMLOG_MARKS_OFFSET RAMLOG_SIZE
#define RAMLOG_MARK_STRIDE  0x1000
#define RAMLOG_TOTAL        (RAMLOG_SIZE + RAMLOG_COPIES * RAMLOG_MARK_STRIDE)

#if defined(XNU_LOADER_RAMLOG)
VOID ramlog_init(VOID);
VOID ramlog_putc(CHAR8 c);
VOID ramlog_mark(CHAR8 c);
#endif

#endif
