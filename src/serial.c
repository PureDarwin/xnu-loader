#include "serial.h"

/* Standard ISA I/O bases. */
#define COM1_BASE 0x3F8
#define COM3_BASE 0x3E8

/* 16550 register offsets from the I/O base. */
#define UART_THR 0 /* Transmit Holding Register (DLAB=0, write)   */
#define UART_DLL 0 /* Divisor Latch Low          (DLAB=1)         */
#define UART_IER 1 /* Interrupt Enable Register  (DLAB=0)         */
#define UART_DLM 1 /* Divisor Latch High         (DLAB=1)         */
#define UART_FCR 2 /* FIFO Control Register       (write)         */
#define UART_LCR 3 /* Line Control Register                       */
#define UART_MCR 4 /* Modem Control Register                      */
#define UART_LSR 5 /* Line Status Register                        */

#define LSR_THRE 0x20 /* Transmit Holding Register empty */

/* 115200 baud: the UART base clock is 1.8432 MHz / 16 = 115200 Hz, so the
 * 16-bit divisor for 115200 baud is exactly 1. */
#define UART_DIVISOR 1

static BOOLEAN serial_ready = FALSE;

static inline VOID io_outb(UINT16 port, UINT8 val) {
  __asm__ volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline UINT8 io_inb(UINT16 port) {
  UINT8 r;
  __asm__ volatile("inb %1, %0" : "=a"(r) : "Nd"(port));
  return r;
}

/* Program a 16550 at I/O base `b` for 115200 8N1, FIFOs on. */
static VOID uart_program(UINT16 b) {
  io_outb(b + UART_IER, 0x00);                 /* mask all interrupts        */
  io_outb(b + UART_LCR, 0x80);                 /* DLAB=1 to set baud divisor */
  io_outb(b + UART_DLL, UART_DIVISOR & 0xFF);  /* divisor low                */
  io_outb(b + UART_DLM, (UART_DIVISOR >> 8));  /* divisor high               */
  io_outb(b + UART_LCR, 0x03);                 /* DLAB=0, 8 bits, no parity, 1 stop */
  io_outb(b + UART_FCR, 0xC7);                 /* enable+clear FIFOs, 14B trigger   */
  io_outb(b + UART_MCR, 0x0B);                 /* DTR | RTS | OUT2                   */
}

static VOID uart_putc(UINT16 b, CHAR8 c) {
  while ((io_inb(b + UART_LSR) & LSR_THRE) == 0) {
    /* spin until the transmit holding register drains */
  }
  io_outb(b + UART_THR, (UINT8)c);
}

static VOID uart_puts(UINT16 b, CONST CHAR8 *s) {
  for (; *s; ++s)
    uart_putc(b, *s);
}

/* Primary serial console port. COM1 (0x3F8) is what real hardware here
 * actually exposes; COM3 was dead on the target board. */
#define SERIAL_BASE COM1_BASE

VOID serial_init(VOID) {
  uart_program(SERIAL_BASE);
  uart_puts(SERIAL_BASE, "SERIAL TEST COM1 (xnu-loader)\r\n");

  serial_ready = TRUE;
}

static VOID serial_putc(CHAR8 c) {
  uart_putc(SERIAL_BASE, c);
}

VOID serial_puts8(CONST CHAR8 *s) {
  if (!serial_ready)
    return;
  for (; *s; ++s)
    serial_putc(*s);
}

VOID serial_put16(CONST CHAR16 *s) {
  if (!serial_ready)
    return;
  /* Log format strings already carry explicit \r\n, so pass bytes straight
   * through; narrow any non-ASCII unit to '?'. */
  for (; *s; ++s)
    serial_putc((*s > 0x7F) ? (CHAR8)'?' : (CHAR8)*s);
}
