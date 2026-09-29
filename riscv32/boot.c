#include <stdint.h>
#include <stddef.h>

// xnu riscv32 boot shim: reads the kernel Mach-O and boot-args.txt from a newc cpio
// initrd, loads the kernel at a 4 MB aligned physical base, turns the firmware FDT into
// the flattened apple device tree and enters _start with paging off

void boot_rv32_jump(uint32_t entry, uint32_t boot_args, uint32_t hartid) __attribute__((noreturn));

#ifdef BOOT_ESP32P4
// the p4's uart0, the tx fifo count sits in bits 16-23 of the status register
#define P4_UART0 0x500CA000u
static void putc(char c) {
  while (((*(volatile uint32_t *)(P4_UART0 + 0x1C) >> 16) & 0xFF) >= 127)
    ;
  *(volatile uint32_t *)P4_UART0 = (uint8_t)c;
}
#else
// qemu virt's ns16550a, byte registers, until the FDT names another
static volatile uint8_t *uart = (volatile uint8_t *)0x10000000;
static uint32_t uart_shift;

static void putc(char c) {
  while (!(uart[5 << uart_shift] & 0x20))
    ;
  uart[0] = (uint8_t)c;
}
#endif
static void puts(const char *s) {
  while (*s) {
    if (*s == '\n')
      putc('\r');
    putc(*s++);
  }
}
static void puthex(uint32_t v) {
  puts("0x");
  for (int i = 7; i >= 0; --i)
    putc("0123456789abcdef"[(v >> (i * 4)) & 0xf]);
}
static void halt(const char *why) {
  puts("boot-rv32: ");
  puts(why);
  puts("\n");
  for (;;)
    __asm__ volatile("wfi");
}

void *memset(void *d, int c, size_t n) {
  uint8_t *p = d;
  while (n--)
    *p++ = (uint8_t)c;
  return d;
}
void *memcpy(void *d, const void *s, size_t n) {
  uint8_t *p = d;
  const uint8_t *q = s;
  if ((((uintptr_t)p | (uintptr_t)q) & 3) == 0) {
    for (; n >= 4; n -= 4, p += 4, q += 4)
      *(uint32_t *)p = *(const uint32_t *)q;
  }
  while (n--)
    *p++ = *q++;
  return d;
}
static size_t strlen(const char *s) {
  size_t n = 0;
  while (s[n])
    ++n;
  return n;
}
static int streq(const char *a, const char *b) {
  while (*a && *a == *b) {
    ++a;
    ++b;
  }
  return *a == *b;
}
static int prefix(const char *s, const char *p) {
  while (*p && *s == *p) {
    ++s;
    ++p;
  }
  return !*p;
}
static int suffix(const char *s, const char *x) {
  size_t n = strlen(s), m = strlen(x);
  return m <= n && streq(s + n - m, x);
}
static int nameis(const char *name, const char *want) {
  while (*want && *name == *want) {
    ++name;
    ++want;
  }
  return !*want && (!*name || *name == '@');
}
static uint32_t be32(const uint8_t *p) {
  return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}
