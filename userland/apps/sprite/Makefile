# Makefile for sprite - Sprite Studio pixel-art and sprite-animation editor

CC = gcc
LD = ld

LIBC_DIR = ../../libc

CFLAGS = -ffreestanding -fstack-protector-strong -mstack-protector-guard=global -fPIE -mno-red-zone \
         -nostdlib -nostdinc -fno-builtin -Wall -Wextra -O2 -g \
         -I$(LIBC_DIR) -isystem /usr/lib/gcc/x86_64-linux-gnu/12/include

LDFLAGS = -pie --no-dynamic-linker -z notext -nostdlib -z max-page-size=0x1000 -T ../../user-pie.ld

# Uppercase: the golden overlay creates /APPS/<binary name> for a declared
# new app (build/golden-new-apps.list), ext2 is case-sensitive, and the
# Start-menu entry launches /APPS/SPRITE.
TARGET = SPRITE
# imgenc.c (BMP24 + PNG writers) is shared with apps/imgconv and compiled
# BY PATH from there: one implementation, no second copy. See imgenc.h.
OBJS = main.o imgenc.o

CRT0 = $(LIBC_DIR)/crt0.o
LIBC = $(LIBC_DIR)/libc.a

all: $(TARGET)

$(TARGET): $(OBJS) $(CRT0) $(LIBC)
	$(LD) $(LDFLAGS) -o $@ $(CRT0) $(OBJS) $(LIBC)
	@echo "Built $(TARGET) (Sprite Studio)"
	@ls -l $(TARGET)

main.o: main.c ../imgconv/imgenc.h
	$(CC) $(CFLAGS) -c $< -o $@

imgenc.o: ../imgconv/imgenc.c ../imgconv/imgenc.h
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(TARGET)

.PHONY: all clean
