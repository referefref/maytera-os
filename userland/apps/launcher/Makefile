# Makefile for launcher - Command Palette / app launcher
CC = gcc
LD = ld
LIBC_DIR = ../../libc
USERLAND_DIR = ../..
CFLAGS = -ffreestanding -fstack-protector-strong -mstack-protector-guard=global -fPIE -mno-red-zone \
         -nostdlib -nostdinc -fno-builtin -Wall -Wextra -O2 -g \
         -I$(LIBC_DIR) -isystem /usr/lib/gcc/x86_64-linux-gnu/12/include
LDFLAGS = -pie --no-dynamic-linker -z notext -nostdlib -z max-page-size=0x1000 -T $(USERLAND_DIR)/user-pie.ld
TARGET = launcher
OBJS = main.o
CRT0 = $(LIBC_DIR)/crt0.o
LIBC = $(LIBC_DIR)/libc.a
all: $(TARGET)
$(TARGET): $(OBJS) $(CRT0) $(LIBC)
	$(LD) $(LDFLAGS) -o $@ $(CRT0) $(OBJS) $(LIBC)
	@echo "Built $(TARGET)"; ls -l $(TARGET)
%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@
clean:
	rm -f $(OBJS) $(TARGET)
.PHONY: all clean
