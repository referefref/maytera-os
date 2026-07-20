# Makefile for Python interpreter

CC = gcc
LD = ld
CFLAGS = -ffreestanding -nostdlib -nostdinc -fno-builtin -fno-stack-protector \
         -fno-pic -mno-red-zone -mno-mmx -mno-sse -mno-sse2 -O2 -Wall \
         -I../../libc -isystem /usr/lib/gcc/x86_64-linux-gnu/12/include

LDFLAGS = -nostdlib -T ../../user.ld

TARGET = python
OBJECTS = main.o

all: $(TARGET)

$(TARGET): $(OBJECTS)
	$(LD) $(LDFLAGS) -o $@ ../../libc/crt0.o $(OBJECTS) ../../libc/libc.a

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJECTS) $(TARGET)

.PHONY: all clean
