# MayteraOS UEFI Bootloader

A minimal gnu-efi based UEFI application that loads the MayteraOS kernel.

## What it does

`BOOTX64.EFI` is loaded by the UEFI firmware from `/EFI/BOOT/BOOTX64.EFI` on
an EFI System Partition. It then:

1. Opens the root of the volume it was loaded from.
2. Loads `\boot\kernel.elf`.
3. Parses the ELF64 header and copies each `PT_LOAD` segment to its
   virtual address (falling back to any free page if the requested address
   is already taken).
4. Locates the framebuffer via the UEFI Graphics Output Protocol.
5. Walks the UEFI configuration tables for the ACPI 2.0 or 1.0 RSDP.
6. Retrieves the UEFI memory map, calls `ExitBootServices`, and builds a
   `boot_info_t` matching `kernel/boot_info.h`:
   - magic `0x4D415954455241` ("MAYTERA")
   - memory map (UEFI types translated to MayteraOS types)
   - framebuffer info (address, resolution, pitch, pixel format)
   - ACPI RSDP address and version
   - kernel physical / virtual base and size
7. Jumps to the kernel entry point with `rdi = &boot_info` (System V AMD64
   calling convention, first argument in `rdi`).

## Build

You need `gcc`, `binutils`, and `gnu-efi` installed. On Debian/Ubuntu:

```bash
sudo apt-get install gcc binutils gnu-efi
```

Then:

```bash
make
```

Outputs:

- `BOOTX64.EFI` the full bootloader. Copy to `/EFI/BOOT/BOOTX64.EFI` on
  your FAT32 EFI System Partition.
- `hello.efi` a minimal UEFI hello-world useful for verifying your
  toolchain. Not required at runtime.

## Installing on a boot disk

After building, stage a FAT32 image like this (adjust paths to taste):

```bash
# Build the kernel first
make -C ../../kernel

# Create a 256 MiB FAT32 disk image
dd if=/dev/zero of=boot_disk.img bs=1M count=256
parted -s boot_disk.img mklabel gpt
parted -s boot_disk.img mkpart ESP fat32 1MiB 100%
parted -s boot_disk.img set 1 esp on

# Format and populate (requires loop mount; use sudo)
LOOP=$(sudo losetup -fP --show boot_disk.img)
sudo mkfs.fat -F32 ${LOOP}p1
mkdir -p mnt
sudo mount ${LOOP}p1 mnt
sudo mkdir -p mnt/EFI/BOOT mnt/boot
sudo cp BOOTX64.EFI       mnt/EFI/BOOT/
sudo cp ../../kernel/kernel.elf mnt/boot/kernel.elf
sudo sync
sudo umount mnt
sudo losetup -d ${LOOP}
rmdir mnt
```

Then boot in QEMU with OVMF:

```bash
qemu-system-x86_64 \
    -machine pc,accel=kvm \
    -cpu host -m 2G \
    -bios /usr/share/OVMF/OVMF_CODE.fd \
    -drive file=boot_disk.img,format=raw,if=ide \
    -serial stdio
```

## Notes

- The bootloader uses `AllocateAddress` to place segments at their
  requested virtual addresses. Higher-half kernels need the linker script
  to provide a physical LMA that is safe to allocate from UEFI memory.
  If `AllocateAddress` fails, it falls back to `AllocateAnyPages`, so a
  higher-half kernel linked with low LMAs will still boot.
- `ExitBootServices` is called after the final `GetMemoryMap`. Once that
  succeeds the kernel receives control in long mode with interrupts
  disabled; it is then responsible for installing its own GDT, IDT, page
  tables, and so on.
- This is the only supported boot path for MayteraOS. There is a
  vestigial multiboot2 header in `kernel/entry.asm` but no 32-to-64
  trampoline, so booting via GRUB `multiboot2` does not currently work.
