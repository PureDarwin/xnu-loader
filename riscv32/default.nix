{ stdenvNoCC
, lib
, llvmPackages
, dtc
}:

# xnu riscv32 boot shim as a riscv linux Image: qemu -kernel over opensbi, u-boot booti.
# The kernel Mach-O, boot-args.txt and an optional ramdisk.img come from a newc cpio
# initrd. Built by clang and lld for a bare rv32 target, linked where the Image loads.
let
  clang = llvmPackages.clang-unwrapped;
  lld = llvmPackages.lld;
  bintools = llvmPackages.bintools-unwrapped;
in
stdenvNoCC.mkDerivation {
  pname = "xnu-loader-riscv32";
  version = "0.1";
  src = ./.;
  dontConfigure = true;
  dontFixup = true;
  nativeBuildInputs = [ dtc ];

  buildPhase = ''
    runHook preBuild
    cc="${clang}/bin/clang --target=riscv32-unknown-elf -march=rv32imac_zicsr_zifencei -mabi=ilp32"
    cc="$cc -mcmodel=medany -ffreestanding -fno-builtin -fno-stack-protector -nostdlib -O2 -Wall"
    $cc -c entry.S -o entry.o
    $cc -c boot.c -o boot.o
    ${lld}/bin/ld.lld -nostdlib -static -z max-page-size=0x1000 -T linker.ld \
      -o boot-rv32.elf entry.o boot.o
    ${bintools}/bin/llvm-objcopy -O binary -R .bss boot-rv32.elf Image

    # the esp32-p4 flavour: m-mode, hp sram, a built in device tree
    dtc -I dts -O dtb -o esp32p4.dtb esp32p4.dts
    $cc -DBOOT_ESP32P4 -c entry_esp32p4.S -o entry_esp32p4.o
    $cc -DBOOT_ESP32P4 -c boot.c -o boot_esp32p4.o
    ${lld}/bin/ld.lld -nostdlib -static -z max-page-size=0x1000 -T linker_esp32p4.ld \
      -o boot-esp32p4.elf entry_esp32p4.o boot_esp32p4.o
    runHook postBuild
  '';

  installPhase = ''
    runHook preInstall
    install -Dm644 boot-rv32.elf $out/boot/xnu-loader-riscv32.elf
    install -Dm644 Image $out/boot/xnu-loader-riscv32.Image
    install -Dm644 boot-esp32p4.elf $out/boot/xnu-loader-esp32p4.elf
    runHook postInstall
  '';

  meta.description = "xnu riscv32 boot shim as a riscv linux Image";
}
