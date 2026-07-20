# Makefile for the MayteraOS UEFI bootloader
#
# Builds BOOTX64.EFI, a gnu-efi-based UEFI application that loads
# /boot/kernel.elf from the filesystem it was booted from, builds a
# boot_info_t structure (memory map, framebuffer, ACPI RSDP) matching
# kernel/boot_info.h, and jumps to the kernel entry point with the
# boot_info pointer in rdi (System V AMD64 ABI, first argument).
#
# Dependencies (Debian/Ubuntu package names):
#   gcc-x86-64-linux-gnu (or native gcc)
#   binutils
#   gnu-efi           # provides /usr/include/efi and /usr/lib/elf_x86_64_efi.lds
#
# Usage:
#   make              # builds BOOTX64.EFI and hello.efi
#   make bootloader   # builds only BOOTX64.EFI
#   make hello        # builds only hello.efi (minimal smoke test)
#   make clean        # removes build artifacts

CC      = gcc
LD      = ld
OBJCOPY = objcopy

ARCH     = x86_64
EFI_ARCH = x86_64

# gnu-efi paths (adjust if your distribution installs them elsewhere)
EFI_INC = /usr/include/efi
EFI_LIB = /usr/lib
EFI_CRT = /usr/lib
EFI_LDS = /usr/lib/elf_$(EFI_ARCH)_efi.lds

CFLAGS = -ffreestanding -fno-stack-protector -fpic \
         -fshort-wchar -mno-red-zone -I$(EFI_INC) \
         -I$(EFI_INC)/$(ARCH) -DEFI_FUNCTION_WRAPPER \
         -Wall -Wextra

LDFLAGS = -shared -Bsymbolic -L$(EFI_LIB) \
          -T $(EFI_LDS) -nostdlib

BOOTLOADER_TARGET = BOOTX64.EFI
HELLO_TARGET      = hello.efi

BOOTLOADER_OBJ = bootloader.o
BOOTLOADER_SO  = bootloader.so
HELLO_OBJ      = hello.o
HELLO_SO       = hello.so

OBJCOPY_SECTIONS = -j .text -j .sdata -j .data -j .dynamic \
                   -j .dynsym -j .rel -j .rela -j .reloc

.PHONY: all bootloader hello clean

all: $(BOOTLOADER_TARGET) $(HELLO_TARGET)

bootloader: $(BOOTLOADER_TARGET)
hello:      $(HELLO_TARGET)

$(BOOTLOADER_TARGET): $(BOOTLOADER_SO)
	@echo "Converting bootloader.so to EFI..."
	$(OBJCOPY) $(OBJCOPY_SECTIONS) --target=efi-app-$(EFI_ARCH) $< $@
	@ls -lh $@

$(HELLO_TARGET): $(HELLO_SO)
	@echo "Converting hello.so to EFI..."
	$(OBJCOPY) $(OBJCOPY_SECTIONS) --target=efi-app-$(EFI_ARCH) $< $@
	@ls -lh $@

$(BOOTLOADER_SO): $(BOOTLOADER_OBJ)
	@echo "Linking bootloader..."
	$(LD) $(LDFLAGS) $< $(EFI_CRT)/crt0-efi-$(EFI_ARCH).o -o $@ -lefi -lgnuefi

$(HELLO_SO): $(HELLO_OBJ)
	@echo "Linking hello..."
	$(LD) $(LDFLAGS) $< $(EFI_CRT)/crt0-efi-$(EFI_ARCH).o -o $@ -lefi -lgnuefi

$(BOOTLOADER_OBJ): bootloader.c
	@echo "Compiling bootloader.c..."
	$(CC) $(CFLAGS) -c $< -o $@

$(HELLO_OBJ): hello.c
	@echo "Compiling hello.c..."
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f *.o *.so $(BOOTLOADER_TARGET) $(HELLO_TARGET)
