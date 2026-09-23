# Makefile for sdl12test - proof-of-life app for the MayteraOS SDL 1.2
# backend (task #745, userland/libSDL). Same link profile as glcube (#319):
# a PIE at the user.ld fixed base, linking libSDL.a and libgl.a which are
# themselves compiled -fno-pic/-mcmodel=large (see userland/libSDL/Makefile
# for why that combination is safe on this platform).
CC = gcc
LD = ld
LIBC_DIR = ../../libc
LIBGL_DIR = ../../libgl
LIBSDL_DIR = ../../libSDL

CFLAGS = -fPIE -ffreestanding -fstack-protector-strong -mstack-protector-guard=global -mno-red-zone -msse -msse2 \
         -nostdlib -nostdinc -fno-builtin -O2 -g \
         -I. -I$(LIBC_DIR) -I$(LIBGL_DIR)/include -I$(LIBSDL_DIR)/include -I$(LIBSDL_DIR)/include/SDL \
         -isystem /usr/lib/gcc/x86_64-linux-gnu/12/include

LDFLAGS = -pie --no-dynamic-linker -z notext -nostdlib -z max-page-size=0x1000 -T ../../user-pie.ld

TARGET = SDL12TEST
SRCS = main.c
OBJS = $(SRCS:.c=.o)
CRT0 = $(LIBC_DIR)/crt0.o
LIBC = $(LIBC_DIR)/libc.a
LIBGL = $(LIBGL_DIR)/libgl.a
LIBSDL = $(LIBSDL_DIR)/libSDL.a

all: $(TARGET)

$(TARGET): $(OBJS) $(CRT0) $(LIBSDL) $(LIBGL) $(LIBC)
	$(LD) $(LDFLAGS) -o $@ $(CRT0) $(OBJS) $(LIBSDL) $(LIBGL) $(LIBC)
	@echo Built $(TARGET)
	@ls -l $(TARGET)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(TARGET)

.PHONY: all clean
