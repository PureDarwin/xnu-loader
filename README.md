# xnu-loader

A GNU-EFI UEFI application (`BOOTX64.EFI`) that boots the XNU/Darwin kernel on
x86-64 QEMU. It reads the kernel Mach-O (or a `kc-tools` kernel collection) from
the ESP, builds the `boot_args` struct XNU's `pstart` expects, exits EFI boot
services, copies segments to their physical addresses, then jumps to the kernel
entry via a 64->32-bit mode switch.

## Building
Nix is preferred, but CMake works just fine.
`nix build` will create a `result` folder; the binary is `result/xnu-loader.efi`.

Deploy it to the ESP with:
`cp result/xnu-loader.efi esp/EFI/BOOT/BOOTX64.EFI`

CMake directly needs `gnu-efi` installed and `GNU_EFI_DIR` set.

## Why?
PureDarwin.

### Why is this so hacky?
I made it to work, not to be clean.
