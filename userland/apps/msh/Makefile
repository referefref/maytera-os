# Makefile for msh - MayteraOS Shell
# #745: msh now links the mports libedit for real line editing (el_gets/history),
# which pulls the ncurses termcap and the musl-regex POSIX regex libedit uses.
# All three are built into userland/ports/out before the golden app loop runs.

CC = gcc
LD = ld

# Paths
LIBC_DIR = ../../libc
USERLAND_DIR = ../..
PORTS_OUT = ../../ports/out

# Compiler flags
CFLAGS = -ffreestanding -fstack-protector-strong -mstack-protector-guard=global -fPIE -mno-red-zone \
         -nostdlib -nostdinc -fno-builtin -Wall -Wextra -O2 -g \
         -I$(LIBC_DIR) -isystem /usr/lib/gcc/x86_64-linux-gnu/12/include \
         -I$(PORTS_OUT)/include

# Linker flags
LDFLAGS = -pie --no-dynamic-linker -z notext -nostdlib -z max-page-size=0x1000 -T $(USERLAND_DIR)/user-pie.ld

# Output
TARGET = msh

# Objects
OBJS = main.o

# Libraries
CRT0 = $(LIBC_DIR)/crt0.o
LIBC = $(LIBC_DIR)/libc.a
LIBEDIT = $(PORTS_OUT)/lib/libedit.a
LIBNCURSES = $(PORTS_OUT)/lib/libncurses.a
LIBREGEX = $(PORTS_OUT)/lib/libregex.a

# Default target
all: $(TARGET)

# Link the executable. Port archives come after our objects and before libc.a,
# because they call malloc/str*/mem* out of libc.
$(TARGET): $(OBJS) $(CRT0) $(LIBC) $(LIBEDIT) $(LIBNCURSES) $(LIBREGEX)
	$(LD) $(LDFLAGS) -o $@ $(CRT0) $(OBJS) $(LIBEDIT) $(LIBNCURSES) $(LIBREGEX) $(LIBC)
	@echo "Built $(TARGET)"
	@ls -l $(TARGET)

# Compile C source
%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

# Clean
clean:
	rm -f $(OBJS) $(TARGET)

.PHONY: all clean
