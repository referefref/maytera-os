# Makefile for musicplayer - MayteraOS retro hi-fi skinned player (#322)
CC = gcc
LD = ld

LIBC_DIR = ../../libc
LIBGL_DIR = ../../libgl
USERLAND_DIR = ../..

CFLAGS = -ffreestanding -fno-stack-protector -fno-pic -mno-red-zone -mcmodel=large \
         -msse -msse2 -nostdlib -nostdinc -fno-builtin -Wall -Wextra -O2 -g \
         -I$(LIBC_DIR) -I$(LIBGL_DIR)/include -isystem /usr/lib/gcc/x86_64-linux-gnu/12/include

LDFLAGS = -nostdlib -z max-page-size=0x1000 -T $(USERLAND_DIR)/user.ld

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

clean:
	rm -f $(OBJS) $(TARGET)

.PHONY: all clean
