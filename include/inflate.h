#ifndef XNU_LOADER_INFLATE_H
#define XNU_LOADER_INFLATE_H

#include "common.h"

/*
 * gzip (RFC 1952) around DEFLATE (RFC 1951), for boards that can only hand the
 * loader a compressed ramdisk (an lk2nd boot image's 64MB ramdisk slot).
 *
 * gzip_size: the uncompressed size from the trailer (ISIZE, so mod 2^32), or 0
 * when src is not a gzip stream.
 * gzip_inflate: decompresses src into dst, which must hold gzip_size bytes;
 * tick, when given, is called every few MB of output (a watchdog to pet).
 * Returns the bytes written, or 0 on a malformed or truncated stream.
 */
UINTN gzip_size(CONST UINT8 *src, UINTN src_len);
UINTN gzip_inflate(CONST UINT8 *src, UINTN src_len, UINT8 *dst, UINTN dst_len, VOID (*tick)(VOID));

#endif
