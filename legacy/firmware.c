#include "firmware.h"
#include <efilib.h>

#undef SetMem
static void native_set_mem(void *ptr, UINTN size, UINT8 value) {
  UINT8 *p = ptr;
  while (size--)
    *p++ = value;
}
#define SetMem(ptr, size, value) native_set_mem((ptr), (size), (value))

extern EFI_STATUS efi_main(EFI_HANDLE, EFI_SYSTEM_TABLE *);
extern UINT8 legacy_entry;
extern UINT8 __bss_end;

#define MAX_RANGES 192
#define PAGE_SIZE 4096ULL
#define LOADER_HANDLE ((EFI_HANDLE)(UINTN)0x584e554c)

static EFI_MEMORY_DESCRIPTOR ranges[MAX_RANGES];
static UINTN nranges;
static UINTN map_key = 1;
static EFI_BOOT_SERVICES boot_services;
static EFI_RUNTIME_SERVICES runtime_services;
static EFI_SYSTEM_TABLE system_table;
static EFI_CONFIGURATION_TABLE config_tables[3];
static SIMPLE_TEXT_OUTPUT_INTERFACE console_out;
static SIMPLE_TEXT_OUTPUT_MODE console_mode;
static EFI_LOADED_IMAGE loaded_image;
static EFI_GUID loaded_image_guid = LOADED_IMAGE_PROTOCOL;
static EFI_GUID graphics_output_guid = EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;
static EFI_GRAPHICS_OUTPUT_PROTOCOL graphics_output;
static EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE graphics_mode;
static EFI_GRAPHICS_OUTPUT_MODE_INFORMATION graphics_info;
static UINT64 runtime_virtual_delta;
static UINT8 normalized_rsdp[36] __attribute__((aligned(16)));

VOID InitializeLib(EFI_HANDLE image, EFI_SYSTEM_TABLE *table) {
  (void)image;
  ST = table;
  BS = table->BootServices;
  RT = table->RuntimeServices;
}

static UINT8 io_in8(UINT16 port) {
  UINT8 v;
  __asm__ volatile("inb %1,%0" : "=a"(v) : "Nd"(port));
  return v;
}
static void io_out8(UINT16 port, UINT8 v) {
  __asm__ volatile("outb %0,%1" : : "a"(v), "Nd"(port));
}

static void debug_putc(char c) {
  __asm__ volatile("outb %0, $0xe9" : : "a"(c));
  if (c == '\n') {
    while (!(io_in8(0x3fd) & 0x20)) {
    }
    io_out8(0x3f8, '\r');
  }
  while (!(io_in8(0x3fd) & 0x20)) {
  }
  io_out8(0x3f8, (UINT8)c);
}

static void debug_string(const char *s) {
  while (*s)
    debug_putc(*s++);
}

static void format_char(CHAR16 **cursor, CHAR16 *end, CHAR16 value) {
  if (*cursor < end)
    *(*cursor)++ = value;
}

static UINTN unsigned_length(UINT64 value, UINTN base) {
  UINTN length = 1;
  while (value >= base) {
    value /= base;
    ++length;
  }
  return length;
}

static void format_unsigned(CHAR16 **cursor, CHAR16 *end, UINT64 value,
                            UINTN base, BOOLEAN uppercase, UINTN width,
                            CHAR16 padding) {
  CHAR16 digits[24];
  UINTN length = 0;

  do {
    UINTN digit = value % base;
    digits[length++] = digit < 10
                           ? (CHAR16)('0' + digit)
                           : (CHAR16)((uppercase ? 'A' : 'a') + digit - 10);
    value /= base;
  } while (value && length < sizeof(digits) / sizeof(digits[0]));

  while (width > length) {
    format_char(cursor, end, padding);
    --width;
  }
  while (length)
    format_char(cursor, end, digits[--length]);
}

