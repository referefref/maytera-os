CC = gcc
CFLAGS = -m64 -ffreestanding -fstack-protector-strong -mstack-protector-guard=global -nostdlib -nostdinc -mno-red-zone -mcmodel=large -fno-pic
LDFLAGS = -nostdlib -static -m elf_x86_64 -T ../user.ld

all: hello.elf

hello.elf: hello.o
	ld $(LDFLAGS) -o hello.elf hello.o

hello.o: hello.c
	$(CC) $(CFLAGS) -c hello.c -o hello.o

clean:
	rm -f *.o *.elf

.PHONY: all clean
