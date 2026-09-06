#include "console.h"
#include "serial.h"
#include <stdarg.h>

/* Format once, then fan the result out to both the EFI console (while boot
 * services are alive) and the COM3 UART (works pre- and post-ExitBootServices,
 * so a serial cable captures the full boot on real hardware). */
static VOID log_emit(CONST CHAR16 *fmt, va_list args) {
  CHAR16 buf[512];

  VSPrint(buf, sizeof(buf), (CHAR16 *)fmt, args);

  if (ST != NULL && ST->ConOut != NULL)
    uefi_call_wrapper(ST->ConOut->OutputString, 2, ST->ConOut, buf);

#if !defined(__aarch64__)
  serial_put16(buf);
#endif
  /* AAVMF's ARM64-virt ConOut is the PL011 UART, so writing directly to the
   * UART here too duplicates every pre-ExitBootServices line. The explicit
   * serial trace path remains available after boot services are gone. */
}

VOID log_info(CONST CHAR16 *fmt, ...) {
  va_list args;
  va_start(args, fmt);
  log_emit(fmt, args);
  va_end(args);
}

VOID log_error(CONST CHAR16 *fmt, ...) {
  va_list args;
  va_start(args, fmt);
  log_emit(fmt, args);
  va_end(args);
}