UINTN UnicodeVSPrint(CHAR16 *buffer, UINTN buffer_size, CONST CHAR16 *format,
                     va_list args) {
  if (!buffer || buffer_size < sizeof(CHAR16) || !format)
    return 0;

  CHAR16 *cursor = buffer;
  CHAR16 *end = buffer + buffer_size / sizeof(CHAR16) - 1;

  while (*format && cursor < end) {
    if (*format != '%') {
      format_char(&cursor, end, *format++);
      continue;
    }

    ++format;
    if (*format == '%') {
      format_char(&cursor, end, *format++);
      continue;
    }

    BOOLEAN left = FALSE;
    CHAR16 padding = ' ';
    while (*format == '-' || *format == '0') {
      if (*format == '-')
        left = TRUE;
      else
        padding = '0';
      ++format;
    }

    UINTN width = 0;
    while (*format >= '0' && *format <= '9')
      width = width * 10 + (*format++ - '0');

    UINTN precision = ~(UINTN)0;
    if (*format == '.') {
      precision = 0;
      ++format;
      while (*format >= '0' && *format <= '9')
        precision = precision * 10 + (*format++ - '0');
    }

    BOOLEAN wide = FALSE;
    if (*format == 'l') {
      wide = TRUE;
      ++format;
    }

    CHAR16 specifier = *format++;
    if (specifier == 'a' || specifier == 's') {
      CONST CHAR8 *ascii = NULL;
      CONST CHAR16 *unicode = NULL;
      UINTN length = 0;

      if (specifier == 'a') {
        ascii = va_arg(args, CONST CHAR8 *);
        while (ascii && ascii[length] && length < precision)
          ++length;
      } else {
        unicode = va_arg(args, CONST CHAR16 *);
        while (unicode && unicode[length] && length < precision)
          ++length;
      }

      if (!left)
        while (width > length) {
          format_char(&cursor, end, ' ');
          --width;
        }
      for (UINTN i = 0; i < length; ++i)
        format_char(&cursor, end,
                    specifier == 'a' ? (UINT8)ascii[i] : unicode[i]);
      if (left)
        while (width > length) {
          format_char(&cursor, end, ' ');
          --width;
        }
      continue;
    }

    if (specifier == 'c') {
      format_char(&cursor, end, (CHAR16)va_arg(args, UINTN));
      continue;
    }

    if (specifier == 'r') {
      format_char(&cursor, end, '0');
      format_char(&cursor, end, 'x');
      format_unsigned(&cursor, end, va_arg(args, EFI_STATUS), 16, FALSE, width,
                      padding);
      continue;
    }

    if (specifier == 'p') {
      UINT64 value = (UINT64)(UINTN)va_arg(args, VOID *);
      format_char(&cursor, end, '0');
      format_char(&cursor, end, 'x');
      format_unsigned(&cursor, end, value, 16, FALSE,
                      width ? width : sizeof(VOID *) * 2, '0');
      continue;
    }

    if (specifier == 'x' || specifier == 'X' || specifier == 'u' ||
        specifier == 'd') {
      UINT64 value;
      BOOLEAN negative = FALSE;

      if (specifier == 'd') {
        INT64 signed_value = wide ? va_arg(args, INT64) : va_arg(args, INT32);
        if (signed_value < 0) {
          negative = TRUE;
          signed_value = -signed_value;
        }
        value = (UINT64)signed_value;
      } else {
        value = wide ? va_arg(args, UINT64) : va_arg(args, UINT32);
      }

      if (negative) {
        format_char(&cursor, end, '-');
        if (width)
          --width;
      }
      format_unsigned(&cursor, end, value,
                      specifier == 'u' || specifier == 'd' ? 10 : 16,
                      specifier == 'X', width, padding);
      continue;
    }

    format_char(&cursor, end, '?');
  }

  *cursor = 0;
  return (UINTN)(cursor - buffer);
}

static BOOLEAN guid_equal(const EFI_GUID *a, const EFI_GUID *b) {
  const UINT64 *aa = (const UINT64 *)a, *bb = (const UINT64 *)b;
  return aa[0] == bb[0] && aa[1] == bb[1];
}
static BOOLEAN checksum_ok(const UINT8 *p, UINTN n) {
  UINT8 s = 0;
  while (n--)
    s += *p++;
  return s == 0;
}
static VOID *find_anchor(UINTN begin, UINTN end, const CHAR8 *sig, UINTN siglen,
                         UINTN step) {
  for (UINTN a = begin; a + siglen <= end; a += step) {
    BOOLEAN ok = TRUE;
    for (UINTN i = 0; i < siglen; ++i)
      if (*(volatile UINT8 *)(a + i) != (UINT8)sig[i])
        ok = FALSE;
    if (ok)
      return (VOID *)a;
  }
  return NULL;
}
static void discover_config_tables(void) {
  UINTN n = 0;
  VOID *rsdp = NULL;
  UINT16 ebda = *(volatile UINT16 *)0x40e;
  if (ebda)
    rsdp = find_anchor((UINTN)ebda << 4, ((UINTN)ebda << 4) + 1024, "RSD PTR ",
                       8, 16);
  if (!rsdp)
    rsdp = find_anchor(0xe0000, 0x100000, "RSD PTR ", 8, 16);
  if (rsdp && checksum_ok(rsdp, 20)) {
    UINT8 revision = *((UINT8 *)rsdp + 15);
    UINTN length = 20;

    if (revision >= 2) {
      UINT32 extended_length = *(UINT32 *)((UINT8 *)rsdp + 20);
      if (extended_length >= sizeof(normalized_rsdp) &&
          checksum_ok(rsdp, sizeof(normalized_rsdp)))
        length = sizeof(normalized_rsdp);
    }

    SetMem(normalized_rsdp, sizeof(normalized_rsdp), 0);
    for (UINTN i = 0; i < length; ++i)
      normalized_rsdp[i] = ((UINT8 *)rsdp)[i];

    if (revision >= 2) {
      static EFI_GUID acpi20 = ACPI_20_TABLE_GUID;
      config_tables[n].VendorGuid = acpi20;
    } else {
      static EFI_GUID acpi10 = ACPI_TABLE_GUID;
      config_tables[n].VendorGuid = acpi10;
    }
    config_tables[n++].VendorTable = normalized_rsdp;
  }
  VOID *sm3 = find_anchor(0xf0000, 0x100000, "_SM3_", 5, 16);
  VOID *sm = find_anchor(0xf0000, 0x100000, "_SM_", 4, 16);
  if (sm3) {
    UINT8 len = *((UINT8 *)sm3 + 6);
    if (checksum_ok(sm3, len)) {
      static EFI_GUID g3 = SMBIOS3_TABLE_GUID;
      config_tables[n].VendorGuid = g3;
      config_tables[n++].VendorTable = sm3;
    }
  }
  if (sm) {
    UINT8 len = *((UINT8 *)sm + 5);
    if (checksum_ok(sm, len)) {
      static EFI_GUID g2 = SMBIOS_TABLE_GUID;
      config_tables[n].VendorGuid = g2;
      config_tables[n++].VendorTable = sm;
    }
  }
  system_table.NumberOfTableEntries = n;
  system_table.ConfigurationTable = config_tables;
}

