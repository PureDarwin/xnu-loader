cmake_minimum_required(VERSION 3.16)
project(xnu_loader C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(NOT DEFINED GNU_EFI_DIR)
  message(FATAL_ERROR "GNU_EFI_DIR must be set")
endif()

set(GNU_EFI_INC
  ${GNU_EFI_DIR}/include
  ${GNU_EFI_DIR}/include/efi
  ${GNU_EFI_DIR}/include/efi/x86_64
  ${GNU_EFI_DIR}/include/efi/protocol
)

set(GNU_EFI_LIB_DIR ${GNU_EFI_DIR}/lib)
set(GNU_EFI_LDS ${GNU_EFI_LIB_DIR}/elf_x86_64_efi.lds)
set(GNU_EFI_CRT ${GNU_EFI_LIB_DIR}/crt0-efi-x86_64.o)

set(SOURCES
  src/main.c
  src/app.c
  src/fileio.c
  src/console.c
  src/macho.c
)

add_library(loader_objs OBJECT ${SOURCES})

target_include_directories(loader_objs PRIVATE
  ${GNU_EFI_INC}
  ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_options(loader_objs PRIVATE
  -fno-stack-protector
  -fshort-wchar
  -fPIC
  -mno-red-zone
  -maccumulate-outgoing-args
  -DGNU_EFI_USE_MS_ABI
  -DEFI_FUNCTION_WRAPPER
)

add_custom_command(
  OUTPUT xnu-loader.so
  COMMAND_EXPAND_LISTS
  COMMAND ${CMAKE_LINKER}
          -nostdlib
          -znocombreloc
          -T ${GNU_EFI_LDS}
          -shared
          -Bsymbolic
          ${GNU_EFI_CRT}
          $<TARGET_OBJECTS:loader_objs>
          -L${GNU_EFI_LIB_DIR}
          -l:libgnuefi.a
          -l:libefi.a
          -o xnu-loader.so
  DEPENDS
          ${GNU_EFI_CRT}
          loader_objs
  VERBATIM
)

add_custom_command(
  OUTPUT xnu-loader.efi
  COMMAND ${CMAKE_OBJCOPY}
          -j .text
          -j .sdata
          -j .data
          -j .dynamic
          -j .dynsym
          -j .rel
          -j .rela
          -j .reloc
          --target=efi-app-x86_64
          xnu-loader.so
          xnu-loader.efi
  DEPENDS xnu-loader.so
  VERBATIM
)

add_custom_target(xnu-loader ALL DEPENDS xnu-loader.efi)