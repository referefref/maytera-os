# Makefile for MayteraOS Userland Compositor

CC = gcc
LD = ld

# Paths
LIBC_DIR = ../libc

# Flags for freestanding userland (matches other app Makefiles)
CFLAGS = -m64 -ffreestanding -fno-builtin -nostdinc -nostdlib \
         -fno-stack-protector -mno-red-zone -Wall -Wextra \
         -I$(LIBC_DIR) -I. -O2 \
         -isystem /usr/lib/gcc/x86_64-linux-gnu/12/include

LDFLAGS = -T ../user.ld -nostdlib -z max-page-size=0x1000

# Source files
COMPOSITOR_SRCS = compositor.c window_manager.c desktop.c taskbar.c client.c
COMPOSITOR_OBJS = $(COMPOSITOR_SRCS:.c=.o)

CRT0 = $(LIBC_DIR)/crt0.o
LIBC = $(LIBC_DIR)/libc.a

# Targets
all: compositor

compositor: $(COMPOSITOR_OBJS) $(CRT0) $(LIBC)
	$(LD) $(LDFLAGS) -o $@ $(CRT0) $(COMPOSITOR_OBJS) $(LIBC)
	@echo "Built compositor"
	@ls -l compositor

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f *.o compositor

# Dependencies
compositor.o: compositor.c compositor.h protocol.h window_manager.h desktop.h taskbar.h
window_manager.o: window_manager.c window_manager.h compositor.h
desktop.o: desktop.c desktop.h compositor.h
taskbar.o: taskbar.c taskbar.h compositor.h
client.o: client.c client.h protocol.h

.PHONY: all clean