static void sort_ranges(void) {
  for (UINTN i = 1; i < nranges; ++i) {
    EFI_MEMORY_DESCRIPTOR v = ranges[i];
    UINTN j = i;
    while (j && ranges[j - 1].PhysicalStart > v.PhysicalStart) {
      ranges[j] = ranges[j - 1];
      --j;
    }
    ranges[j] = v;
  }
}

static EFI_STATUS reserve_range(EFI_PHYSICAL_ADDRESS base, UINTN pages,
                                EFI_MEMORY_TYPE type) {
  UINT64 end = base + pages * PAGE_SIZE;
  for (UINTN i = 0; i < nranges; ++i) {
    UINT64 rb = ranges[i].PhysicalStart;
    UINT64 re = rb + ranges[i].NumberOfPages * PAGE_SIZE;
    if (ranges[i].Type != EfiConventionalMemory || base < rb || end > re)
      continue;
    if (nranges + 2 >= MAX_RANGES)
      return EFI_OUT_OF_RESOURCES;
    EFI_MEMORY_DESCRIPTOR old = ranges[i];
    ranges[i].PhysicalStart = base;
    ranges[i].NumberOfPages = pages;
    ranges[i].Type = type;
    ranges[i].Attribute =
        type == EfiRuntimeServicesCode || type == EfiRuntimeServicesData
            ? EFI_MEMORY_RUNTIME
            : 0;
    if (base > rb) {
      ranges[nranges] = old;
      ranges[nranges].NumberOfPages = (base - rb) / PAGE_SIZE;
      ++nranges;
    }
    if (end < re) {
      ranges[nranges] = old;
      ranges[nranges].PhysicalStart = end;
      ranges[nranges].NumberOfPages = (re - end) / PAGE_SIZE;
      ++nranges;
    }
    sort_ranges();
    ++map_key;
    return EFI_SUCCESS;
  }
  return EFI_NOT_FOUND;
}

static EFI_STATUS EFIAPI bs_allocate_pages(EFI_ALLOCATE_TYPE kind,
                                           EFI_MEMORY_TYPE type, UINTN pages,
                                           EFI_PHYSICAL_ADDRESS *memory) {
  if (!memory || !pages)
    return EFI_INVALID_PARAMETER;
  /* The BIOS handoff and XNU's x86 boot_args use 32-bit physical fields. */
  UINT64 limit = 0xffffffffULL;
  if (kind == AllocateMaxAddress && *memory < limit)
    limit = *memory;
  if (kind == AllocateAddress)
    return reserve_range(*memory, pages, type);
  for (UINTN n = nranges; n-- > 0;) {
    if (ranges[n].Type != EfiConventionalMemory)
      continue;
    UINT64 rb = ranges[n].PhysicalStart;
    UINT64 re = rb + ranges[n].NumberOfPages * PAGE_SIZE;
    if (re > limit + 1 && limit != ~0ULL)
      re = (limit + 1) & ~(PAGE_SIZE - 1);
    if (re < rb + pages * PAGE_SIZE)
      continue;
    UINT64 base = re - pages * PAGE_SIZE;
    EFI_STATUS s = reserve_range(base, pages, type);
    if (!EFI_ERROR(s))
      *memory = base;
    return s;
  }
  return EFI_OUT_OF_RESOURCES;
}

