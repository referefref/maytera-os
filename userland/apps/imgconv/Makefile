# Makefile for imgconv - Image Converter (batch convert / resize)

CC = gcc
LD = ld

LIBC_DIR = ../../libc

CFLAGS = -ffreestanding -fstack-protector-strong -mstack-protector-guard=global -fPIE -mno-red-zone \
         -nostdlib -nostdinc -fno-builtin -Wall -Wextra -O2 -g \
         -I$(LIBC_DIR) -isystem /usr/lib/gcc/x86_64-linux-gnu/12/include

LDFLAGS = -pie --no-dynamic-linker -z notext -nostdlib -z max-page-size=0x1000 -T ../../user-pie.ld

# Uppercase: the golden overlay creates /APPS/<binary name> for a declared
# new app (build/golden-new-apps.list), ext2 is case-sensitive, and the
# Start-menu entry launches /APPS/IMGCONV.
TARGET = IMGCONV
# jpegenc.o is apps/paint/jpegenc.c compiled BY PATH: a pure function over an
# ARGB buffer with no Studio state, so both apps share one JPEG encoder and
# cannot drift apart on output. It is not copied here and paint is not edited.
OBJS = main.o imgenc.o jpegenc.o

CRT0 = $(LIBC_DIR)/crt0.o
LIBC = $(LIBC_DIR)/libc.a

all: $(TARGET)

$(TARGET): $(OBJS) $(CRT0) $(LIBC)
	$(LD) $(LDFLAGS) -o $@ $(CRT0) $(OBJS) $(LIBC)
	@echo "Built $(TARGET) (Image Converter)"
	@ls -l $(TARGET)

main.o: main.c imgenc.h
	$(CC) $(CFLAGS) -c $< -o $@

imgenc.o: imgenc.c imgenc.h
	$(CC) $(CFLAGS) -c $< -o $@

jpegenc.o: ../paint/jpegenc.c ../paint/studio.h
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(TARGET)

.PHONY: all clean
