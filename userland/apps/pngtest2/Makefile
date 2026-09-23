# pngtest2 - running proof for the mports libpng port. Flags come from
# userland/ports/ports.mk (never pasted); see zlibtest/Makefile for why.
#
# libpng needs zlib: the link pulls libpng.a THEN libz.a THEN libc.a, because
# libpng calls into zlib and zlib calls into libc.
.DEFAULT_GOAL := all

CC = gcc
LD = ld
USERLAND_DIR = ../..
LIBC_DIR = ../../libc
PORTS_DIR = ../../ports
BUILD_DIR = build

include $(PORTS_DIR)/ports.mk

TARGET = PNGTEST2
OBJECTS = $(BUILD_DIR)/main.o
CRT0 = $(LIBC_DIR)/crt0.o
LIBC = $(LIBC_DIR)/libc.a
LIBPNG = $(MPORTS_LIBDIR)/libpng.a
LIBZ = $(MPORTS_LIBDIR)/libz.a

all: $(TARGET)

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(LIBPNG):
	@echo "pngtest2: libpng.a not built yet; running the ports driver (pulls zlib via needs=)"
	bash $(PORTS_DIR)/mports.sh build libpng

$(BUILD_DIR)/main.o: main.c | $(BUILD_DIR) $(LIBPNG)
	$(CC) $(MPORTS_CFLAGS) $(MPORTS_INCLUDE) -MMD -MP -c $< -o $@

$(TARGET): $(OBJECTS) $(CRT0) $(LIBC) $(LIBPNG)
	$(LD) $(MPORTS_LDFLAGS) -o $@ $(CRT0) $(OBJECTS) $(LIBPNG) $(LIBZ) $(LIBC)
	@echo "Built $(TARGET)"

clean:
	rm -rf $(BUILD_DIR) $(TARGET)

-include $(BUILD_DIR)/main.d
.PHONY: all clean
