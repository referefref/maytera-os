# Makefile for settings - MayteraOS user-space application
# (#267) now links libhelp.a for the help-subsystem reference integration.

CC = gcc
LD = ld

# Paths
LIBC_DIR = ../../libc
LIBHELP_DIR = ../../libhelp
USERLAND_DIR = ../..

# Compiler flags
CFLAGS = -ffreestanding -fno-stack-protector -fno-pic -mno-red-zone -mcmodel=large \
         -nostdlib -nostdinc -fno-builtin -Wall -Wextra -O2 -g \
         -I$(LIBC_DIR) -I$(LIBHELP_DIR) -isystem /usr/lib/gcc/x86_64-linux-gnu/12/include

# Linker flags
LDFLAGS = -nostdlib -z max-page-size=0x1000 -T $(USERLAND_DIR)/user.ld

# Output
TARGET = settings

# Objects
OBJS = main.o

# Libraries
CRT0 = $(LIBC_DIR)/crt0.o
LIBC = $(LIBC_DIR)/libc.a
LIBHELP = $(LIBHELP_DIR)/libhelp.a

# Default target
all: $(TARGET)

# Link the executable (libhelp before libc so its libc references resolve).
$(TARGET): $(OBJS) $(CRT0) $(LIBHELP) $(LIBC)
	$(LD) $(LDFLAGS) -o $@ $(CRT0) $(OBJS) $(LIBHELP) $(LIBC)
	@echo "Built $(TARGET)"
	@ls -l $(TARGET)

# Build libhelp first if needed.
$(LIBHELP):
	$(MAKE) -C $(LIBHELP_DIR)

# Compile C source
%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

# Clean
clean:
	rm -f $(OBJS) $(TARGET)

.PHONY: all clean
