# Makefile for sleep_cmd - MayteraOS user-space application

CC = gcc
LD = ld

# Paths
LIBC_DIR = ../../libc
USERLAND_DIR = ../..

# Compiler flags
CFLAGS = -ffreestanding -fstack-protector-strong -mstack-protector-guard=global -fPIE -mno-red-zone \
         -nostdlib -nostdinc -fno-builtin -Wall -Wextra -O2 -g \
         -I$(LIBC_DIR) -isystem /usr/lib/gcc/x86_64-linux-gnu/12/include

# Linker flags
LDFLAGS = -pie --no-dynamic-linker -z notext -nostdlib -z max-page-size=0x1000 -T $(USERLAND_DIR)/user-pie.ld

# Output
# THE INSTALL NAME, AND WHY BOTH THE BINARY AND THE DIRECTORY HAD TO MOVE.
#
# msh resolves a typed command as exact -> UPPERCASE -> name.ELF ->
# NAME.ELF. While this built `sleep_cmd` it shipped as /APPS/SLEEP_CMD,
# which none of those four forms reaches, so `sleep` was not a command on
# this OS at all.
#
# build/build-golden.sh resolves an app's /APPS entry as: exact match to
# the BINARY name among the entries already on the image, then
# case-insensitive to the binary name, then case-insensitive to the SOURCE
# DIRECTORY name, else the binary name verbatim. So renaming only the
# binary was not enough: with the directory still called sleep_cmd, step 3
# matched the existing /APPS/SLEEP_CMD and the fresh binary would have
# landed there - and RETIRED_APP_NAMES, later in the same script, would
# then have deleted it. A green build with no sleep on the image.
#
# Hence: directory sleepcmd (so step 3 cannot match), binary SLEEP (so the
# install name is /APPS/SLEEP, which is what the on-VM verification ran),
# and SLEEP_CMD retired in build-golden.sh, because a source change alone
# deletes NOTHING from an image built by overlaying onto a static asset
# base (local 98).
TARGET = SLEEP

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
