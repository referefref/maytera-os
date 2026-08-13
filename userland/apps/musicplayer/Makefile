# Makefile for musicplayer - MayteraOS retro hi-fi skinned player (#322)
CC = gcc
LD = ld

LIBC_DIR = ../../libc
LIBGL_DIR = ../../libgl
USERLAND_DIR = ../..

CFLAGS = -ffreestanding -fstack-protector-strong -mstack-protector-guard=global -fPIE -mno-red-zone \
         -msse -msse2 -nostdlib -nostdinc -fno-builtin -Wall -Wextra -O2 -g \
         -I$(LIBC_DIR) -I../../libarchive -I$(LIBGL_DIR)/include -isystem /usr/lib/gcc/x86_64-linux-gnu/12/include

LDFLAGS = -pie --no-dynamic-linker -z notext -nostdlib -z max-page-size=0x1000 -T $(USERLAND_DIR)/user-pie.ld

TARGET = musicplr
OBJS = main.o arc.o viz.o
CRT0 = $(LIBC_DIR)/crt0.o
LIBC = $(LIBC_DIR)/libc.a
LIBGL = $(LIBGL_DIR)/libgl.a

all: $(TARGET)

$(TARGET): $(OBJS) $(CRT0) $(LIBC) $(LIBGL)
	$(LD) $(LDFLAGS) -o $@ $(CRT0) $(OBJS) $(LIBGL) $(LIBC)
	@echo "Built $(TARGET)"
	@ls -l $(TARGET)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

# #613: single canonical archiver source (see userland/libarchive).
arc.o: ../../libarchive/arc.c ../../libarchive/arc.h ../../libarchive/arc_port.h
	$(CC) $(CFLAGS) -c ../../libarchive/arc.c -o $@

clean:
	rm -f $(OBJS) $(TARGET)

.PHONY: all clean
