# Makefile for tail - MayteraOS user-space application

CC = gcc
LD = ld

# Paths
LIBC_DIR = ../../libc
USERLAND_DIR = ../..

# Compiler flags
CFLAGS = -ffreestanding -fno-stack-protector -fno-pic -mno-red-zone \
         -nostdlib -nostdinc -fno-builtin -mcmodel=large -Wall -Wextra -O2 -g \
         -I$(LIBC_DIR) -isystem /usr/lib/gcc/x86_64-linux-gnu/12/include

# Linker flags
LDFLAGS = -nostdlib -z max-page-size=0x1000 -T $(USERLAND_DIR)/user.ld

# Output
TARGET = tail

# Objects
OBJS = main.o

# Libraries
CRT0 = $(LIBC_DIR)/crt0.o
LIBC = $(LIBC_DIR)/libc.a

# Default target
all: $(TARGET)

# Link the executable
$(TARGET): $(OBJS) $(CRT0) $(LIBC)
	$(LD) $(LDFLAGS) -o $@ $(CRT0) $(OBJS) $(LIBC)
	@echo "Built $(TARGET)"

# Compile C source
%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

# Clean
clean:
	rm -f $(OBJS) $(TARGET)

.PHONY: all clean
