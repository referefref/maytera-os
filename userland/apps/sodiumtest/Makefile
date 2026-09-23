# sodiumtest - running proof for the mports libsodium port. Flags come from
# userland/ports/ports.mk (never pasted); see md4ctest/Makefile for the model.
.DEFAULT_GOAL := all
CC = gcc
LD = ld
USERLAND_DIR = ../..
LIBC_DIR = ../../libc
PORTS_DIR = ../../ports
BUILD_DIR = build

include $(PORTS_DIR)/ports.mk

TARGET = SODIUMTEST
OBJECTS = $(BUILD_DIR)/main.o
CRT0 = $(LIBC_DIR)/crt0.o
LIBC = $(LIBC_DIR)/libc.a
LIBSODIUM = $(MPORTS_LIBDIR)/libsodium.a

all: $(TARGET)

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(LIBSODIUM):
	@echo "sodiumtest: libsodium.a not built yet; running the ports driver"
	bash $(PORTS_DIR)/mports.sh build libsodium

$(BUILD_DIR)/main.o: main.c | $(BUILD_DIR) $(LIBSODIUM)
	$(CC) $(MPORTS_CFLAGS) $(MPORTS_INCLUDE) -MMD -MP -c $< -o $@

$(TARGET): $(OBJECTS) $(CRT0) $(LIBC) $(LIBSODIUM)
	$(LD) $(MPORTS_LDFLAGS) -o $@ $(CRT0) $(OBJECTS) $(LIBSODIUM) $(LIBC)
	@echo "Built $(TARGET)"

clean:
	rm -rf $(BUILD_DIR) $(TARGET)
