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
    });
  };
}