static uint64_t cells(const uint8_t *p, uint32_t n) {
  uint64_t v = 0;
  for (uint32_t i = 0; i < n; ++i)
    v = v << 32 | be32(p + 4 * i);
  return v;
}
static uint32_t rd32(const uint8_t *p) {
  return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

#define FDT_BEGIN_NODE 1
#define FDT_END_NODE 2
#define FDT_PROP 3
#define FDT_NOP 4

// what the shim itself needs from the FDT
static uint32_t ram_base, ram_size, initrd_start, initrd_end;
static char cmdline[256];
static const uint8_t *fdt_seed, *stdout_path;
static uint32_t fdt_seed_len, stdout_path_len;

static void parse_fdt(const uint8_t *h) {
  const uint8_t *st = h + be32(h + 8);
  const char *strings = (const char *)(h + be32(h + 12));
  uint32_t ac = 2, sc = 2, depth = 0;
  enum { OTHER, MEMORY, CHOSEN } kind = OTHER;

  for (;;) {
    uint32_t tok = be32(st);
    st += 4;
    if (tok == FDT_BEGIN_NODE) {
      const char *name = (const char *)st;
      st += (strlen(name) + 4) & ~3u;
      if (++depth == 2)
        kind = nameis(name, "memory") ? MEMORY : nameis(name, "chosen") ? CHOSEN : OTHER;
    } else if (tok == FDT_END_NODE) {
      if (depth-- == 2)
        kind = OTHER;
      if (depth == 0)
        break;
    } else if (tok == FDT_PROP) {
      uint32_t len = be32(st);
      const char *pn = strings + be32(st + 4);
      const uint8_t *v = st + 8;
      st += 8 + ((len + 3) & ~3u);
      if (depth == 1 && streq(pn, "#address-cells"))
        ac = be32(v);
      else if (depth == 1 && streq(pn, "#size-cells"))
        sc = be32(v);
      else if (depth == 2 && kind == MEMORY && streq(pn, "reg") && !ram_size) {
        ram_base = (uint32_t)cells(v, ac);
        ram_size = (uint32_t)cells(v + 4 * ac, sc);
      } else if (depth == 2 && kind == CHOSEN && streq(pn, "bootargs")) {
        uint32_t n = len < sizeof(cmdline) ? len : sizeof(cmdline) - 1;
        memcpy(cmdline, v, n);
        cmdline[n] = 0;
      } else if (depth == 2 && kind == CHOSEN && streq(pn, "stdout-path")) {
        stdout_path = v;
        stdout_path_len = len;
      } else if (depth == 2 && kind == CHOSEN &&
                 (streq(pn, "rng-seed") || (streq(pn, "kaslr-seed") && !fdt_seed))) {
        fdt_seed = v;
        fdt_seed_len = len;
      } else if (depth == 2 && kind == CHOSEN && streq(pn, "linux,initrd-start")) {
        initrd_start = (uint32_t)cells(v, len / 4);
      } else if (depth == 2 && kind == CHOSEN && streq(pn, "linux,initrd-end")) {
        initrd_end = (uint32_t)cells(v, len / 4);
      }
    } else if (tok != FDT_NOP) {
      break;
    }
  }
}

// newc cpio lookup by name
static uint32_t hex8(const uint8_t *s) {
  uint32_t v = 0;
  for (int i = 0; i < 8; ++i) {
    uint8_t c = s[i];
    v = v << 4 | (c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 :
                  c >= 'A' && c <= 'F' ? c - 'A' + 10 : 0);
  }
  return v;
}
static const uint8_t *cpio_find(const char *want, uint32_t *size) {
  const uint8_t *base = (const uint8_t *)initrd_start;
  uint32_t off = 0, total = initrd_end - initrd_start;
  while (off + 110 <= total && base[off] == '0') {
    const uint8_t *h = base + off;
    uint32_t fsize = hex8(h + 54), nsize = hex8(h + 94);
    const char *name = (const char *)(h + 110);
    uint32_t data = (off + 110 + nsize + 3) & ~3u;
    if (streq(name, "TRAILER!!!"))
      break;
    if (streq(name, want)) {
      *size = fsize;
      return base + data;
    }
    off = (data + fsize + 3) & ~3u;
  }
  return 0;
}

// the flattened apple device tree: node {nprops, nchildren}, prop {name[32], len, value}
// written in two passes over the FDT, the first only counts so each node header is exact
#define MAX_NODES 1024
#define MAX_DEPTH 16
#define DT_NAME_LEN 32

static uint32_t node_props[MAX_NODES], node_children[MAX_NODES];
static uint8_t *dt_out;
static int dt_emit;
static uint32_t cur_node;

static void dt_node(uint32_t idx) {
  cur_node = idx;
  if (dt_emit) {
    ((uint32_t *)dt_out)[0] = node_props[idx];
    ((uint32_t *)dt_out)[1] = node_children[idx];
    dt_out += 8;
  }
}
static void dt_prop(const char *name, const void *v, uint32_t len) {
  if (!dt_emit) {
    node_props[cur_node]++;
    return;
  }
  uint32_t n = (uint32_t)strlen(name);
  if (n > DT_NAME_LEN - 1)
    n = DT_NAME_LEN - 1;
  memset(dt_out, 0, DT_NAME_LEN);
  memcpy(dt_out, name, n);
  *(uint32_t *)(dt_out + DT_NAME_LEN) = len;
  memcpy(dt_out + DT_NAME_LEN + 4, v, len);
  memset(dt_out + DT_NAME_LEN + 4 + len, 0, ((len + 3) & ~3u) - len);
  dt_out += DT_NAME_LEN + 4 + ((len + 3) & ~3u);
}
static void dt_str(const char *name, const char *s) {
  dt_prop(name, s, (uint32_t)strlen(s) + 1);
}
static void dt_u32(const char *name, uint32_t v) {
  dt_prop(name, &v, 4);
}

// dtc's guess: one or more non-empty printable strings, each nul terminated
static int looks_like_strings(const uint8_t *v, uint32_t len) {
  if (len == 0 || v[len - 1] != 0)
    return 0;
  uint32_t start = 0;
  for (uint32_t i = 0; i < len; i++) {
    if (v[i] == 0) {
      if (i == start)
        return 0;
      start = i + 1;
    } else if (v[i] < 0x20 || v[i] > 0x7e) {
      return 0;
    }
  }
  return 1;
}

// n big endian cells as the kernel reads them: 1 cell a u32, the last 2 cells one u64
static uint32_t put_value(uint8_t *out, const uint8_t *c, uint32_t n) {
  uint32_t o = 0;
  for (uint32_t i = 0; n >= 2 && i < n - 2; i++, o += 4) {
    uint32_t w = be32(c + 4 * i);
    memcpy(out + o, &w, 4);
  }
  if (n == 1) {
    uint32_t w = be32(c);
    memcpy(out + o, &w, 4);
    o += 4;
  } else if (n >= 2) {
    uint32_t hi = be32(c + 4 * (n - 2)), lo = be32(c + 4 * (n - 1));
    memcpy(out + o, &lo, 4);
    memcpy(out + o + 4, &hi, 4);
    o += 8;
  }
  return o;
}
static int put_tuples(uint8_t *out, const uint8_t *v, uint32_t len, const uint32_t *f, uint32_t nf) {
  uint32_t stride = 0;
  for (uint32_t i = 0; i < nf; i++)
    stride += f[i];
  if (stride == 0 || len % (4 * stride) != 0)
    return 0;
  uint32_t o = 0;
  for (uint32_t at = 0; at < len;) {
    for (uint32_t i = 0; i < nf; i++) {
      o += put_value(out + o, v + at, f[i]);
      at += 4 * f[i];
    }
  }
  return 1;
}

struct level {
  uint32_t idx, addr_cells, size_cells, parent_addr_cells, parent_size_cells;
  int skip, has_addr_cells, has_size_cells, has_ranges, is_cpu, have_reg;
  uint32_t reg;
};

static uint8_t prop_buf[4096];

static void import_prop(struct level *lv, const char *name, const uint8_t *v, uint32_t len) {
  if (streq(name, "name"))
    return;
  if (len == 0 || len > sizeof(prop_buf)) {
    dt_prop(name, v, len > sizeof(prop_buf) ? 0 : len);
    return;
  }
  int numeric = streq(name, "reg") || streq(name, "ranges") || streq(name, "dma-ranges") ||
                streq(name, "phandle") || streq(name, "linux,phandle") ||
                streq(name, "interrupt-parent") || streq(name, "interrupts") ||
                streq(name, "interrupts-extended") || name[0] == '#';
  int done = 0;
  if (!numeric && (looks_like_strings(v, len) || suffix(name, "-names"))) {
    memcpy(prop_buf, v, len);
    done = 1;
  } else if (len % 4 != 0) {
    memcpy(prop_buf, v, len);
    done = 1;
  } else if (streq(name, "reg")) {
    uint32_t f[2] = { lv->parent_addr_cells, lv->parent_size_cells };
    done = put_tuples(prop_buf, v, len, f, 2);
  } else if (streq(name, "ranges") || streq(name, "dma-ranges")) {
    uint32_t f[3] = { lv->addr_cells, lv->parent_addr_cells, lv->size_cells };
    done = put_tuples(prop_buf, v, len, f, 3);
  } else if (len == 8 && (suffix(name, "-frequency") || prefix(name, "linux,initrd-"))) {
    put_value(prop_buf, v, 2);
    done = 1;
  }
  if (!done) {
    for (uint32_t i = 0; i + 4 <= len; i += 4) {
      uint32_t w = be32(v + i);
      memcpy(prop_buf + i, &w, 4);
    }
  }
  dt_prop(name, prop_buf, len);
  // SecureDTFindNodeWithPhandle and the platform expert look for apple's spelling
  if (streq(name, "phandle"))
    dt_prop("AAPL,phandle", prop_buf, len);
}

// a node's properties come before its children, so the conversion reads them ahead
static void scan_node(const uint8_t *st, const char *strings, struct level *lv) {
  for (;;) {
    uint32_t tok = be32(st);
    if (tok == FDT_NOP) {
      st += 4;
      continue;
    }
    if (tok != FDT_PROP)
      return;
    uint32_t len = be32(st + 4);
    const char *pn = strings + be32(st + 8);
    const uint8_t *v = st + 12;
    if (len == 4 && streq(pn, "#address-cells")) {
      lv->addr_cells = be32(v);
      lv->has_addr_cells = 1;
    } else if (len == 4 && streq(pn, "#size-cells")) {
      lv->size_cells = be32(v);
      lv->has_size_cells = 1;
    } else if (streq(pn, "ranges")) {
      lv->has_ranges = 1;
    } else if (streq(pn, "device_type") && len >= 4 && streq((const char *)v, "cpu")) {
      lv->is_cpu = 1;
    } else if (streq(pn, "reg") && len >= 4) {
      lv->reg = be32(v + (lv->parent_addr_cells >= 2 ? 4 : 0));
      lv->have_reg = 1;
    }
    st += 12 + ((len + 3) & ~3u);
  }
}

// set by main before the conversion, the loader's own /chosen and its ramdisk
static uint32_t boot_hartid, rd_phys, rd_size;
static uint8_t seed[64];

static void emit_extra_children(uint32_t *next) {
  uint32_t chosen = (*next)++, mm = (*next)++, defaults = (*next)++, options = (*next)++;
  if (!dt_emit) {
    node_children[0] += 3;
    for (uint32_t i = chosen; i <= options; i++)
      node_props[i] = node_children[i] = 0;
  }

  dt_node(chosen);
  if (!dt_emit)
    node_children[chosen] = 1;
  dt_str("name", "chosen");
  dt_u32("dram-base", ram_base);
  dt_u32("dram-size", ram_size);
  dt_prop("random-seed", seed, sizeof(seed));
  dt_str("firmware-version", "xnu-loader rv32");
  dt_u32("boot-hart", boot_hartid);
  if (stdout_path && looks_like_strings(stdout_path, stdout_path_len))
    dt_prop("stdout-path", stdout_path, stdout_path_len);

  dt_node(mm);
  dt_str("name", "memory-map");
  if (rd_size) {
    uint32_t r[2] = { rd_phys, rd_size };
    dt_prop("RAMDisk", r, sizeof(r));
  }

  dt_node(defaults);
  dt_str("name", "defaults");
  dt_node(options);
  dt_str("name", "options");
}

static uint32_t convert_fdt(const uint8_t *h) {
  const uint8_t *st = h + be32(h + 8);
  const char *strings = (const char *)(h + be32(h + 12));
  struct level lv[MAX_DEPTH];
  uint32_t depth = 0, next = 0;

  for (;;) {
    uint32_t tok = be32(st);
    st += 4;
    if (tok == FDT_BEGIN_NODE) {
      const char *name = (const char *)st;
      st += (strlen(name) + 4) & ~3u;
      if (depth >= MAX_DEPTH)
        halt("FDT nests too deep");
      struct level *l = &lv[depth], *up = depth ? &lv[depth - 1] : 0;
      memset(l, 0, sizeof(*l));
      // the devicetree spec defaults when a node leaves them out
      l->addr_cells = 2;
      l->size_cells = 1;
      l->parent_addr_cells = up ? up->addr_cells : 2;
      l->parent_size_cells = up ? up->size_cells : 1;
      scan_node(st, strings, l);
      if (depth && (up->skip || (depth == 1 && nameis(name, "chosen")))) {
        // /chosen is the loader's own
        l->skip = 1;
      } else {
        if (next >= MAX_NODES)
          halt("FDT has too many nodes");
        l->idx = next++;
        if (!dt_emit) {
          node_props[l->idx] = 0;
          node_children[l->idx] = 0;
          if (up)
            node_children[up->idx]++;
        }
        dt_node(l->idx);
        dt_str("name", depth ? name : "device-tree");
        // iokit's address resolution assumes 1 size cell where a bus says nothing
        if (depth == 0 || l->has_ranges) {
          if (!l->has_addr_cells)
            dt_u32("#address-cells", l->addr_cells);
          if (!l->has_size_cells)
            dt_u32("#size-cells", l->size_cells);
        }
        // the kernel takes the cpu marked running as the boot hart
        if (l->is_cpu && l->have_reg)
          dt_str("state", l->reg == boot_hartid ? "running" : "waiting");
      }
      depth++;
    } else if (tok == FDT_END_NODE) {
      if (depth == 0)
        break;
      depth--;
      if (depth == 0) {
        emit_extra_children(&next);
        break;
      }
      // properties of the parent that follow a child would break the flattened layout
      cur_node = lv[depth - 1].idx;
    } else if (tok == FDT_PROP) {
      uint32_t len = be32(st);
      const char *pn = strings + be32(st + 4);
      const uint8_t *v = st + 8;
      st += 8 + ((len + 3) & ~3u);
      if (depth == 0 || lv[depth - 1].skip)
        continue;
      cur_node = lv[depth - 1].idx;
      import_prop(&lv[depth - 1], pn, v, len);
    } else if (tok != FDT_NOP) {
      break;
    }
  }
  return next;
}

#ifndef BOOT_ESP32P4
// the uart the FDT's stdout-path or first ns16550 names, with its register shift
static void find_uart(const uint8_t *h) {
  const uint8_t *st = h + be32(h + 8);
  const char *strings = (const char *)(h + be32(h + 12));
  uint32_t depth = 0, ac[MAX_DEPTH], is16550 = 0, shift = 0, addr = 0;
  ac[0] = 2;

  for (;;) {
    uint32_t tok = be32(st);
    st += 4;
    if (tok == FDT_BEGIN_NODE) {
      st += (strlen((const char *)st) + 4) & ~3u;
      if (depth + 1 < MAX_DEPTH)
        ac[depth + 1] = 2;
      depth++;
      is16550 = shift = addr = 0;
    } else if (tok == FDT_END_NODE) {
      if (is16550 && addr) {
        uart = (volatile uint8_t *)addr;
        uart_shift = shift;
        return;
      }
      is16550 = 0;
      if (depth-- <= 1)
        return;
    } else if (tok == FDT_PROP) {
      uint32_t len = be32(st);
      const char *pn = strings + be32(st + 4);
      const uint8_t *v = st + 8;
      st += 8 + ((len + 3) & ~3u);
      if (streq(pn, "#address-cells") && depth < MAX_DEPTH)
        ac[depth] = be32(v);
      else if (streq(pn, "compatible"))
        for (uint32_t i = 0; i < len; i += strlen((const char *)v + i) + 1)
          is16550 |= prefix((const char *)v + i, "ns16550");
      else if (streq(pn, "reg") && depth >= 1)
        addr = (uint32_t)cells(v, ac[depth - 1]);
      else if (streq(pn, "reg-shift"))
        shift = be32(v);
    } else if (tok != FDT_NOP) {
      return;
    }
  }
}

#endif /* !BOOT_ESP32P4 */

static uint64_t rdtime64(void) {
  uint32_t hi, lo, hi2;
  do {
    __asm__ volatile("rdtimeh %0" : "=r"(hi));
    __asm__ volatile("rdtime %0" : "=r"(lo));
    __asm__ volatile("rdtimeh %0" : "=r"(hi2));
  } while (hi != hi2);
  return (uint64_t)hi << 32 | lo;
}

// 64 seed bytes for xnu's PRNG: the FDT's rng-seed mixed with the timer
static void make_seed(uint8_t *out) {
  uint64_t x = rdtime64() ^ 0x9e3779b97f4a7c15ull;
  for (uint32_t i = 0; i < 64; ++i) {
    if (fdt_seed && i < fdt_seed_len)
      x ^= (uint64_t)fdt_seed[i] << (8 * (i & 7));
    x += 0x9e3779b97f4a7c15ull;
    uint64_t z = x;
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    z ^= z >> 31;
    out[i] = (uint8_t)(z | 1);
  }
}

// pexpert/riscv/boot.h on rv32, the arm32 layout
struct boot_args32 {
  uint16_t Revision, Version;
  uint32_t virtBase, physBase, memSize, topOfKernelData;
  uint32_t v_baseAddr, v_display, v_rowBytes, v_width, v_height, v_depth;
  uint32_t machineType;
  uint32_t deviceTreeP, deviceTreeLength;
  char CommandLine[256];
  uint32_t bootFlags, memSizeActual;
};

#define VIRT_BASE 0x80000000u
#define ALIGN(x, a) (((x) + (a) - 1) & ~((a) - 1))
#define MB4 0x400000u
#define CPU_TYPE_RISCV 24

extern char _start[], __bss_end[];

#ifndef BOOT_ESP32P4
void boot_rv32_main(uint32_t hartid, uint32_t fdt) {
  const uint8_t *h = (const uint8_t *)fdt;
  if (be32(h) != 0xd00dfeed)
    halt("no device tree in a1");
  find_uart(h);
  parse_fdt(h);
  boot_hartid = hartid;
  puts("\nboot-rv32: xnu riscv32 shim, hart ");
  puthex(hartid);
  puts("\nboot-rv32: RAM ");
  puthex(ram_base);
  puts(" + ");
  puthex(ram_size);
  puts(", initrd ");
  puthex(initrd_start);
  puts(" - ");
  puthex(initrd_end);
  puts("\n");
  if (!initrd_start || initrd_end <= initrd_start)
    halt("no initrd: pass the kernel in a cpio with -initrd");

  uint32_t ksize = 0, asize = 0, rsize = 0;
  const uint8_t *k = cpio_find("EFI/BOOT/kernel", &ksize);
  const uint8_t *a = cpio_find("EFI/BOOT/boot-args.txt", &asize);
  const uint8_t *rd = cpio_find("EFI/BOOT/ramdisk.img", &rsize);
  if (!k)
    halt("EFI/BOOT/kernel not in the initrd");
  if (rd32(k) != 0xfeedface || rd32(k + 4) != CPU_TYPE_RISCV)
    halt("kernel is not a riscv32 Mach-O");

  uint32_t ncmds = rd32(k + 16), entry = 0, top = 0;
  const uint8_t *lc = k + 28;
  for (uint32_t i = 0; i < ncmds; ++i) {
    uint32_t cmd = rd32(lc), size = rd32(lc + 4);
    if (cmd == 1 && rd32(lc + 28) && rd32(lc + 24) >= VIRT_BASE) {
      uint32_t end = rd32(lc + 24) - VIRT_BASE + rd32(lc + 28);
      if (end > top)
        top = end;
    }
    lc += size;
  }

  // the kernel owns [physBase, RAM end) and xnu never sees what lies below it, so go as
  // low as this shim allows and only climb over the initrd when the image would hit it
  // start.s wants physBase 4 MB aligned and at least 8 MB into RAM for the satp switch
  uint32_t span = ALIGN(top, 0x1000) + 0x1000 + 0x40000 + 0x4000 + (rd ? ALIGN(rsize, 0x1000) : 0);
  uint32_t low = ALIGN((uint32_t)__bss_end, MB4);
  if (low < ram_base + 2 * MB4)
    low = ram_base + 2 * MB4;
  if (low < initrd_end && low + span > initrd_start)
    low = ALIGN(initrd_end, MB4);
  uint32_t phys_base = low;

  top = 0;
  lc = k + 28;
  for (uint32_t i = 0; i < ncmds; ++i) {
    uint32_t cmd = rd32(lc), size = rd32(lc + 4);
    if (cmd == 1) { // LC_SEGMENT
      uint32_t vm = rd32(lc + 24), vms = rd32(lc + 28), fo = rd32(lc + 32), fs = rd32(lc + 36);
      if (vms && vm >= VIRT_BASE) {
        uint8_t *dst = (uint8_t *)(phys_base + vm - VIRT_BASE);
        memcpy(dst, k + fo, fs);
        memset(dst + fs, 0, vms - fs);
        if (vm - VIRT_BASE + vms > top)
          top = vm - VIRT_BASE + vms;
      }
    } else if (cmd == 5) { // LC_UNIXTHREAD, riscv_thread_state32: x0-x31 then pc
      entry = rd32(lc + 16 + 32 * 4);
    }
    lc += size;
  }
  if (!entry)
    halt("kernel has no LC_UNIXTHREAD entry");

  uint32_t ba_phys = phys_base + ALIGN(top, 0x1000);
  struct boot_args32 *ba = (struct boot_args32 *)ba_phys;
  uint32_t dt_phys = ba_phys + 0x1000;

  // xnu takes /chosen/memory-map/RAMDisk as md0 and reaches it through the static
  // mapping, so it sits below topOfKernelData
  rd_phys = ALIGN(dt_phys + 0x40000, 0x4000);
  rd_size = rd ? ALIGN(rsize, 0x1000) : 0;
  if (rd) {
    puts("boot-rv32: moving the ramdisk\n");
    memcpy((void *)rd_phys, rd, rsize);
  }

  make_seed(seed);
  dt_emit = 0;
  convert_fdt(h);
  dt_emit = 1;
  dt_out = (uint8_t *)dt_phys;
  uint32_t nodes = convert_fdt(h);
  uint32_t dt_len = (uint32_t)(dt_out - (uint8_t *)dt_phys);
  if (dt_len > 0x40000)
    halt("device tree overlaps the ramdisk");

  uint32_t top_of_kernel = ALIGN(rd ? rd_phys + rd_size : dt_phys + dt_len, 0x4000);
  if (top_of_kernel + 0x800000 > ram_base + ram_size)
    halt("not enough RAM above the kernel");

  memset(ba, 0, sizeof(*ba));
  ba->Revision = 2;
  ba->Version = 2;
  ba->virtBase = VIRT_BASE;
  ba->physBase = phys_base;
  ba->memSize = ram_base + ram_size - phys_base;
  ba->memSizeActual = ram_size;
  ba->topOfKernelData = top_of_kernel;
  ba->deviceTreeP = dt_phys - phys_base + VIRT_BASE;
  ba->deviceTreeLength = dt_len;
  const char *args = a ? (const char *)a : cmdline;
  uint32_t alen = a ? asize : (uint32_t)strlen(cmdline);
  if (alen > sizeof(ba->CommandLine) - 1)
    alen = sizeof(ba->CommandLine) - 1;
  memcpy(ba->CommandLine, args, alen);
  for (uint32_t i = 0; i < alen; ++i)
    if (ba->CommandLine[i] == '\n')
      ba->CommandLine[i] = ' ';

  puts("boot-rv32: device tree ");
  puthex(nodes);
  puts(" nodes, ");
  puthex(dt_len);
  puts(" bytes\n");
  if (rd) {
    puts("boot-rv32: ramdisk at ");
    puthex(rd_phys);
    puts(" + ");
    puthex(rd_size);
    puts("\n");
  }
  puts("boot-rv32: kernel at ");
  puthex(phys_base);
  puts(", entry ");
  puthex(entry);
  puts(", boot_args ");
  puthex(ba_phys);
  puts(", top ");
  puthex(top_of_kernel);
  puts("\nboot-rv32: args \"");
  puts(ba->CommandLine);
  puts("\"\n");
  boot_rv32_jump(entry - VIRT_BASE + phys_base, ba_phys, hartid);
}
#endif /* !BOOT_ESP32P4 */

#ifdef BOOT_ESP32P4
// the esp32-p4 path: no mmu, so the kernel runs at the address it was linked at, one page
// into psram. The emulator's loader (or a flash image) leaves a newc cpio with the kernel,
// boot-args.txt and an optional ramdisk.img at P4_PAYLOAD
#define P4_PSRAM_BASE 0x48000000u
#define P4_PAYLOAD 0x48C00000u
#define P4_FLASH_BASE 0x40000000u
#define P4_FLASH_SIZE 0x01000000u
#define P4_FLASH_MMU_CONTENT 0x5008C37Cu
#define P4_FLASH_MMU_INDEX 0x5008C380u
#define P4_FLASH_MMU_VALID (1u << 12)
#define P4_PSRAM_MMU_CONTENT 0x5008E37Cu
#define P4_PSRAM_MMU_INDEX 0x5008E380u
#define P4_PSRAM_MMU_VALID (1u << 11)
#define P4_PSRAM_MMU_ACCESS (1u << 10)
#define P4_L1_ACS_FAIL_CTRL 0x3FF10168u
#define P4_L2_ACS_FAIL_CTRL 0x3FF102E8u
#define P4_MHINT_SBE (1u << 20)

extern const uint8_t p4_dtb[];

static void *memmove(void *d, const void *s, size_t n) {
  uint8_t *p = d;
  const uint8_t *q = s;
  if (p <= q || p >= q + n)
    return memcpy(d, s, n);
  while (n--)
    p[n] = q[n];
  return d;
}

// every 64 KB page of the configured psram at its own address in the cached window
static void p4_map_psram(uint32_t size) {
  for (uint32_t n = 0; n < size / 0x10000; n++) {
    *(volatile uint32_t *)P4_PSRAM_MMU_INDEX = n;
    *(volatile uint32_t *)P4_PSRAM_MMU_CONTENT = P4_PSRAM_MMU_VALID | P4_PSRAM_MMU_ACCESS | n;
  }
}

// every 64 KB page of flash at its own address in the cached flash window
static void p4_map_flash(void) {
  for (uint32_t n = 0; n < P4_FLASH_SIZE / 0x10000; n++) {
    *(volatile uint32_t *)P4_FLASH_MMU_INDEX = n;
    *(volatile uint32_t *)P4_FLASH_MMU_CONTENT = P4_FLASH_MMU_VALID | n;
  }
}

// an ext4 root at the start of flash stays there as a read-only memory disk, no psram
// holds a copy of it
static uint32_t p4_flash_rootfs(void) {
  const uint8_t *sb = (const uint8_t *)(P4_FLASH_BASE + 1024);
  if (sb[0x38] != 0x53 || sb[0x39] != 0xef)
    return 0;
  uint32_t blocks = rd32(sb + 4);
  uint32_t bsize = 1024u << rd32(sb + 24);
  uint64_t bytes = (uint64_t)blocks * bsize;
  if (bytes == 0 || bytes > P4_FLASH_SIZE)
    return 0;
  return (uint32_t)bytes;
}

// failed cache accesses become precise bus errors instead of silently reading zero
static void p4_precise_faults(void) {
  *(volatile uint32_t *)P4_L1_ACS_FAIL_CTRL |= 0x13;
  *(volatile uint32_t *)P4_L2_ACS_FAIL_CTRL |= 1;
  __asm__ volatile("csrs 0x7c5, %0" ::"r"(P4_MHINT_SBE));
}

void boot_p4_main(void) {
  const uint8_t *h = p4_dtb;
  uint32_t hartid;
  __asm__ volatile("csrr %0, mhartid" : "=r"(hartid));
  if (be32(h) != 0xd00dfeed)
    halt("the built in device tree is missing");
  parse_fdt(h);
  p4_map_psram(ram_size);
  p4_map_flash();
  p4_precise_faults();
  boot_hartid = hartid;
  puts("\nboot-rv32: xnu esp32-p4 shim, hart ");
  puthex(hartid);
  puts("\nboot-rv32: psram ");
  puthex(ram_base);
  puts(" + ");
  puthex(ram_size);
  puts("\n");

  // the cpio ends at its trailer, cpio_find stops there
  initrd_start = P4_PAYLOAD;
  initrd_end = ram_base + ram_size;
  uint32_t ksize = 0, asize = 0, rsize = 0;
  const uint8_t *k = cpio_find("EFI/BOOT/kernel", &ksize);
  const uint8_t *a = cpio_find("EFI/BOOT/boot-args.txt", &asize);
  const uint8_t *rd = cpio_find("EFI/BOOT/ramdisk.img", &rsize);
  if (!k)
    halt("no kernel in the payload cpio at 0x48c00000");
  if (rd32(k) != 0xfeedface || rd32(k + 4) != CPU_TYPE_RISCV)
    halt("kernel is not a riscv32 Mach-O");

  // the ramdisk goes to the top of psram first, out of the kernel's way, and xnu manages
  // only what lies below it
  uint32_t mem_end = ram_base + ram_size;
  uint32_t flash_rd = p4_flash_rootfs();
  if (flash_rd) {
    rd_phys = P4_FLASH_BASE;
    rd_size = ALIGN(flash_rd, 0x1000);
  } else if (rd) {
    rd_size = ALIGN(rsize, 0x1000);
    rd_phys = (mem_end - rd_size) & ~0x3FFFu;
    memmove((void *)rd_phys, rd, rsize);
    mem_end = rd_phys;
  }

  // segments at their own addresses, the kernel links at psram plus one page
  uint32_t ncmds = rd32(k + 16), entry = 0, top = 0;
  const uint8_t *lc = k + 28;
  for (uint32_t i = 0; i < ncmds; ++i) {
    uint32_t cmd = rd32(lc), size = rd32(lc + 4);
    if (cmd == 1) {
      uint32_t vm = rd32(lc + 24), vms = rd32(lc + 28), fo = rd32(lc + 32), fs = rd32(lc + 36);
      if (vms && vm >= P4_PSRAM_BASE && vm + vms <= P4_PAYLOAD) {
        memcpy((void *)vm, k + fo, fs);
        memset((void *)(vm + fs), 0, vms - fs);
        if (vm + vms > top)
          top = vm + vms;
      }
    } else if (cmd == 5) {
      entry = rd32(lc + 16 + 32 * 4);
    }
    lc += size;
  }
  if (!entry)
    halt("kernel has no LC_UNIXTHREAD entry");

  uint32_t ba_phys = ALIGN(top, 0x1000);
  struct boot_args32 *ba = (struct boot_args32 *)ba_phys;
  uint32_t dt_phys = ba_phys + 0x1000;

  make_seed(seed);
  dt_emit = 0;
  convert_fdt(h);
  dt_emit = 1;
  dt_out = (uint8_t *)dt_phys;
  uint32_t nodes = convert_fdt(h);
  uint32_t dt_len = (uint32_t)(dt_out - (uint8_t *)dt_phys);
  uint32_t top_of_kernel = ALIGN(dt_phys + dt_len, 0x4000);
  if (top_of_kernel >= P4_PAYLOAD)
    halt("kernel, boot args and device tree run into the payload at 0x48c00000");

  memset(ba, 0, sizeof(*ba));
  ba->Revision = 2;
  ba->Version = 2;
  ba->virtBase = P4_PSRAM_BASE;
  ba->physBase = P4_PSRAM_BASE;
  ba->memSize = mem_end - P4_PSRAM_BASE;
  ba->memSizeActual = ram_size;
  ba->topOfKernelData = top_of_kernel;
  ba->deviceTreeP = dt_phys;
  ba->deviceTreeLength = dt_len;
  uint32_t alen = a ? asize : 0;
  if (alen > sizeof(ba->CommandLine) - 1)
    alen = sizeof(ba->CommandLine) - 1;
  memcpy(ba->CommandLine, a, alen);
  for (uint32_t i = 0; i < alen; ++i)
    if (ba->CommandLine[i] == '\n')
      ba->CommandLine[i] = ' ';

  puts("boot-rv32: device tree ");
  puthex(nodes);
  puts(" nodes, ");
  puthex(dt_len);
  puts(" bytes\n");
  if (rd) {
    puts("boot-rv32: ramdisk at ");
    puthex(rd_phys);
    puts(" + ");
    puthex(rd_size);
    puts("\n");
  }
  puts("boot-rv32: kernel entry ");
  puthex(entry);
  puts(", boot_args ");
  puthex(ba_phys);
  puts(", top ");
  puthex(top_of_kernel);
  puts(", managed to ");
  puthex(mem_end);
  puts("\nboot-rv32: args \"");
  puts(ba->CommandLine);
  puts("\"\n");
  boot_rv32_jump(entry, ba_phys, hartid);
}
#endif /* BOOT_ESP32P4 */
