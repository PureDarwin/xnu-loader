{ stdenv
, lib
, cmake
, coreutils
, dosfstools
, gnu-efi
, mtools
, arch ? "x86_64"
, qemuVirt ? false
}:

assert arch == "x86_64" || arch == "aarch64";

let
  # UEFI's spec-mandated removable-media fallback path name differs per
  # arch (BOOTX64.EFI, BOOTAA64.EFI, ...) - real firmware only looks for
  # its own arch's name here.
  bootFileName = if arch == "x86_64" then "BOOTX64.EFI" else "BOOTAA64.EFI";
in
stdenv.mkDerivation rec {
  pname = "xnu-loader";
  version = "0.1";

  src = ./.;

  nativeBuildInputs = [
    cmake
    coreutils
    dosfstools
    gnu-efi
    mtools
  ];

  cmakeFlags = [
    "-DGNU_EFI_DIR=${gnu-efi}"
    "-DARCH=${arch}"
  ] ++ lib.optional qemuVirt "-DXNU_LOADER_QEMU_VIRT=ON";

  installPhase = ''
    runHook preInstall

    mkdir -p $out
    cp xnu-loader.efi $out/xnu-loader.efi

    mkdir -p $out/img/EFI/BOOT
    cp xnu-loader.efi $out/img/EFI/BOOT/${bootFileName}

    dd if=/dev/zero of=$out/xnu-loader.img bs=1M count=64
    mkfs.vfat -F 32 $out/xnu-loader.img
    mmd -i $out/xnu-loader.img ::EFI
    mmd -i $out/xnu-loader.img ::EFI/BOOT
    mcopy -i $out/xnu-loader.img $out/img/EFI/BOOT/${bootFileName} ::EFI/BOOT/${bootFileName}

    runHook postInstall
  '';

  meta = with lib; {
    description = "GNU-EFI XNU loader experiment";
    platforms = platforms.linux;
  };
}