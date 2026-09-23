# Makefile for SHEETS.ELF - MayteraOS office suite app (skeleton).
CC = gcc
LD = ld
LIBC_DIR = ../../libc
OFFICELIB = ../../officelib
USERLAND_DIR = ../..
CFLAGS = -ffreestanding -fstack-protector-strong -mstack-protector-guard=global -fPIE -mno-red-zone \
         -nostdlib -nostdinc -fno-builtin -Wall -Wextra -O2 -g \
         -I$(LIBC_DIR) -I$(OFFICELIB)/include -isystem /usr/lib/gcc/x86_64-linux-gnu/12/include
LDFLAGS = -pie --no-dynamic-linker -z notext -nostdlib -z max-page-size=0x1000 -T $(USERLAND_DIR)/user-pie.ld
TARGET = SHEETS
OBJS = main.o
CRT0 = $(LIBC_DIR)/crt0.o
LIBC = $(LIBC_DIR)/libc.a
LIBOFFICE = $(OFFICELIB)/libmoffice.a
all: $(TARGET)
$(LIBOFFICE):
	$(MAKE) -C $(OFFICELIB)
$(TARGET): $(OBJS) $(CRT0) $(LIBOFFICE) $(LIBC)
	$(LD) $(LDFLAGS) -o $@ $(CRT0) $(OBJS) $(LIBOFFICE) $(LIBC)
	@ls -l $(TARGET)
%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@
clean:
	rm -f $(OBJS) $(TARGET)
.PHONY: all clean
