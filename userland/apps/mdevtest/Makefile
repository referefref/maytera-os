# mdevtest - headless dev-bridge self-test binary.
# Mirrors the freestanding userland app build used by atastress/hello.
CC = gcc
LD = ld
LIBC_DIR = ../../libc
BUILD_DIR = build
TAG ?= dev
CFLAGS = -ffreestanding -fno-stack-protector -fno-pic -mno-red-zone -mcmodel=large \
         -nostdlib -nostdinc -fno-builtin -Wall -Wextra -O2 -g \
         -DMDEVTEST_TAG=\"$(TAG)\" \
         -I$(LIBC_DIR) -isystem /usr/lib/gcc/x86_64-linux-gnu/12/include
LDFLAGS = -nostdlib -z max-page-size=0x1000 -T ../../user.ld
CRT0 = $(LIBC_DIR)/crt0.o
LIBC = $(LIBC_DIR)/libc.a

all: $(BUILD_DIR) mdevtest

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

mdevtest: $(BUILD_DIR)/main.o $(CRT0) $(LIBC)
	$(LD) $(LDFLAGS) -o $@ $(CRT0) $(BUILD_DIR)/main.o $(LIBC)

$(BUILD_DIR)/main.o: main.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -rf $(BUILD_DIR) mdevtest
.PHONY: all clean
