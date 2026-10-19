/* DEFLATE inside gzip (include/inflate.h). Huffman codes decode through a 9-bit
 * table for the common short codes and the canonical-code ranges for the rest;
 * the stream is read LSB first through a 64-bit bit buffer. */
#include "inflate.h"

#define FAST_BITS 9
#define MAX_BITS  15

typedef struct {
  UINT16 fast[1 << FAST_BITS];     // symbol << 4 | length, 0 when the code is longer
  UINT16 first_code[MAX_BITS + 2]; // first canonical code of each length
  UINT32 max_code[MAX_BITS + 2];   // one past the last code of each length, left-aligned to 16 bits
  UINT16 first_symbol[MAX_BITS + 2];
  UINT8 size[288];
  UINT16 value[288];
} Huffman;

typedef struct {
  CONST UINT8 *in, *in_end;
  UINT8 *out, *out_start, *out_end;
  UINT64 bits;
  UINT32 nbits;
  BOOLEAN bad;
} Inflater;

static UINT32 reverse16(UINT32 v) {
  v = ((v & 0xaaaa) >> 1) | ((v & 0x5555) << 1);
  v = ((v & 0xcccc) >> 2) | ((v & 0x3333) << 2);
  v = ((v & 0xf0f0) >> 4) | ((v & 0x0f0f) << 4);
  v = ((v & 0xff00) >> 8) | ((v & 0x00ff) << 8);
  return v;
}

static BOOLEAN huffman_build(Huffman *h, CONST UINT8 *lengths, UINT32 count) {
  UINT32 sizes[MAX_BITS + 1] = { 0 }, next_code[MAX_BITS + 2];
  UINT32 code = 0, symbol = 0;

  for (UINT32 i = 0; i < (1u << FAST_BITS); ++i)
    h->fast[i] = 0;
  for (UINT32 i = 0; i < count; ++i)
    ++sizes[lengths[i]];
  sizes[0] = 0;
  for (UINT32 i = 1; i <= MAX_BITS; ++i)
    if (sizes[i] > (1u << i))
      return FALSE;
  for (UINT32 i = 1; i <= MAX_BITS; ++i) {
    next_code[i] = code;
    h->first_code[i] = (UINT16)code;
    h->first_symbol[i] = (UINT16)symbol;
    code += sizes[i];
    if (sizes[i] && code - 1 >= (1u << i))
      return FALSE;
    h->max_code[i] = code << (16 - i);
    code <<= 1;
    symbol += sizes[i];
  }
  h->max_code[MAX_BITS + 1] = 0x10000;
  for (UINT32 i = 0; i < count; ++i) {
    UINT32 len = lengths[i];
    if (!len)
      continue;
    UINT32 slot = next_code[len] - h->first_code[len] + h->first_symbol[len];
    h->size[slot] = (UINT8)len;
    h->value[slot] = (UINT16)i;
    if (len <= FAST_BITS) {
      // the stream holds codes bit-reversed: every fast index whose low len bits match
      for (UINT32 j = reverse16(next_code[len]) >> (16 - len); j < (1u << FAST_BITS); j += 1u << len)
        h->fast[j] = (UINT16)(i << 4 | len);
    }
    ++next_code[len];
  }
  return TRUE;
}

// only real input enters the bit buffer (a stored block hands the unused bytes back),
// so running short means a truncated stream
static VOID refill(Inflater *z) {
  while (z->nbits <= 56 && z->in < z->in_end) {
    z->bits |= (UINT64)*z->in++ << z->nbits;
    z->nbits += 8;
  }
}

static UINT32 take(Inflater *z, UINT32 n) {
  if (z->nbits < n)
    refill(z);
  if (z->nbits < n) {
    z->bad = TRUE;
    return 0;
  }
  UINT32 v = (UINT32)(z->bits & ((1ULL << n) - 1));
  z->bits >>= n;
  z->nbits -= n;
  return v;
}

