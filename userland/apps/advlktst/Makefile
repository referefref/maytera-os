# Makefile for advlktst - #404 Stage 6 advisory-lock end-to-end test.
#
# Built exactly like a normal app (same flags as lockprobe/calc). It is a dev
# probe harness: declared in build/golden-new-apps.list (so the build MAY create
# /APPS/ADVLKTST) and in build/unshipped-apps.list (so a NORMAL golden does NOT
# ship it; build --with-unshipped-apps to include it for verification). Launched
# on a test image by a /ADVLKTST.RUN marker on the ESP (kernel gui/desktop.c),
# the same mechanism as /PTTEST.RUN.

CC = gcc
LD = ld

LIBC_DIR = ../../libc
USERLAND_DIR = ../..

CFLAGS = -ffreestanding -fstack-protector-strong -mstack-protector-guard=global -fPIE -mno-red-zone \
         -nostdlib -nostdinc -fno-builtin -Wall -Wextra -O2 -g \
         -I$(LIBC_DIR) -isystem /usr/lib/gcc/x86_64-linux-gnu/12/include

LDFLAGS = -pie --no-dynamic-linker -z notext -nostdlib -z max-page-size=0x1000 -T $(USERLAND_DIR)/user-pie.ld

TARGET = ADVLKTST
OBJS = main.o
CRT0 = $(LIBC_DIR)/crt0.o
LIBC = $(LIBC_DIR)/libc.a

all: $(TARGET)

$(TARGET): $(OBJS) $(CRT0) $(LIBC)
	$(LD) $(LDFLAGS) -o $@ $(CRT0) $(OBJS) $(LIBC)
	@echo "Built $(TARGET)"

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(TARGET)

.PHONY: all clean
