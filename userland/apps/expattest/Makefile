# expattest - running proof for the mports Expat port. Flags come from
# userland/ports/ports.mk (never pasted); see zlibtest/Makefile for why.
.DEFAULT_GOAL := all

CC = gcc
LD = ld
USERLAND_DIR = ../..
LIBC_DIR = ../../libc
PORTS_DIR = ../../ports
BUILD_DIR = build

include $(PORTS_DIR)/ports.mk

TARGET = EXPATTEST
OBJECTS = $(BUILD_DIR)/main.o
CRT0 = $(LIBC_DIR)/crt0.o
LIBC = $(LIBC_DIR)/libc.a
LIBEXPAT = $(MPORTS_LIBDIR)/libexpat.a

all: $(TARGET)

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(LIBEXPAT):
	@echo "expattest: libexpat.a not built yet; running the ports driver"
	bash $(PORTS_DIR)/mports.sh build expat

$(BUILD_DIR)/main.o: main.c | $(BUILD_DIR) $(LIBEXPAT)
	$(CC) $(MPORTS_CFLAGS) $(MPORTS_INCLUDE) -MMD -MP -c $< -o $@

$(TARGET): $(OBJECTS) $(CRT0) $(LIBC) $(LIBEXPAT)
	$(LD) $(MPORTS_LDFLAGS) -o $@ $(CRT0) $(OBJECTS) $(LIBEXPAT) $(LIBC)
	@echo "Built $(TARGET)"

clean:
	rm -rf $(BUILD_DIR) $(TARGET)

-include $(BUILD_DIR)/main.d
.PHONY: all clean
