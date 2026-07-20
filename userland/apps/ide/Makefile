# Maytera Code Studio (IDE) - MayteraOS user-space application
CC = gcc
LD = ld
LIBC_DIR = ../../libc
CFLAGS = -ffreestanding -fno-stack-protector -fno-pic -fno-pie -mno-red-zone -mcmodel=large \
         -nostdlib -nostdinc -fno-builtin -Wall -Wextra -O2 -g \
         -I. -I$(LIBC_DIR) -isystem /usr/lib/gcc/x86_64-linux-gnu/12/include
LDFLAGS = -nostdlib -z max-page-size=0x1000 -T ../../user.ld
TARGET = IDE
SRCS = $(wildcard *.c)
ASMS = $(wildcard *.asm)
OBJS = $(SRCS:.c=.o) $(ASMS:.asm=.o)
CRT0 = $(LIBC_DIR)/crt0.o
LIBC = $(LIBC_DIR)/libc.a
all: $(TARGET)
$(TARGET): $(OBJS) $(CRT0) $(LIBC)
	$(LD) $(LDFLAGS) -o $@ $(CRT0) $(OBJS) $(LIBC)
	@ls -l $(TARGET)
%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@
%.o: %.asm
	nasm -f elf64 $< -o $@
clean:
	rm -f $(OBJS) $(TARGET)
.PHONY: all clean
