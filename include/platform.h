#ifndef XNU_LOADER_PLATFORM_H
#define XNU_LOADER_PLATFORM_H

#if defined(__aarch64__)

#if (defined(XNU_LOADER_PLATFORM_BCM2837) + \
     defined(XNU_LOADER_PLATFORM_QEMUVIRT) + \
     defined(XNU_LOADER_PLATFORM_SUN50I)) != 1
#error "exactly one XNU_LOADER_PLATFORM_* must be defined for aarch64"
#endif

/*
 * Base of physical DRAM, used both to place the kernel image and to fill
 * /chosen dram-base. QEMU virt and Allwinner H616/H618 both start RAM at
 * 0x40000000; BCM2837 starts at 0.
 */
#if defined(XNU_LOADER_PLATFORM_BCM2837)
#define XNU_LOADER_RAM_BASE 0ULL
#else
#define XNU_LOADER_RAM_BASE 0x40000000ULL
#endif

/*
 * Platforms whose arm-io node carries a uart0 child that XNU's serial_init()
 * can match. /defaults serial-device is only emitted for these; without a
 * matching node serial_init() returns early and the kernel boots silent.
 */
#if defined(XNU_LOADER_PLATFORM_QEMUVIRT) || defined(XNU_LOADER_PLATFORM_SUN50I)
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
#endif

#endif /* __aarch64__ */

#endif /* XNU_LOADER_PLATFORM_H */
