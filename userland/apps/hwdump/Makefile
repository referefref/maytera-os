# hwdump - hardware inventory dump tool.
#
# #661: this directory shipped main.c with NO Makefile, so nothing ever built
# it. The image therefore carried /APPS/HWDUMP.ELF, a 2026-07-10 ET_EXEC
# asset-base binary with a writable+executable segment, and no commit could
# replace it. Source with no Makefile is source that does not exist as far as
# the build is concerned.
#
# Mirrors the freestanding PIE userland app build used by mdevtest/atastress.
CC = gcc
LD = ld
LIBC_DIR = ../../libc
BUILD_DIR = build
CFLAGS = -ffreestanding -fstack-protector-strong -mstack-protector-guard=global -fPIE -mno-red-zone \
         -nostdlib -nostdinc -fno-builtin -Wall -Wextra -O2 -g \
         -I$(LIBC_DIR) -isystem /usr/lib/gcc/x86_64-linux-gnu/12/include
LDFLAGS = -pie --no-dynamic-linker -z notext -nostdlib -z max-page-size=0x1000 -T ../../user-pie.ld
CRT0 = $(LIBC_DIR)/crt0.o
LIBC = $(LIBC_DIR)/libc.a

all: $(BUILD_DIR) hwdump

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

hwdump: $(BUILD_DIR)/main.o $(CRT0) $(LIBC)
	$(LD) $(LDFLAGS) -o $@ $(CRT0) $(BUILD_DIR)/main.o $(LIBC)

$(BUILD_DIR)/main.o: main.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -rf $(BUILD_DIR) hwdump
.PHONY: all clean
