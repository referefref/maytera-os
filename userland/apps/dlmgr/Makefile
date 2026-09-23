# Makefile for dlmgr - MayteraOS user-mode Downloads (download manager) app.
# Same flags as the other GUI apps (weather/rss); compiles the shared async
# HTTP/1.1 client from userland/lib/netapps alongside main.c, the way the
# OPL2 consumers compile userland/lib/opl2.

CC = gcc
LD = ld

LIBC_DIR = ../../libc
NETLIB_DIR = ../../lib/netapps

CFLAGS = -ffreestanding -fstack-protector-strong -mstack-protector-guard=global -fPIE -mno-red-zone \
         -nostdlib -nostdinc -fno-builtin -Wall -Wextra -O2 -g \
         -I$(LIBC_DIR) -isystem /usr/lib/gcc/x86_64-linux-gnu/12/include

LDFLAGS = -pie --no-dynamic-linker -z notext -nostdlib -z max-page-size=0x1000 -T ../../user-pie.ld

TARGET = DLMGR

OBJS = main.o httpx.o

CRT0 = $(LIBC_DIR)/crt0.o
LIBC = $(LIBC_DIR)/libc.a

all: $(TARGET)

$(TARGET): $(OBJS) $(CRT0) $(LIBC)
	$(LD) $(LDFLAGS) -o $@ $(CRT0) $(OBJS) $(LIBC)
	@echo "Built $(TARGET)"
	@ls -l $(TARGET)

main.o: main.c $(NETLIB_DIR)/netui.h $(NETLIB_DIR)/httpx.h
	$(CC) $(CFLAGS) -c $< -o $@

httpx.o: $(NETLIB_DIR)/httpx.c $(NETLIB_DIR)/httpx.h
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(TARGET)

install: $(TARGET)
	mkdir -p ../../../apps
	cp $(TARGET) ../../../apps/
	@echo "Installed to ../../../apps/"

.PHONY: all clean install
