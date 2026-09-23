# Makefile for restcli - MayteraOS user-mode HTTP Client (REST request tester).
# Same flags as the other GUI apps; compiles the shared async HTTP/1.1 client
# from userland/lib/netapps alongside main.c (see dlmgr/Makefile).

CC = gcc
LD = ld

LIBC_DIR = ../../libc
NETLIB_DIR = ../../lib/netapps

CFLAGS = -ffreestanding -fstack-protector-strong -mstack-protector-guard=global -fPIE -mno-red-zone \
         -nostdlib -nostdinc -fno-builtin -Wall -Wextra -O2 -g \
         -I$(LIBC_DIR) -isystem /usr/lib/gcc/x86_64-linux-gnu/12/include

LDFLAGS = -pie --no-dynamic-linker -z notext -nostdlib -z max-page-size=0x1000 -T ../../user-pie.ld

TARGET = RESTCLI

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
