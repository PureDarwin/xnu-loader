#include "serial.h"
#include "platform.h"

static BOOLEAN serial_ready = FALSE;

#if defined(PD_ARCH_X86)

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

#define UART_TX_SPIN_LIMIT 100000u

static VOID uart_putc(UINT16 b, CHAR8 c) {
  UINT32 spins = 0;
  while ((io_inb(b + UART_LSR) & LSR_THRE) == 0) {
    if (++spins >= UART_TX_SPIN_LIMIT)
      return;  /* drop the byte rather than wedge the caller */
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

VOID serial_reinit(VOID) {
  uart_program(SERIAL_BASE);
  serial_ready = TRUE;
}

static VOID serial_putc(CHAR8 c) {
  uart_putc(SERIAL_BASE, c);
}

#elif defined(__aarch64__) && defined(XNU_LOADER_PLATFORM_QEMUVIRT)

#define QEMUVIRT_UART_BASE 0x09000000ULL
#define PL011_DR  (*(volatile UINT32 *)(QEMUVIRT_UART_BASE + 0x00))
#define PL011_FR  (*(volatile UINT32 *)(QEMUVIRT_UART_BASE + 0x18))
#define PL011_FR_TXFF (1U << 5)

static VOID uart_putc(CHAR8 c) {
  UINT32 spins = 0;
  while ((PL011_FR & PL011_FR_TXFF) != 0) {
    if (++spins >= 100000u)
      return;  /* bounded: see the x86 uart_putc comment */
  }
  PL011_DR = (UINT32)(UINT8)c;
}

VOID serial_init(VOID) {
  /* QEMU's virt PL011 comes up already enabled by firmware/reset; just
   * start using it directly. */
  serial_ready = TRUE;
}

VOID serial_reinit(VOID) {
  serial_ready = TRUE;
}

static VOID serial_putc(CHAR8 c) {
  uart_putc(c);
}

#elif defined(__aarch64__) && defined(XNU_LOADER_PLATFORM_SUN50I)

/* Allwinner H616/H618 UART0: a Synopsys DesignWare APB UART, 16550-compatible
 * but with 32-bit registers on a 4-byte stride, so register index N lives at
 * base + (N << 2). U-Boot leaves it running at 115200 8N1; serial_init()
 * reprograms it anyway so the loader does not depend on that. */
#define SUN50I_UART_REG(n) \
  (*(volatile UINT32 *)(SUN50I_UART0_BASE + ((UINT64)(n) << SUN50I_UART0_SHIFT)))

#define SUN50I_UART_THR SUN50I_UART_REG(0)  /* tx holding      (DLAB=0) */
#define SUN50I_UART_DLL SUN50I_UART_REG(0)  /* divisor low     (DLAB=1) */
#define SUN50I_UART_IER SUN50I_UART_REG(1)  /* irq enable      (DLAB=0) */
#define SUN50I_UART_DLH SUN50I_UART_REG(1)  /* divisor high    (DLAB=1) */
#define SUN50I_UART_FCR SUN50I_UART_REG(2)  /* FIFO control    (write)  */
#define SUN50I_UART_LCR SUN50I_UART_REG(3)  /* line control             */
#define SUN50I_UART_LSR SUN50I_UART_REG(5)  /* line status              */
#define SUN50I_UART_USR SUN50I_UART_REG(31) /* DesignWare status (0x7c) */

#define SUN50I_UART_LSR_THRE 0x20 /* transmit holding register empty */
#define SUN50I_UART_LCR_8N1  0x03
#define SUN50I_UART_LCR_DLAB 0x80
#define SUN50I_UART_FCR_INIT 0x07 /* enable FIFOs, clear rx and tx    */

static VOID uart_putc(CHAR8 c) {
  UINT32 spins = 0;
  while ((SUN50I_UART_LSR & SUN50I_UART_LSR_THRE) == 0) {
    if (++spins >= 100000u)
      return;  /* bounded: see the x86 uart_putc comment */
  }
  SUN50I_UART_THR = (UINT32)(UINT8)c;
}

VOID serial_init(VOID) {
  /* Round to nearest: exact divisor is 13.02 at 24MHz/115200. */
  CONST UINT32 divisor = (SUN50I_UART0_CLOCK_HZ + (8 * SUN50I_UART0_BAUD)) /
                         (16 * SUN50I_UART0_BAUD);
  UINT32 spins = 0;

  SUN50I_UART_IER = 0;
  SUN50I_UART_FCR = SUN50I_UART_FCR_INIT;

  /* A DesignWare UART discards LCR writes while it is busy, which would drop
   * the divisor below and silently leave the port at whatever rate firmware
   * set. Drain the transmitter, then clear any latched busy-detect via USR. */
  while ((SUN50I_UART_LSR & SUN50I_UART_LSR_THRE) == 0) {
    if (++spins >= 100000u)
      break;
  }
  (VOID)SUN50I_UART_USR;

  SUN50I_UART_LCR = SUN50I_UART_LCR_DLAB;
  SUN50I_UART_DLL = divisor & 0xFF;
  SUN50I_UART_DLH = (divisor >> 8) & 0xFF;
  SUN50I_UART_LCR = SUN50I_UART_LCR_8N1;
  (VOID)SUN50I_UART_USR;

  serial_ready = TRUE;
}

VOID serial_reinit(VOID) {
  serial_init();
}

static VOID serial_putc(CHAR8 c) {
  uart_putc(c);
}

#elif defined(__aarch64__) && defined(XNU_LOADER_PLATFORM_BCM2837)

#define BCM2837_PERIPHERAL_BASE 0x3F000000ULL
#define AUX_ENABLES     (*(volatile UINT32 *)(BCM2837_PERIPHERAL_BASE + 0x215004))
#define AUX_MU_IO_REG   (*(volatile UINT32 *)(BCM2837_PERIPHERAL_BASE + 0x215040))
#define AUX_MU_IER_REG  (*(volatile UINT32 *)(BCM2837_PERIPHERAL_BASE + 0x215044))
#define AUX_MU_LCR_REG  (*(volatile UINT32 *)(BCM2837_PERIPHERAL_BASE + 0x21504C))
#define AUX_MU_LSR_REG  (*(volatile UINT32 *)(BCM2837_PERIPHERAL_BASE + 0x215054))
#define AUX_MU_CNTL_REG (*(volatile UINT32 *)(BCM2837_PERIPHERAL_BASE + 0x215060))
#define AUX_MU_BAUD_REG (*(volatile UINT32 *)(BCM2837_PERIPHERAL_BASE + 0x215068))

static VOID uart_putc(CHAR8 c) {
  UINT32 spins = 0;
  while ((AUX_MU_LSR_REG & 0x20) == 0) {
    if (++spins >= 100000u)
      return;  /* bounded: see the x86 uart_putc comment */
  }
  AUX_MU_IO_REG = (UINT32)(UINT8)c;
}

VOID serial_init(VOID) {
  AUX_ENABLES |= 1;      /* enable mini UART */
  AUX_MU_IER_REG = 0;    /* mask interrupts */
  AUX_MU_LCR_REG = 3;    /* 8-bit mode */
  AUX_MU_CNTL_REG = 0;   /* disable tx/rx while reprogramming */
  AUX_MU_BAUD_REG = 270; /* 115200 baud @ 250MHz core clock */
  AUX_MU_CNTL_REG = 3;   /* enable tx+rx */

  serial_ready = TRUE;
}

VOID serial_reinit(VOID) {
  serial_init();
}

static VOID serial_putc(CHAR8 c) {
  uart_putc(c);
}

#else
#error "serial.c: unsupported architecture"
#endif

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

VOID serial_puthex(UINT64 v) {
  CONST CHAR8 *digits = (CONST CHAR8 *)"0123456789ABCDEF";
  CHAR8 buf[17];
  int i = 16;

  buf[16] = 0;
  if (v == 0) {
    serial_puts8((CONST CHAR8 *)"0");
    return;
  }
  while (v != 0 && i > 0) {
    buf[--i] = digits[v & 0xF];
    v >>= 4;
  }
  serial_puts8(&buf[i]);
}

VOID serial_trace(CONST CHAR8 *tag, UINT64 v) {
  serial_puts8((CONST CHAR8 *)"[EBS] ");
  serial_puts8(tag);
  serial_puts8((CONST CHAR8 *)" 0x");
  serial_puthex(v);
  serial_puts8((CONST CHAR8 *)"\r\n");
}

VOID serial_mark(CONST CHAR8 *tag) {
  serial_puts8((CONST CHAR8 *)"[EBS] ");
  serial_puts8(tag);
  serial_puts8((CONST CHAR8 *)"\r\n");
}
