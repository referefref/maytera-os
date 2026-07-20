# Makefile for libGL.a - TinyGL (C-Chads fork) ported to MayteraOS freestanding userland (#319)
CC = gcc
AR = ar
LIBC_DIR = ../libc

CFLAGS = -ffreestanding -fno-stack-protector -fno-pic -mno-red-zone -msse -msse2 \
         -nostdlib -nostdinc -fno-builtin -O2 -g -mcmodel=large -fno-pie \
         -DNO_DEBUG_OUTPUT -DNDEBUG \
         -I. -Iinclude -I$(LIBC_DIR) -isystem /usr/lib/gcc/x86_64-linux-gnu/12/include

SRCS = $(wildcard src/*.c)
OBJS = $(SRCS:.c=.o)

all: libgl.a

libgl.a: $(OBJS)
	$(AR) rcs $@ $(OBJS)
	@echo "Built libgl.a"
	@ls -l libgl.a

src/%.o: src/%.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f src/*.o libgl.a

.PHONY: all clean
