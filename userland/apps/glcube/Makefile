# Makefile for glcube (TinyGL demo) - MayteraOS (#319)
CC = gcc
LD = ld
LIBC_DIR = ../../libc
LIBGL_DIR = ../../libgl

CFLAGS = -fPIE -ffreestanding -fstack-protector-strong -mstack-protector-guard=global -mno-red-zone -msse -msse2          -nostdlib -nostdinc -fno-builtin -O2 -g          -I. -I$(LIBC_DIR) -I$(LIBGL_DIR)/include          -isystem /usr/lib/gcc/x86_64-linux-gnu/12/include

LDFLAGS = -pie --no-dynamic-linker -z notext -nostdlib -z max-page-size=0x1000 -T ../../user-pie.ld

TARGET = GLCUBE
SRCS = main.c
OBJS = $(SRCS:.c=.o)
CRT0 = $(LIBC_DIR)/crt0.o
LIBC = $(LIBC_DIR)/libc.a
LIBGL = $(LIBGL_DIR)/libgl.a

all: $(TARGET)

$(TARGET): $(OBJS) $(CRT0) $(LIBC) $(LIBGL)
	$(LD) $(LDFLAGS) -o $@ $(CRT0) $(OBJS) $(LIBGL) $(LIBC)
	@echo Built $(TARGET)
	@ls -l $(TARGET)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(TARGET)

.PHONY: all clean
