#ifndef XNU_LOADER_PLATFORM_H
#define XNU_LOADER_PLATFORM_H

#if defined(__aarch64__)

#if (defined(XNU_LOADER_PLATFORM_BCM2837) + \
     defined(XNU_LOADER_PLATFORM_QEMUVIRT) + \
     defined(XNU_LOADER_PLATFORM_SUN50I) + \
     defined(XNU_LOADER_PLATFORM_SG2002)) != 1
#error "exactly one XNU_LOADER_PLATFORM_* must be defined for aarch64"
#endif

/*
 * Base of physical DRAM, used both to place the kernel image and to fill
 * /chosen dram-base. QEMU virt and Allwinner H616/H618 both start RAM at
 * 0x40000000; BCM2837 starts at 0.
 */
#if defined(XNU_LOADER_PLATFORM_BCM2837)
#define XNU_LOADER_RAM_BASE 0ULL
#elif defined(XNU_LOADER_PLATFORM_SG2002)
#define XNU_LOADER_RAM_BASE 0x80000000ULL
#else
#define XNU_LOADER_RAM_BASE 0x40000000ULL
#endif

/*
 * Platforms whose arm-io node carries a uart0 child that XNU's serial_init()
 * can match. /defaults serial-device is only emitted for these; without a
 * matching node serial_init() returns early and the kernel boots silent.
 */
#if defined(XNU_LOADER_PLATFORM_SUN50I) || defined(XNU_LOADER_PLATFORM_SG2002)
// cortex-a53 boards: 4k kernel, a designware uart and a gic-400 the kernel drives itself
#define XNU_LOADER_A53_4K 1
#endif

#if defined(XNU_LOADER_PLATFORM_QEMUVIRT) || defined(XNU_LOADER_A53_4K)
#define XNU_LOADER_HAVE_DT_UART 1
#endif

/*
 * Allwinner H616/H618 (Orange Pi Zero 3).
 *
 * UART0 is a Synopsys DesignWare APB UART - 16550-compatible, but with
 * 32-bit registers on a 4-byte stride, so register index N sits at byte
 * offset N << 2.
 *
 * arm-io covers the peripheral window starting at 0x01000000, which is what
 * XNU's pe_arm_get_soc_base_phys() returns; the uart0 reg offset is relative
 * to that, so 0x01000000 + 0x04000000 = 0x05000000.
 */
#if defined(XNU_LOADER_PLATFORM_SUN50I)
#define SUN50I_SOC_BASE       0x01000000ULL
#define SUN50I_SOC_SIZE       0x07000000ULL
#define SUN50I_UART0_OFFSET   0x04000000ULL
#define SUN50I_UART0_BASE     (SUN50I_SOC_BASE + SUN50I_UART0_OFFSET)
#define SUN50I_UART0_SIZE     0x400ULL
#define SUN50I_UART0_SHIFT    2
#define SUN50I_UART0_CLOCK_HZ 24000000
#define SUN50I_UART0_BAUD     115200
#define SUN50I_GICD_OFFSET    0x02021000ULL
#define SUN50I_GICC_OFFSET    0x02022000ULL
#endif

// sophgo sg2002 (licheerv nano) on its cortex-a53, 256 MiB at 0x80000000
// uart0 is a designware apb uart like the h616's, fed from the 25 MHz crystal
#if defined(XNU_LOADER_PLATFORM_SG2002)
#define SG2002_SOC_BASE       0x01000000ULL
#define SG2002_SOC_SIZE       0x07000000ULL
#define SG2002_UART0_OFFSET   0x03140000ULL
#define SG2002_UART0_SIZE     0x100ULL
#define SG2002_UART0_CLOCK_HZ 25000000
#define SG2002_GICD_OFFSET    0x00F01000ULL
#define SG2002_GICC_OFFSET    0x00F02000ULL
#endif

// one set of names for the a53 boards so serial and the devicetree share code
#if defined(XNU_LOADER_PLATFORM_SUN50I)
#define A53_SOC_BASE          SUN50I_SOC_BASE
#define A53_SOC_SIZE          SUN50I_SOC_SIZE
#define A53_SOC_IO_TYPE       "sun50i-io"
#define A53_UART0_OFFSET      SUN50I_UART0_OFFSET
#define A53_UART0_SIZE        SUN50I_UART0_SIZE
#define A53_UART0_CLOCK_HZ    SUN50I_UART0_CLOCK_HZ
#define A53_GICD_OFFSET       SUN50I_GICD_OFFSET
#define A53_GICC_OFFSET       SUN50I_GICC_OFFSET
#elif defined(XNU_LOADER_PLATFORM_SG2002)
#define A53_SOC_BASE          SG2002_SOC_BASE
#define A53_SOC_SIZE          SG2002_SOC_SIZE
#define A53_SOC_IO_TYPE       "sg2002-io"
#define A53_UART0_OFFSET      SG2002_UART0_OFFSET
#define A53_UART0_SIZE        SG2002_UART0_SIZE
#define A53_UART0_CLOCK_HZ    SG2002_UART0_CLOCK_HZ
#define A53_GICD_OFFSET       SG2002_GICD_OFFSET
#define A53_GICC_OFFSET       SG2002_GICC_OFFSET
#endif
#if defined(XNU_LOADER_A53_4K)
#define A53_UART0_BASE        (A53_SOC_BASE + A53_UART0_OFFSET)
#define A53_UART0_SHIFT       2
#define A53_UART0_BAUD        115200
#endif

#endif /* __aarch64__ */

#endif /* XNU_LOADER_PLATFORM_H */
