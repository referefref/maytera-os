# Makefile for clock - MayteraOS Clock Display

CC = gcc
LD = ld

LIBC_DIR = ../../libc

CFLAGS = -ffreestanding -mcmodel=large -fno-stack-protector -fno-pic -mno-red-zone          -nostdlib -nostdinc -fno-builtin -Wall -Wextra -O2 -g          -I$(LIBC_DIR) -isystem /usr/lib/gcc/x86_64-linux-gnu/12/include

LDFLAGS = -nostdlib -z max-page-size=0x1000 -T ../../user.ld

TARGET = clock
OBJS = main.o

CRT0 = $(LIBC_DIR)/crt0.o
LIBC = $(LIBC_DIR)/libc.a

all: $(TARGET)

$(TARGET): $(OBJS) $(CRT0) $(LIBC)
	$(LD) $(LDFLAGS) -o $@ $(CRT0) $(OBJS) $(LIBC)
	@echo "Built $(TARGET)"
	@ls -l $(TARGET)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(TARGET)

.PHONY: all clean
