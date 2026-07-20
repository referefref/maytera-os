# Makefile for clear

CC = gcc
LD = ld

LIBC_DIR = ../../libc
CFLAGS = -m64 -ffreestanding -fno-stack-protector -nostdlib -nostdinc \
         -mno-red-zone -mcmodel=large -fno-pic \
         -Wall -Wextra -O2 \
         -I$(LIBC_DIR) -isystem /usr/lib/gcc/x86_64-linux-gnu/12/include

LDFLAGS = -nostdlib -static -m elf_x86_64 -T ../../user.ld -z max-page-size=0x1000

CRT0 = $(LIBC_DIR)/crt0.o
LIBC = $(LIBC_DIR)/libc.a

TARGET = clear.elf
SRCS = main.c
OBJS = $(SRCS:.c=.o)

all: $(TARGET)

$(TARGET): $(OBJS) $(CRT0) $(LIBC)
	$(LD) $(LDFLAGS) -o $@ $(CRT0) $(OBJS) $(LIBC)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f *.o $(TARGET)

.PHONY: all clean
