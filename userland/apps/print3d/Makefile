# Makefile for print3d - MayteraOS "3D Print" app (#396, M3D Micro)

CC = gcc
LD = ld

LIBC_DIR = ../../libc
LIBGL_DIR = ../../libgl

CFLAGS = -ffreestanding -mcmodel=large -fno-stack-protector -fno-pic -mno-red-zone \
         -msse -msse2 \
         -nostdlib -nostdinc -fno-builtin -Wall -Wextra -O2 -g \
         -I$(LIBC_DIR) -I$(LIBGL_DIR)/include \
         -isystem /usr/lib/gcc/x86_64-linux-gnu/12/include

LDFLAGS = -nostdlib -z max-page-size=0x1000 -T ../../user.ld

TARGET = print3d
OBJS = main.o m3d_gcode.o m3d_preprocess.o m3d_transport.o stlview.o

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
