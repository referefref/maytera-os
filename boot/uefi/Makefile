# Makefile for UEFI Hello World Test
# MayteraOS UEFI Development

# Compiler and tools
CC = gcc
LD = ld
OBJCOPY = objcopy

# Architecture
ARCH = x86_64
EFI_ARCH = x86_64

# GNU-EFI paths
EFI_INC = /usr/include/efi
EFI_LIB = /usr/lib
EFI_CRT = /usr/lib
EFI_LDS = /usr/lib/elf_$(EFI_ARCH)_efi.lds

# Compiler flags
CFLAGS = -ffreestanding -fno-stack-protector -fpic \
         -fshort-wchar -mno-red-zone -I$(EFI_INC) \
         -I$(EFI_INC)/$(ARCH) -DEFI_FUNCTION_WRAPPER \
         -Wall -Wextra

# Linker flags
LDFLAGS = -shared -Bsymbolic -L$(EFI_LIB) \
          -T $(EFI_LDS) -nostdlib

# Build targets
HELLO_TARGET = hello.efi
BOOTLOADER_TARGET = BOOTX64.EFI
HELLO_OBJ = hello.o
HELLO_SO = hello.so
BOOTLOADER_OBJ = bootloader.o
BOOTLOADER_SO = bootloader.so

.PHONY: all clean test hello bootloader kernel

all: $(HELLO_TARGET) $(BOOTLOADER_TARGET)

hello: $(HELLO_TARGET)

bootloader: $(BOOTLOADER_TARGET)

$(HELLO_TARGET): $(HELLO_SO)
	@echo "Converting hello.so to EFI..."
	$(OBJCOPY) -j .text -j .sdata -j .data -j .dynamic \
	           -j .dynsym  -j .rel -j .rela -j .reloc \
	           --target=efi-app-$(EFI_ARCH) $(HELLO_SO) $(HELLO_TARGET)
	@echo "Build complete: $(HELLO_TARGET)"
	@ls -lh $(HELLO_TARGET)

$(BOOTLOADER_TARGET): $(BOOTLOADER_SO)
	@echo "Converting bootloader.so to EFI..."
	$(OBJCOPY) -j .text -j .sdata -j .data -j .dynamic \
	           -j .dynsym  -j .rel -j .rela -j .reloc \
	           --target=efi-app-$(EFI_ARCH) $(BOOTLOADER_SO) $(BOOTLOADER_TARGET)
	@echo "Build complete: $(BOOTLOADER_TARGET)"
	@ls -lh $(BOOTLOADER_TARGET)

$(HELLO_SO): $(HELLO_OBJ)
	@echo "Linking hello..."
	$(LD) $(LDFLAGS) $(HELLO_OBJ) \
	      $(EFI_CRT)/crt0-efi-$(EFI_ARCH).o \
	      -o $(HELLO_SO) -lefi -lgnuefi
	@echo "Linked: $(HELLO_SO)"

$(BOOTLOADER_SO): $(BOOTLOADER_OBJ)
	@echo "Linking bootloader..."
	$(LD) $(LDFLAGS) $(BOOTLOADER_OBJ) \
	      $(EFI_CRT)/crt0-efi-$(EFI_ARCH).o \
	      -o $(BOOTLOADER_SO) -lefi -lgnuefi
	@echo "Linked: $(BOOTLOADER_SO)"

$(HELLO_OBJ): hello.c
	@echo "Compiling hello.c..."
	$(CC) $(CFLAGS) -c hello.c -o $(HELLO_OBJ)

$(BOOTLOADER_OBJ): bootloader.c
	@echo "Compiling bootloader.c..."
	$(CC) $(CFLAGS) -c bootloader.c -o $(BOOTLOADER_OBJ)

clean:
	@echo "Cleaning build artifacts..."
	rm -f *.o *.so *.EFI
	@echo "Clean complete."

# Kernel path
KERNEL_ELF = ../kernel/kernel.elf

# Build kernel first
kernel:
	@echo "Building kernel..."
	$(MAKE) -C ../kernel

# Test in QEMU with UEFI firmware
test: $(BOOTLOADER_TARGET) kernel
	@echo "Testing MayteraOS in QEMU with OVMF..."
	@echo "Creating test disk image..."
	@dd if=/dev/zero of=boot_disk.img bs=1M count=50 2>/dev/null
	@parted boot_disk.img -s mklabel gpt
	@parted boot_disk.img -s mkpart ESP fat32 1MiB 100%
	@parted boot_disk.img -s set 1 esp on
	@echo "Formatting ESP partition and installing bootloader + kernel..."
	@LOOP=$$(losetup -fP --show boot_disk.img); \
	mkfs.fat -F32 $${LOOP}p1 >/dev/null 2>&1; \
	mkdir -p /tmp/maytera_boot; \
	mount $${LOOP}p1 /tmp/maytera_boot; \
	mkdir -p /tmp/maytera_boot/EFI/BOOT; \
	mkdir -p /tmp/maytera_boot/boot; \
	cp $(BOOTLOADER_TARGET) /tmp/maytera_boot/EFI/BOOT/; \
	cp $(KERNEL_ELF) /tmp/maytera_boot/boot/kernel.elf; \
	sync; \
	umount /tmp/maytera_boot; \
	losetup -d $$LOOP; \
	rmdir /tmp/maytera_boot
	@echo "Booting in QEMU..."
	@echo "Press Ctrl+A then X to exit QEMU"
	@sleep 1
	timeout 30 qemu-system-x86_64 -bios /usr/share/ovmf/OVMF.fd \
	                   -drive file=boot_disk.img,format=raw \
	                   -m 2G \
	                   -serial mon:stdio \
	                   -display none || true

# Test hello world
test-hello: $(HELLO_TARGET)
	@echo "Testing $(HELLO_TARGET) in QEMU..."
	@dd if=/dev/zero of=hello_disk.img bs=1M count=50 2>/dev/null
	@parted hello_disk.img -s mklabel gpt
	@parted hello_disk.img -s mkpart ESP fat32 1MiB 100%
	@parted hello_disk.img -s set 1 esp on
	@LOOP=$$(losetup -fP --show hello_disk.img); \
	mkfs.fat -F32 $${LOOP}p1 >/dev/null 2>&1; \
	mkdir -p /tmp/hello_esp; \
	mount $${LOOP}p1 /tmp/hello_esp; \
	mkdir -p /tmp/hello_esp/EFI/BOOT; \
	cp $(HELLO_TARGET) /tmp/hello_esp/EFI/BOOT/BOOTX64.EFI; \
	sync; \
	umount /tmp/hello_esp; \
	losetup -d $$LOOP; \
	rmdir /tmp/hello_esp
	@echo "Booting in QEMU (10 second timeout)..."
	timeout 10 qemu-system-x86_64 -bios /usr/share/ovmf/OVMF.fd \
	                   -hda hello_disk.img \
	                   -m 512M \
	                   -nographic || true
