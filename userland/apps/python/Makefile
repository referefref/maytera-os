# Makefile for Python interpreter

CC = gcc
LD = ld
CFLAGS = -ffreestanding -nostdlib -nostdinc -fno-builtin -fstack-protector-strong -mstack-protector-guard=global \
         -fPIE -mno-red-zone -mno-mmx -mno-sse -mno-sse2 -O2 -Wall \
         -I../../libc -isystem /usr/lib/gcc/x86_64-linux-gnu/12/include

LDFLAGS = -pie --no-dynamic-linker -z notext -nostdlib -T ../../user-pie.ld

TARGET = python
OBJECTS = main.o

all: $(TARGET)

$(TARGET): $(OBJECTS)
	$(LD) -z max-page-size=0x1000 $(LDFLAGS) -o $@ ../../libc/crt0.o $(OBJECTS) ../../libc/libc.a

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJECTS) $(TARGET)

.PHONY: all clean
