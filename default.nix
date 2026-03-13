{ stdenv
, lib
, cmake
, coreutils
, dosfstools
, gnu-efi
, mtools
}:

stdenv.mkDerivation rec {
  pname = "uefi-xnu-loader";
  version = "0.1";

  src = ./.;

  nativeBuildInputs = [
    cmake
    coreutils
    dosfstools
    gnu-efi
    mtools
  ];

  dontConfigure = false;

  cmakeFlags = [
    "-DGNU_EFI_DIR=${gnu-efi}"
  ];

  installPhase = ''
    runHook preInstall

    mkdir -p $out/bin
    cp xnu-loader.efi $out/bin/xnu-loader.efi

    dd if=/dev/zero of=$out/xnu-loader.img bs=1M count=64
    mkfs.vfat -F 32 $out/xnu-loader.img
    mmd -i $out/xnu-loader.img ::EFI
    mmd -i $out/xnu-loader.img ::EFI/BOOT

    mcopy -i $out/xnu-loader.img $out/bin/xnu-loader.efi ::EFI/BOOT/BOOTX64.EFI

    if [ -f kernel ]; then
      mcopy -i $out/xnu-loader.img kernel ::EFI/BOOT/kernel
    fi

    runHook postInstall
  '';

  meta = with lib; {
    description = "GNU-EFI XNU loader experiment";
    platforms = platforms.linux;
  };
}