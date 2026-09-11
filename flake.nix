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
      };
      arm64-virt = pkgs.pkgsCross.aarch64-multiplatform.callPackage ./. {
        arch = "aarch64";
        qemuVirt = true;
      };
      # 32-bit UEFI on a 64-bit CPU: the EFI binary must be IA32 while the
      # kernel it boots is x86_64. Built from the i686 package set so libgcc,
      # gnu-efi and binutils are all 32-bit; a 64-bit toolchain has no 32-bit
      # libgcc and its ld defaults to the wrong emulation.
      ia32 = pkgs.pkgsi686Linux.callPackage ./. {
        arch = "x86_64";
        loaderArch = "ia32";
      };

      legacy-boot = pkgs.callPackage ./legacy { };
    });
  };
}
