cmake_minimum_required(VERSION 3.16)
project(xnu_loader C)

enable_language(ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(NOT DEFINED GNU_EFI_DIR)
  message(FATAL_ERROR "GNU_EFI_DIR must be set")
endif()

set(ARCH x86_64)

set(GNU_EFI_INC
  ${GNU_EFI_DIR}/include
  ${GNU_EFI_DIR}/include/efi
  ${GNU_EFI_DIR}/include/efi/${ARCH}
  ${GNU_EFI_DIR}/include/protocol
  ${GNU_EFI_DIR}/include/efi/protocol
)

set(GNU_EFI_LIB_DIR ${GNU_EFI_DIR}/lib)
set(GNU_EFI_LDS ${GNU_EFI_LIB_DIR}/elf_${ARCH}_efi.lds)
set(GNU_EFI_CRT ${GNU_EFI_LIB_DIR}/crt0-efi-${ARCH}.o)

execute_process(
  COMMAND ${CMAKE_C_COMPILER} -m64 -print-libgcc-file-name
  OUTPUT_VARIABLE LIBGCC_FILE
  OUTPUT_STRIP_TRAILING_WHITESPACE
)

message(STATUS "libgcc: ${LIBGCC_FILE}")

set(SOURCES
  src/main.c
  src/app.c
  src/boot.c
  src/console.c
  src/devtree.c
  src/fileio.c
  src/jump.S
  src/lowmem.c
  src/macho.c
  src/serial.c
)

add_library(loader_objs OBJECT ${SOURCES})

target_include_directories(loader_objs PRIVATE
  ${GNU_EFI_INC}
  ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_options(loader_objs PRIVATE
  -ffreestanding
  -fno-stack-protector
  -fno-stack-check
  -funsigned-char
  -fshort-wchar
  -fpie
  -mno-red-zone
  -maccumulate-outgoing-args
  -mno-avx
  -Wall
  -Wextra
  -Wno-pointer-sign
  -ggdb
)

target_compile_definitions(loader_objs PRIVATE
  EFI_FUNCTION_WRAPPER
  CONFIG_x86_64
)

add_custom_command(
  OUTPUT xnu-loader.so
  COMMAND_EXPAND_LISTS
  COMMAND ${CMAKE_LINKER}
          -nostdlib
          --warn-common
          --no-undefined
          --fatal-warnings
          --no-warn-rwx-segments
          -znocombreloc
          -shared
          -Bsymbolic
          -L${GNU_EFI_LIB_DIR}
          ${GNU_EFI_CRT}
          $<TARGET_OBJECTS:loader_objs>
          -o xnu-loader.so
          -lgnuefi
          -lefi
          -T ${GNU_EFI_LDS}
          $<$<BOOL:${LIBGCC_FILE}>:${LIBGCC_FILE}>
  DEPENDS loader_objs
  VERBATIM
)

add_custom_command(
  OUTPUT xnu-loader.efi
  COMMAND ${CMAKE_OBJCOPY}
          -j .text
          -j .sdata
          -j .data
          -j .rodata
          -j .dynamic
          -j .dynsym
          -j .rel
          -j .rela
          -j .reloc
          --target=efi-app-${ARCH}
          xnu-loader.so xnu-loader.efi
  DEPENDS xnu-loader.so
  VERBATIM
)

add_custom_target(xnu-loader ALL DEPENDS xnu-loader.efi)