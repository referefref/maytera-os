# Makefile for sertest - Stage 2 serial.port demonstration harness (unshipped dev app).
CC = gcc
GCC_INC := $(shell $(CC) -print-file-name=include)
CFLAGS = -fPIE -m64 -ffreestanding -fPIC -fstack-protector-strong -mstack-protector-guard=global -nostdlib -nostdinc -mno-red-zone -I../../libc -isystem $(GCC_INC)
LDFLAGS = -pie --no-dynamic-linker -z notext -nostdlib -m elf_x86_64 -T ../../user-pie.ld

LIBC_DIR = ../../libc
LIBC = $(LIBC_DIR)/libc.a

all: sertest

sertest: main.o $(LIBC)
	ld -z max-page-size=0x1000 $(LDFLAGS) -o sertest ../../libc/crt0.o main.o $(LIBC)

main.o: main.c
	$(CC) $(CFLAGS) -c main.c -o main.o

$(LIBC):
	$(MAKE) -C $(LIBC_DIR)

clean:
	rm -f *.o sertest

.PHONY: all clean