// bits past the end of the input read as zeros here; a code that needs them is rejected
static INT32 decode(Inflater *z, CONST Huffman *h) {
  if (z->nbits < 16)
    refill(z);
  UINT32 fast = h->fast[z->bits & ((1u << FAST_BITS) - 1)];
  UINT32 len;
  if (fast) {
    len = fast & 15;
    if (len > z->nbits)
      return -1;
    z->bits >>= len;
    z->nbits -= len;
    return (INT32)(fast >> 4);
  }
  // longer than FAST_BITS: find the length whose canonical range holds the next 16 bits
  UINT32 k = reverse16((UINT32)(z->bits & 0xffff));
  for (len = FAST_BITS + 1; len <= MAX_BITS; ++len)
    if (k < h->max_code[len])
      break;
  if (len > MAX_BITS || len > z->nbits)
    return -1;
  UINT32 slot = (k >> (16 - len)) - h->first_code[len] + h->first_symbol[len];
  if (slot >= 288 || h->size[slot] != len)
    return -1;
  z->bits >>= len;
  z->nbits -= len;
  return h->value[slot];
}

static CONST UINT16 length_base[29] = { 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
                                        35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258 };
static CONST UINT8 length_extra[29] = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
                                        3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0 };
static CONST UINT16 dist_base[30] = { 1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
                                      257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145,
                                      8193, 12289, 16385, 24577 };
static CONST UINT8 dist_extra[30] = { 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
                                      7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13 };

static BOOLEAN inflate_codes(Inflater *z, CONST Huffman *lit, CONST Huffman *dist,
                             VOID (*tick)(VOID), UINT8 **next_tick) {
  for (;;) {
    INT32 sym = decode(z, lit);
    if (sym < 0 || z->bad)
      return FALSE;
    if (sym < 256) {
      if (z->out >= z->out_end)
        return FALSE;
      *z->out++ = (UINT8)sym;
      continue;
    }
    if (sym == 256)
      return TRUE;
    sym -= 257;
    if (sym >= 29)
      return FALSE;
    UINT32 len = length_base[sym] + take(z, length_extra[sym]);
    INT32 d = decode(z, dist);
    if (d < 0 || d >= 30)
      return FALSE;
    UINT32 back = dist_base[d] + take(z, dist_extra[d]);
    if ((UINTN)(z->out - z->out_start) < back || (UINTN)(z->out_end - z->out) < len)
      return FALSE;
    CONST UINT8 *from = z->out - back;
    for (UINT32 i = 0; i < len; ++i)   // overlapping copies repeat the pattern, so bytewise
      z->out[i] = from[i];
    z->out += len;
    if (tick && z->out >= *next_tick) {
      tick();
      *next_tick = z->out + 0x400000;
    }
  }
}

static BOOLEAN inflate_dynamic(Inflater *z, Huffman *lit, Huffman *dist) {
  static CONST UINT8 order[19] = { 16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15 };
  UINT8 lengths[286 + 30], code_lengths[19] = { 0 };
  Huffman codes;
  UINT32 hlit = take(z, 5) + 257, hdist = take(z, 5) + 1, hclen = take(z, 4) + 4;

  if (hlit > 286 || hdist > 30)
    return FALSE;
  for (UINT32 i = 0; i < hclen; ++i)
    code_lengths[order[i]] = (UINT8)take(z, 3);
  if (!huffman_build(&codes, code_lengths, 19))
    return FALSE;
  for (UINT32 n = 0; n < hlit + hdist;) {
    INT32 c = decode(z, &codes);
    if (c < 0 || z->bad)
      return FALSE;
    if (c < 16) {
      lengths[n++] = (UINT8)c;
      continue;
    }
    UINT32 repeat;
    UINT8 fill = 0;
    if (c == 16) {
      if (n == 0)
        return FALSE;
      fill = lengths[n - 1];
      repeat = take(z, 2) + 3;
    } else if (c == 17) {
      repeat = take(z, 3) + 3;
    } else {
      repeat = take(z, 7) + 11;
    }
    if (n + repeat > hlit + hdist)
      return FALSE;
    while (repeat--)
      lengths[n++] = fill;
  }
  return huffman_build(lit, lengths, hlit) && huffman_build(dist, lengths + hlit, hdist);
}