static EFI_STATUS EFIAPI bs_free_pages(EFI_PHYSICAL_ADDRESS memory,
                                       UINTN pages) {
  for (UINTN i = 0; i < nranges; ++i) {
    if (ranges[i].PhysicalStart == memory && ranges[i].NumberOfPages == pages) {
      ranges[i].Type = EfiConventionalMemory;
      ++map_key;
      return EFI_SUCCESS;
    }
  }
  return EFI_NOT_FOUND;
}

static EFI_STATUS EFIAPI bs_get_memory_map(UINTN *size,
                                           EFI_MEMORY_DESCRIPTOR *map,
                                           UINTN *key, UINTN *desc_size,
                                           UINT32 *desc_version) {
  if (!size || !key || !desc_size || !desc_version)
    return EFI_INVALID_PARAMETER;
  UINTN needed = nranges * sizeof(EFI_MEMORY_DESCRIPTOR);
  *desc_size = sizeof(EFI_MEMORY_DESCRIPTOR);
  *desc_version = 1;
  if (!map || *size < needed) {
    *size = needed;
    return EFI_BUFFER_TOO_SMALL;
  }
  for (UINTN i = 0; i < nranges; ++i)
    map[i] = ranges[i];
  *size = needed;
  *key = map_key;
  return EFI_SUCCESS;
}

