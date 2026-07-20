# Makefile for IPC test application

CC = gcc
CFLAGS = -m64 -ffreestanding -fPIC -fno-stack-protector -nostdlib -nostdinc -mno-red-zone -I../../libc -mcmodel=large
LDFLAGS = -nostdlib -static -m elf_x86_64 -T ../../user.ld

LIBC_DIR = ../../libc
LIBC = $(LIBC_DIR)/libc.a

all: ipc_test

ipc_test: main.o $(LIBC)
	ld $(LDFLAGS) -o ipc_test main.o $(LIBC)

main.o: main.c
	$(CC) $(CFLAGS) -c main.c -o main.o

$(LIBC):
	$(MAKE) -C $(LIBC_DIR)

clean:
	rm -f *.o ipc_test

.PHONY: all clean