static BOOLEAN inflate_fixed(Huffman *lit, Huffman *dist) {
  UINT8 lengths[288];
  for (UINT32 i = 0; i < 288; ++i)
    lengths[i] = i < 144 ? 8 : i < 256 ? 9 : i < 280 ? 7 : 8;
  if (!huffman_build(lit, lengths, 288))
    return FALSE;
  for (UINT32 i = 0; i < 30; ++i)
    lengths[i] = 5;
  return huffman_build(dist, lengths, 30);
}

// a stored block starts on a byte boundary: give back what the bit buffer read ahead
static BOOLEAN inflate_stored(Inflater *z) {
  take(z, z->nbits & 7);
  while (z->nbits >= 8) {
    --z->in;
    z->nbits -= 8;
  }
  z->bits = 0;
  if (z->in_end - z->in < 4)
    return FALSE;
  UINT32 len = z->in[0] | (UINT32)z->in[1] << 8, nlen = z->in[2] | (UINT32)z->in[3] << 8;
  z->in += 4;
  if ((len ^ 0xffff) != nlen || (UINTN)(z->in_end - z->in) < len || (UINTN)(z->out_end - z->out) < len)
    return FALSE;
  for (UINT32 i = 0; i < len; ++i)
    z->out[i] = z->in[i];
  z->in += len;
  z->out += len;
  return TRUE;
}

static UINTN gzip_header(CONST UINT8 *src, UINTN src_len) {
  if (src_len < 18 || src[0] != 0x1f || src[1] != 0x8b || src[2] != 8)
    return 0;
  UINT8 flags = src[3];
  UINTN off = 10;
  if (flags & 4) {   // FEXTRA
    if (off + 2 > src_len)
      return 0;
    off += 2 + (src[off] | (UINTN)src[off + 1] << 8);
  }
  for (UINT32 field = 8; field <= 16; field <<= 1) {   // FNAME, FCOMMENT: nul terminated
    if (flags & field) {
      while (off < src_len && src[off])
        ++off;
      ++off;
    }
  }
  if (flags & 2)     // FHCRC
    off += 2;
  return off + 8 <= src_len ? off : 0;
}

UINTN gzip_size(CONST UINT8 *src, UINTN src_len) {
  if (!gzip_header(src, src_len))
    return 0;
  CONST UINT8 *t = src + src_len - 4;
  return t[0] | (UINTN)t[1] << 8 | (UINTN)t[2] << 16 | (UINTN)t[3] << 24;
}

UINTN gzip_inflate(CONST UINT8 *src, UINTN src_len, UINT8 *dst, UINTN dst_len, VOID (*tick)(VOID)) {
  static Huffman lit, dist;   // ~3KB each: off the small loader stack
  UINTN start = gzip_header(src, src_len);
  if (!start)
    return 0;

  Inflater z = { src + start, src + src_len - 8, dst, dst, dst + dst_len, 0, 0, FALSE };
  UINT8 *next_tick = dst + 0x400000;
  UINT32 last;
  do {
    last = take(&z, 1);
    UINT32 type = take(&z, 2);
    BOOLEAN ok = type == 0   ? inflate_stored(&z)
                 : type == 1 ? inflate_fixed(&lit, &dist) && inflate_codes(&z, &lit, &dist, tick, &next_tick)
                 : type == 2 ? inflate_dynamic(&z, &lit, &dist) && inflate_codes(&z, &lit, &dist, tick, &next_tick)
                             : FALSE;
    if (!ok || z.bad)
      return 0;
  } while (!last);
  return (UINTN)(z.out - dst);
}