typedef struct {
  UINTN pages;
  UINT64 magic;
} PoolHeader;
static EFI_STATUS EFIAPI bs_allocate_pool(EFI_MEMORY_TYPE type, UINTN size,
                                          VOID **out) {
  if (!out)
    return EFI_INVALID_PARAMETER;
  UINTN pages = (size + sizeof(PoolHeader) + PAGE_SIZE - 1) / PAGE_SIZE;
  EFI_PHYSICAL_ADDRESS address = ~0ULL;
  EFI_STATUS s = bs_allocate_pages(AllocateMaxAddress, type, pages, &address);
  if (EFI_ERROR(s))
    return s;
  PoolHeader *h = (PoolHeader *)(UINTN)address;
  h->pages = pages;
  h->magic = 0x504f4f4c;
  *out = h + 1;
  return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI bs_free_pool(VOID *ptr) {
  if (!ptr)
    return EFI_INVALID_PARAMETER;
  PoolHeader *h = (PoolHeader *)ptr - 1;
  if (h->magic != 0x504f4f4c)
    return EFI_INVALID_PARAMETER;
  return bs_free_pages((EFI_PHYSICAL_ADDRESS)(UINTN)h, h->pages);
}

static EFI_STATUS EFIAPI bs_handle_protocol(EFI_HANDLE handle, EFI_GUID *guid,
                                            VOID **out) {
  if (!out)
    return EFI_INVALID_PARAMETER;
  if (handle == LOADER_HANDLE && guid &&
      guid->Data1 == loaded_image_guid.Data1) {
    *out = &loaded_image;
    return EFI_SUCCESS;
  }
  if (handle == legacy_storage_handle())
    return legacy_storage_protocol(guid, out);
  return EFI_NOT_FOUND;
}

static EFI_STATUS EFIAPI gop_query_mode(
    EFI_GRAPHICS_OUTPUT_PROTOCOL *self, UINT32 mode, UINTN *size,
    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION **info) {
  (void)self;
  if (!size || !info)
    return EFI_INVALID_PARAMETER;
  if (mode != 0 || !graphics_mode.MaxMode)
    return EFI_UNSUPPORTED;
  *size = sizeof(graphics_info);
  *info = &graphics_info;
  return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI gop_set_mode(EFI_GRAPHICS_OUTPUT_PROTOCOL *self,
                                       UINT32 mode) {
  (void)self;
  return mode == 0 && graphics_mode.MaxMode ? EFI_SUCCESS : EFI_UNSUPPORTED;
}

static EFI_STATUS EFIAPI gop_blt(
    EFI_GRAPHICS_OUTPUT_PROTOCOL *self,
    EFI_GRAPHICS_OUTPUT_BLT_PIXEL *buffer,
    EFI_GRAPHICS_OUTPUT_BLT_OPERATION operation, UINTN source_x,
    UINTN source_y, UINTN destination_x, UINTN destination_y, UINTN width,
    UINTN height, UINTN delta) {
  (void)self;
  (void)buffer;
  (void)operation;
  (void)source_x;
  (void)source_y;
  (void)destination_x;
  (void)destination_y;
  (void)width;
  (void)height;
  (void)delta;
  return EFI_UNSUPPORTED;
}

static EFI_STATUS EFIAPI bs_locate_handles(EFI_LOCATE_SEARCH_TYPE type,
                                           EFI_GUID *guid, VOID *key,
                                           UINTN *count, EFI_HANDLE **handles) {
  (void)type;
  (void)key;
  VOID *protocol = NULL;
  if (EFI_ERROR(legacy_storage_protocol(guid, &protocol)))
    return EFI_NOT_FOUND;
  EFI_STATUS s = bs_allocate_pool(EfiBootServicesData, sizeof(EFI_HANDLE),
                                  (VOID **)handles);
  if (EFI_ERROR(s))
    return s;
  (*handles)[0] = legacy_storage_handle();
  *count = 1;
  return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI bs_locate_protocol(EFI_GUID *guid, VOID *registration,
                                            VOID **out) {
  (void)registration;
  if (!guid || !out)
    return EFI_INVALID_PARAMETER;
  if (guid->Data1 == graphics_output_guid.Data1 && graphics_mode.MaxMode) {
    *out = &graphics_output;
    return EFI_SUCCESS;
  }
  return EFI_NOT_FOUND;
}

static EFI_STATUS EFIAPI bs_exit(EFI_HANDLE image, UINTN key) {
  (void)image;
  if (key != map_key)
    return EFI_INVALID_PARAMETER;

  /*
   * SeaBIOS is allowed to leave the PC interrupt hardware in its boot-time
   * configuration. XNU installs exception vectors before it remaps the
   * legacy IRQs, so an old IRQ0/vector-8 delivery in that window is observed
   * as a double fault and resets the machine before the trap path can log it.
   * UEFI firmware normally quiesces these sources as part of ExitBootServices.
   */
  io_out8(0x21, 0xff);
  io_out8(0xa1, 0xff);
  io_out8(0xa0, 0x20);
  io_out8(0x20, 0x20);

  /* Disable RTC update, alarm, and periodic interrupts and clear IRQ8. */
  io_out8(0x70, 0x0b);
  io_out8(0x71, (UINT8)(io_in8(0x71) & ~0x70));
  io_out8(0x70, 0x0c);
  (void)io_in8(0x71);

  return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI bs_stall(UINTN usec) {
  /*
   * Channel 2 is not used for the scheduler tick and gives us an actual
   * 1.193182 MHz reference clock. Do not derive Stall() from CPUID leaf
   * 0x16: KVM does not expose that leaf for every host CPU, and the old
   * 1 GHz fallback made the loader report a fictitious TSC frequency to XNU.
   */
  UINT8 speaker = io_in8(0x61);
  while (usec) {
    UINTN chunk = usec > 50000 ? 50000 : usec;
    UINT32 count = (UINT32)(((UINT64)chunk * 1193182ULL + 999999ULL) /
                            1000000ULL);
    if (!count)
      count = 1;

    /* Channel 2, lobyte/hibyte, mode 0, binary counter. */
    io_out8(0x61, (UINT8)(speaker & ~0x01));
    io_out8(0x43, 0xb0);
    io_out8(0x42, (UINT8)count);
    io_out8(0x42, (UINT8)(count >> 8));
    io_out8(0x61, (UINT8)((speaker & ~0x02) | 0x01));
    while (!(io_in8(0x61) & 0x20))
      __asm__ volatile("pause");

    usec -= chunk;
  }
  io_out8(0x61, speaker);
  return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI bs_crc32(VOID *data, UINTN size, UINT32 *out) {
  if (!data || !out)
    return EFI_INVALID_PARAMETER;
  UINT32 crc = ~0U;
  UINT8 *p = data;
  while (size--) {
    crc ^= *p++;
    for (int n = 0; n < 8; ++n)
      crc = (crc >> 1) ^ (0xedb88320U & -(crc & 1));
  }
  *out = ~crc;
  return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI rt_set_virtual(UINTN map_size, UINTN desc_size,
                                        UINT32 version,
                                        EFI_MEMORY_DESCRIPTOR *map) {
  (void)version;

  UINT64 runtime_base = (UINT64)(UINTN)&legacy_entry;
  for (UINTN offset = 0; offset + desc_size <= map_size; offset += desc_size) {
    EFI_MEMORY_DESCRIPTOR *descriptor =
        (EFI_MEMORY_DESCRIPTOR *)((UINT8 *)map + offset);
    UINT64 descriptor_end =
        descriptor->PhysicalStart + descriptor->NumberOfPages * PAGE_SIZE;

    if ((descriptor->Attribute & EFI_MEMORY_RUNTIME) &&
        runtime_base >= descriptor->PhysicalStart &&
        runtime_base < descriptor_end) {
      runtime_virtual_delta =
          descriptor->VirtualStart - descriptor->PhysicalStart;
      return EFI_SUCCESS;
    }
  }

  return EFI_NOT_FOUND;
}

static EFI_STATUS EFIAPI rt_convert_pointer(UINTN disposition,
                                             VOID **address) {
  (void)disposition;
  if (!address)
    return EFI_INVALID_PARAMETER;
  if (*address)
    *address = (VOID *)((UINTN)*address + runtime_virtual_delta);
  return EFI_SUCCESS;
}

static UINT8 cmos_read(UINT8 index) {
  io_out8(0x70, (UINT8)(index | 0x80));
  return io_in8(0x71);
}

static UINT8 bcd_to_binary(UINT8 value) {
  return (UINT8)((value & 0x0f) + ((value >> 4) * 10));
}

static EFI_STATUS EFIAPI rt_get_time(EFI_TIME *time,
                                      EFI_TIME_CAPABILITIES *capabilities) {
  if (!time)
    return EFI_INVALID_PARAMETER;

  while (cmos_read(0x0a) & 0x80) {
  }

  UINT8 second = cmos_read(0x00);
  UINT8 minute = cmos_read(0x02);
  UINT8 hour = cmos_read(0x04);
  UINT8 day = cmos_read(0x07);
  UINT8 month = cmos_read(0x08);
  UINT8 year = cmos_read(0x09);
  UINT8 century = cmos_read(0x32);
  UINT8 status_b = cmos_read(0x0b);

  if (!(status_b & 0x04)) {
    second = bcd_to_binary(second);
    minute = bcd_to_binary(minute);
    hour = bcd_to_binary(hour);
    day = bcd_to_binary(day);
    month = bcd_to_binary(month);
    year = bcd_to_binary(year);
    century = bcd_to_binary(century);
  }

  SetMem(time, sizeof(*time), 0);
  time->Year = (UINT16)((century ? century : 20) * 100 + year);
  time->Month = month;
  time->Day = day;
  time->Hour = hour;
  time->Minute = minute;
  time->Second = second;
  time->TimeZone = EFI_UNSPECIFIED_TIMEZONE;

  if (capabilities) {
    capabilities->Resolution = 1;
    capabilities->Accuracy = 50000000;
    capabilities->SetsToZero = FALSE;
  }
  return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI rt_set_time(EFI_TIME *time) {
  (void)time;
  return EFI_UNSUPPORTED;
}

static EFI_STATUS EFIAPI rt_get_wakeup_time(BOOLEAN *enabled,
                                             BOOLEAN *pending,
                                             EFI_TIME *time) {
  if (!enabled || !pending || !time)
    return EFI_INVALID_PARAMETER;
  *enabled = FALSE;
  *pending = FALSE;
  SetMem(time, sizeof(*time), 0);
  return EFI_UNSUPPORTED;
}

static EFI_STATUS EFIAPI rt_set_wakeup_time(BOOLEAN enable, EFI_TIME *time) {
  (void)enable;
  (void)time;
  return EFI_UNSUPPORTED;
}

void legacy_runtime_fixup(EFI_RUNTIME_SERVICES *runtime_copy) {
  if (!runtime_copy || !runtime_virtual_delta)
    return;

  UINT64 runtime_begin = (UINT64)(UINTN)&legacy_entry;
  UINT64 runtime_end = (UINT64)(UINTN)&__bss_end;
  UINT64 *function =
      (UINT64 *)((UINT8 *)runtime_copy + sizeof(EFI_TABLE_HEADER));
  UINTN function_count =
      (sizeof(EFI_RUNTIME_SERVICES) - sizeof(EFI_TABLE_HEADER)) /
      sizeof(UINT64);

  for (UINTN i = 0; i < function_count; ++i) {
    if (function[i] >= runtime_begin && function[i] < runtime_end)
      function[i] += runtime_virtual_delta;
  }

  runtime_copy->Hdr.CRC32 = 0;
  bs_crc32(runtime_copy, runtime_copy->Hdr.HeaderSize,
           &runtime_copy->Hdr.CRC32);
}
static EFI_STATUS EFIAPI rt_get_variable(CHAR16 *name, EFI_GUID *vendor,
                                         UINT32 *attrs, UINTN *size,
                                         VOID *data) {
  (void)name;
  (void)vendor;
  (void)attrs;
  (void)size;
  (void)data;
  return EFI_NOT_FOUND;
}
static EFI_STATUS EFIAPI rt_set_variable(CHAR16 *name, EFI_GUID *vendor,
                                         UINT32 attrs, UINTN size, VOID *data) {
  (void)name;
  (void)vendor;
  (void)attrs;
  (void)size;
  (void)data;
  return EFI_UNSUPPORTED;
}

static EFI_STATUS EFIAPI rt_get_next_variable(UINTN *name_size, CHAR16 *name,
                                               EFI_GUID *vendor) {
  if (!name_size || !name || !vendor)
    return EFI_INVALID_PARAMETER;
  return EFI_NOT_FOUND;
}

static EFI_STATUS EFIAPI rt_get_next_high_count(UINT32 *count) {
  static UINT32 high_count;
  if (!count)
    return EFI_INVALID_PARAMETER;
  *count = ++high_count;
  return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI rt_update_capsule(EFI_CAPSULE_HEADER **capsules,
                                            UINTN count,
                                            EFI_PHYSICAL_ADDRESS scatter) {
  (void)capsules;
  (void)count;
  (void)scatter;
  return EFI_UNSUPPORTED;
}

static EFI_STATUS EFIAPI rt_query_capsule(EFI_CAPSULE_HEADER **capsules,
                                           UINTN count, UINT64 *max_size,
                                           EFI_RESET_TYPE *reset_type) {
  (void)capsules;
  (void)count;
  (void)max_size;
  (void)reset_type;
  return EFI_UNSUPPORTED;
}

static EFI_STATUS EFIAPI rt_query_variable(UINT32 attrs, UINT64 *maximum,
                                            UINT64 *remaining,
                                            UINT64 *maximum_variable) {
  (void)attrs;
  if (!maximum || !remaining || !maximum_variable)
    return EFI_INVALID_PARAMETER;
  *maximum = 0;
  *remaining = 0;
  *maximum_variable = 0;
  return EFI_UNSUPPORTED;
}

static EFI_STATUS EFIAPI rt_reset(EFI_RESET_TYPE type, EFI_STATUS status,
                                   UINTN data_size, CHAR16 *data) {
  (void)type;
  (void)status;
  (void)data_size;
  (void)data;
  io_out8(0xcf9, 0x06);
  while (io_in8(0x64) & 0x02) {
  }
  io_out8(0x64, 0xfe);
  for (;;)
    __asm__ volatile("cli; hlt");
}

static EFI_STATUS EFIAPI con_output(SIMPLE_TEXT_OUTPUT_INTERFACE *self,
                                    CHAR16 *s) {
  (void)self;
  while (*s) {
    UINT8 character = *s > 0x7f ? '?' : (UINT8)*s;
    __asm__ volatile("outb %0, $0xe9" : : "a"(character));
    ++s;
  }
  return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI con_clear(SIMPLE_TEXT_OUTPUT_INTERFACE *self) {
  (void)self;
  return EFI_SUCCESS;
}

static void initialize_ranges(LegacyE820Entry *map, UINT32 count) {
  nranges = 0;
  for (UINT32 i = 0; i < count && nranges < MAX_RANGES; ++i) {
    UINT64 b = (map[i].base + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    UINT64 e = (map[i].base + map[i].length) & ~(PAGE_SIZE - 1);
    if (e <= b)
      continue;
    EFI_MEMORY_DESCRIPTOR *d = &ranges[nranges++];
    SetMem(d, sizeof(*d), 0);
    d->PhysicalStart = b;
    d->NumberOfPages = (e - b) / PAGE_SIZE;
    d->Type = map[i].type == 1 ? EfiConventionalMemory : EfiReservedMemoryType;
  }
  sort_ranges();

  /* Keep the bootstrap page tables and BIOS stages out of the allocator. */
  reserve_range(0x1000, (0x20000 - 0x1000) / PAGE_SIZE, EfiLoaderCode);

  /* XNU maps this range at the virtual address supplied to SVAM. */
  UINT64 runtime_begin = (UINT64)(UINTN)&legacy_entry;
  UINT64 runtime_end =
      ((UINT64)(UINTN)&__bss_end + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
  reserve_range(runtime_begin, (runtime_end - runtime_begin) / PAGE_SIZE,
                EfiRuntimeServicesCode);
}

void legacy_firmware_main(LegacyE820Entry *map, UINT32 count,
                          UINT32 boot_drive, LegacyFramebuffer *framebuffer) {
  (void)boot_drive;
  debug_string("legacy: installing EFI compatibility services\n");
  initialize_ranges(map, count);
  SetMem(&boot_services, sizeof(boot_services), 0);
  boot_services.Hdr.Signature = EFI_BOOT_SERVICES_SIGNATURE;
  boot_services.Hdr.Revision = EFI_BOOT_SERVICES_REVISION;
  boot_services.Hdr.HeaderSize = sizeof(boot_services);
  boot_services.AllocatePages = bs_allocate_pages;
  boot_services.FreePages = bs_free_pages;
  boot_services.GetMemoryMap = bs_get_memory_map;
  boot_services.AllocatePool = bs_allocate_pool;
  boot_services.FreePool = bs_free_pool;
  boot_services.HandleProtocol = bs_handle_protocol;
  boot_services.PCHandleProtocol = bs_handle_protocol;
  boot_services.LocateHandleBuffer = bs_locate_handles;
  boot_services.LocateProtocol = bs_locate_protocol;
  boot_services.ExitBootServices = bs_exit;
  boot_services.Stall = bs_stall;
  boot_services.CalculateCrc32 = bs_crc32;
  SetMem(&runtime_services, sizeof(runtime_services), 0);
  runtime_services.Hdr.Signature = EFI_RUNTIME_SERVICES_SIGNATURE;
  runtime_services.Hdr.Revision = EFI_RUNTIME_SERVICES_REVISION;
  runtime_services.Hdr.HeaderSize = sizeof(runtime_services);
  runtime_services.GetTime = rt_get_time;
  runtime_services.SetTime = rt_set_time;
  runtime_services.GetWakeupTime = rt_get_wakeup_time;
  runtime_services.SetWakeupTime = rt_set_wakeup_time;
  runtime_services.SetVirtualAddressMap = rt_set_virtual;
  runtime_services.ConvertPointer = rt_convert_pointer;
  runtime_services.GetVariable = rt_get_variable;
  runtime_services.GetNextVariableName = rt_get_next_variable;
  runtime_services.SetVariable = rt_set_variable;
  runtime_services.GetNextHighMonotonicCount = rt_get_next_high_count;
  runtime_services.ResetSystem = rt_reset;
  runtime_services.UpdateCapsule = rt_update_capsule;
  runtime_services.QueryCapsuleCapabilities = rt_query_capsule;
  runtime_services.QueryVariableInfo = rt_query_variable;
  runtime_services.Hdr.CRC32 = 0;
  bs_crc32(&runtime_services, runtime_services.Hdr.HeaderSize,
           &runtime_services.Hdr.CRC32);
  SetMem(&console_out, sizeof(console_out), 0);
  SetMem(&console_mode, sizeof(console_mode), 0);
  console_out.OutputString = con_output;
  console_out.ClearScreen = con_clear;
  console_out.Mode = &console_mode;
  SetMem(&graphics_output, sizeof(graphics_output), 0);
  SetMem(&graphics_mode, sizeof(graphics_mode), 0);
  SetMem(&graphics_info, sizeof(graphics_info), 0);
  if (framebuffer && framebuffer->valid && framebuffer->bits_per_pixel == 32) {
    graphics_info.HorizontalResolution = framebuffer->width;
    graphics_info.VerticalResolution = framebuffer->height;
    graphics_info.PixelFormat =
        framebuffer->red_position > framebuffer->blue_position
            ? PixelBlueGreenRedReserved8BitPerColor
            : PixelRedGreenBlueReserved8BitPerColor;
    graphics_info.PixelsPerScanLine = framebuffer->pixels_per_scanline;
    graphics_mode.MaxMode = 1;
    graphics_mode.Info = &graphics_info;
    graphics_mode.SizeOfInfo = sizeof(graphics_info);
    graphics_mode.FrameBufferBase = framebuffer->base;
    graphics_mode.FrameBufferSize =
        (UINTN)framebuffer->pixels_per_scanline * framebuffer->height * 4;
    graphics_output.QueryMode = gop_query_mode;
    graphics_output.SetMode = gop_set_mode;
    graphics_output.Blt = gop_blt;
    graphics_output.Mode = &graphics_mode;
    debug_string("legacy: VBE framebuffer exposed as GOP\n");
  } else {
    debug_string("legacy: no VBE framebuffer\n");
  }
  SetMem(&loaded_image, sizeof(loaded_image), 0);
  loaded_image.Revision = EFI_LOADED_IMAGE_PROTOCOL_REVISION;
  loaded_image.DeviceHandle = LOADER_HANDLE;
  loaded_image.ImageBase = (VOID *)0x20000;
  SetMem(&system_table, sizeof(system_table), 0);
  system_table.Hdr.Signature = EFI_SYSTEM_TABLE_SIGNATURE;
  system_table.Hdr.Revision = EFI_SYSTEM_TABLE_REVISION;
  system_table.Hdr.HeaderSize = sizeof(system_table);
  system_table.FirmwareVendor = L"xnu-loader BIOS shim";
  system_table.ConOut = &console_out;
  system_table.StdErr = &console_out;
  system_table.RuntimeServices = &runtime_services;
  system_table.BootServices = &boot_services;
  discover_config_tables();
  debug_string("legacy: entering shared EFI loader\n");
  EFI_STATUS storage_status = legacy_storage_init(boot_drive);
  if (EFI_ERROR(storage_status))
    debug_string("legacy: FAT32 volume unavailable\n");
  loaded_image.DeviceHandle = legacy_storage_handle();
  EFI_STATUS status = efi_main(LOADER_HANDLE, &system_table);
  debug_string("legacy: loader returned\n");
  (void)status;
  for (;;)
    __asm__ volatile("cli; hlt");
}
