# Makefile for Hello GUI app

CC = gcc
LD = ld

# Paths
LIBC_DIR = ../../libc
BUILD_DIR = build

# Compiler flags for user-space
CFLAGS = -ffreestanding -fno-stack-protector -fno-pic -mno-red-zone \
         -nostdlib -nostdinc -fno-builtin -Wall -Wextra -O2 -g \
         -I$(LIBC_DIR) -isystem /usr/lib/gcc/x86_64-linux-gnu/12/include

# Linker flags
LDFLAGS = -nostdlib -z max-page-size=0x1000

# Output
TARGET = setmouse

# Source files
SOURCES = main.c
OBJECTS = $(BUILD_DIR)/main.o

# Libc
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
