# libarchivetest - running proof for the mports libarchive port (#745).
# Gets its flags from userland/ports/ports.mk, like zlibtest/pcre2check.
.DEFAULT_GOAL := all

CC = gcc
LD = ld

USERLAND_DIR = ../..
LIBC_DIR = ../../libc
PORTS_DIR = ../../ports
BUILD_DIR = build

include $(PORTS_DIR)/ports.mk

TARGET = LIBARCTEST
OBJECTS = $(BUILD_DIR)/main.o
CRT0 = $(LIBC_DIR)/crt0.o
LIBC = $(LIBC_DIR)/libc.a
LIBARCHIVE = $(MPORTS_LIBDIR)/libarchive.a
LIBZ = $(MPORTS_LIBDIR)/libz.a

all: $(TARGET)

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

# The ports are built by userland/ports/mports.sh (build-golden.sh runs it
# before any app). This rule is LOUD and fails if mports fails; it does not
# silently substitute anything.
$(LIBARCHIVE):
	@echo "libarchivetest: libarchive.a not built yet; running the ports driver"
	bash $(PORTS_DIR)/mports.sh build libarchive

$(BUILD_DIR)/main.o: main.c embed_arr.h | $(BUILD_DIR) $(LIBARCHIVE)
	$(CC) $(MPORTS_CFLAGS) $(MPORTS_INCLUDE) -MMD -MP -c $< -o $@

# Link order: our objects, then libarchive.a, then libz.a (libarchive calls
# zlib), then libc.a last (both call memcpy/malloc out of libc).
$(TARGET): $(OBJECTS) $(CRT0) $(LIBC) $(LIBARCHIVE) $(LIBZ)
	$(LD) $(MPORTS_LDFLAGS) -o $@ $(CRT0) $(OBJECTS) $(LIBARCHIVE) $(LIBZ) $(LIBC)
	@echo "Built $(TARGET)"

clean:
	rm -rf $(BUILD_DIR) $(TARGET)

-include $(BUILD_DIR)/main.d

.PHONY: all clean
