# Makefile for updated - MayteraOS Auto-Update daemon (#402)
CC = gcc
LD = ld

LIBC_DIR = ../../libc
BUILD_DIR = build

CFLAGS = -ffreestanding -fstack-protector-strong -mstack-protector-guard=global -fPIE -mno-red-zone \
         -nostdlib -nostdinc -fno-builtin -Wall -Wextra -O2 -g \
         -I$(LIBC_DIR) -I../../libarchive -isystem /usr/lib/gcc/x86_64-linux-gnu/12/include

LDFLAGS = -pie --no-dynamic-linker -z notext -nostdlib -z max-page-size=0x1000 -T ../../user-pie.ld

TARGET = updated
OBJECTS = $(BUILD_DIR)/main.o $(BUILD_DIR)/arc.o

CRT0 = $(LIBC_DIR)/crt0.o
LIBC = $(LIBC_DIR)/libc.a

all: $(BUILD_DIR) $(TARGET)

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(TARGET): $(OBJECTS) $(CRT0) $(LIBC)
	$(LD) $(LDFLAGS) -o $@ $(CRT0) $(OBJECTS) $(LIBC)
	@echo "Built $(TARGET)"
	@ls -l $(TARGET)

$(BUILD_DIR)/main.o: main.c ../../libarchive/arc.h ../../libarchive/arc_port.h
	$(CC) $(CFLAGS) -c $< -o $@

# #613: ONE canonical archiver source (userland/libarchive). This app used to
# carry a byte-identical private copy of arc.c/arc.h/arc_port.h, which is the
# forked-primitive pattern the reuse rule forbids: a fix (or a hardening) in
# one copy reached none of the others.
$(BUILD_DIR)/arc.o: ../../libarchive/arc.c ../../libarchive/arc.h ../../libarchive/arc_port.h
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -rf $(BUILD_DIR) $(TARGET)

.PHONY: all clean
