#include "console.h"
#include <stdarg.h>

VOID log_info(CONST CHAR16 *fmt, ...) {
  va_list args;
  va_start(args, fmt);
  VPrint((CHAR16 *)fmt, args);
  va_end(args);
}

VOID log_error(CONST CHAR16 *fmt, ...) {
  va_list args;
  va_start(args, fmt);
  VPrint((CHAR16 *)fmt, args);
  va_end(args);
}