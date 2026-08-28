# Makefile for winchprb - #227 headless reproduction harness for the
# vi-resize crash + terminal/pty wedge. Debug-only probe, never shipped to a
# production golden; declared in build/golden-new-apps.list like the other
# probe apps (pttest, zombprobe, hdadbg, ...).

CC = gcc
LD = ld

USERLAND_DIR = ../..
LIBC_DIR = ../../libc
BUILD_DIR = build

CFLAGS = -ffreestanding -fstack-protector-strong -mstack-protector-guard=global -fPIE -mno-red-zone \
         -nostdlib -nostdinc -fno-builtin -Wall -Wextra -O2 -g \
         -I$(LIBC_DIR) -isystem /usr/lib/gcc/x86_64-linux-gnu/12/include

LDFLAGS = -nostdlib -z max-page-size=0x1000 -pie --no-dynamic-linker -z notext -T $(USERLAND_DIR)/user-pie.ld

TARGET = WINCHPRB

OBJECTS = $(BUILD_DIR)/main.o
CRT0 = $(LIBC_DIR)/crt0.o
LIBC = $(LIBC_DIR)/libc.a

all: $(BUILD_DIR) $(TARGET)

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(TARGET): $(OBJECTS) $(CRT0) $(LIBC)
	$(LD) $(LDFLAGS) -o $@ $(CRT0) $(OBJECTS) $(LIBC)
	@echo "Built $(TARGET)"

$(BUILD_DIR)/main.o: main.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -rf $(BUILD_DIR) $(TARGET)

.PHONY: all clean
