{
  description = "UEFI NVMe boot test package";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
  };

  outputs = { self, nixpkgs }: let
    systems = [ "x86_64-linux" "x86_64-darwin" "aarch64-linux" "aarch64-darwin" ];
    forAllSystems = nixpkgs.lib.genAttrs systems;
  in {
    packages = forAllSystems (system: let
      pkgs = import nixpkgs { inherit system; };
    in {
      default = pkgs.callPackage ./. {};
      hello = pkgs.callPackage ./hello.nix {};
      arm64 = pkgs.pkgsCross.aarch64-multiplatform.callPackage ./. {
        arch = "aarch64";
        platform = "bcm2837";
      };
      arm64-virt = pkgs.pkgsCross.aarch64-multiplatform.callPackage ./. {
        arch = "aarch64";
        platform = "qemuvirt";
      };
      # Allwinner H616/H618 - Orange Pi Zero 3.
      arm64-sun50i = pkgs.pkgsCross.aarch64-multiplatform.callPackage ./. {
        arch = "aarch64";
        platform = "sun50i";
      };
      arm64-sc8280xp = pkgs.pkgsCross.aarch64-multiplatform.callPackage ./. {
        arch = "aarch64";
        platform = "sc8280xp";
      };
      ia32 = pkgs.pkgsi686Linux.callPackage ./. {
        arch = "x86_64";
        loaderArch = "ia32";
      };

      legacy-boot = pkgs.callPackage ./legacy { };
      # The loader as an arm64 Linux Image (U-Boot booti, QEMU -kernel).
      kernel-arm64 = pkgs.pkgsCross.aarch64-multiplatform.callPackage ./kernel/arm64.nix { };
      kernel-arm64-sun50i = pkgs.pkgsCross.aarch64-multiplatform.callPackage ./kernel/arm64.nix {
        platform = "sun50i";
      };
      kernel-arm64-sg2002 = pkgs.pkgsCross.aarch64-multiplatform.callPackage ./kernel/arm64.nix {
        platform = "sg2002";
      };
      # any board whose device tree describes it, such as the orange pi zero 4 (a733) which marks on uart0
      kernel-arm64-generic = pkgs.pkgsCross.aarch64-multiplatform.callPackage ./kernel/arm64.nix {
        platform = "generic";
      };
      kernel-arm64-a733 = pkgs.pkgsCross.aarch64-multiplatform.callPackage ./kernel/arm64.nix {
        platform = "generic";
        markUart = "0x02500000";
      };
      # msm8916 boards booted by lk2nd (the uz801 4g stick): no reachable uart, so the console is
      # a ram log at 0x9c000000 read back after the apss watchdog or a psci reset
      kernel-arm64-msm8916 = pkgs.pkgsCross.aarch64-multiplatform.callPackage ./kernel/arm64.nix {
        platform = "generic";
        kernel4k = true;
        ramlog = "0x9c000000";
        hangWdt = "0x0b017000";
        hangWdtBark = "35";   # SPI 3
        noWfi = true;
      };
      # jetson tx1 (tegra210): marks on uarta, ram log below where mainline u-boot relocates itself
      kernel-arm64-tx1 = pkgs.pkgsCross.aarch64-multiplatform.callPackage ./kernel/arm64.nix {
        platform = "generic";
        kernel4k = true;
        markUart = "0x70006000";
        ramlog = "0xf0000000";
      };
      # the same for rcm-loaded u-boot: a hang ends in a WDT0 reset into RCM, where the ram log is read
      kernel-arm64-tx1-wdt = pkgs.pkgsCross.aarch64-multiplatform.callPackage ./kernel/arm64.nix {
        platform = "generic";
        kernel4k = true;
        markUart = "0x70006000";
        ramlog = "0xf0000000";
        hangWdt = "0x60005000";
        hangWdtKind = "tegra";
        hangWdtSeconds = 120;
        hangWdtRcmSip = true;
      };
      # the loader as a riscv64 linux Image (opensbi, u-boot booti, qemu -kernel), clang and lld
      kernel-riscv64 = pkgs.callPackage ./kernel/riscv64.nix {
        compilerRt = pkgs.pkgsCross.riscv64.llvmPackages.compiler-rt;
      };
      # xnu arm32 boot shim as a Linux zImage (QEMU -kernel, U-Boot bootz)
      kernel-arm32 = pkgs.pkgsCross.armv7l-hf-multiplatform.callPackage ./arm32 { };
      # Luckfox Pico (RV1103/RV1106) and QEMU's virt Cortex-A7, fixed at build time
      arm32 = pkgs.pkgsCross.armv7l-hf-multiplatform.callPackage ./arm32 {
        platform = "rv1106";
      };
      arm32-virt = pkgs.pkgsCross.armv7l-hf-multiplatform.callPackage ./arm32 {
        platform = "qemuvirt";
      };
      # luckfox lyra (rk3506)
      arm32-rk3506 = pkgs.pkgsCross.armv7l-hf-multiplatform.callPackage ./arm32 {
        platform = "rk3506";
      };
      # allwinner a20 (sun7i), e.g. Banana Pi
      arm32-a20 = pkgs.pkgsCross.armv7l-hf-multiplatform.callPackage ./arm32 {
        platform = "a20";
      };
      # The loader as a Multiboot2 ELF kernel (GRUB etc.).
      kernel-multiboot2 = pkgs.callPackage ./kernel { };
      kernel-grub-bios = pkgs.callPackage ./kernel/grub-bios.nix {
        loaderKernel = pkgs.callPackage ./kernel { };
      };
      kernel-grub-efi = pkgs.callPackage ./kernel/grub-efi.nix {
        loaderKernel = pkgs.callPackage ./kernel { };
      };
    });
  };
}
