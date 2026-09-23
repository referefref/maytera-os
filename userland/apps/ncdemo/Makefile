# Makefile for ncdemo - the ncurses port smoke-test app (#745).
# Links the mports ncurses static library (built into userland/ports/out before
# the golden's app loop runs) plus the shared libc.
CC = gcc
LD = ld

LIBC_DIR = ../../libc
USERLAND_DIR = ../..
PORTS_OUT = ../../ports/out

CFLAGS = -ffreestanding -fstack-protector-strong -mstack-protector-guard=global -fPIE -mno-red-zone \
         -nostdlib -nostdinc -fno-builtin -Wall -Wextra -O2 -g \
         -I$(LIBC_DIR) -isystem /usr/lib/gcc/x86_64-linux-gnu/12/include \
         -I$(PORTS_OUT)/include

LDFLAGS = -pie --no-dynamic-linker -z notext -nostdlib -z max-page-size=0x1000 -T $(USERLAND_DIR)/user-pie.ld

TARGET = ncdemo
CRT0 = $(LIBC_DIR)/crt0.o
LIBC = $(LIBC_DIR)/libc.a
LIBNCURSES = $(PORTS_OUT)/lib/libncurses.a

all: $(TARGET)

$(TARGET): main.o $(CRT0) $(LIBC) $(LIBNCURSES)
	$(LD) $(LDFLAGS) -o $@ $(CRT0) main.o $(LIBNCURSES) $(LIBC)
	@echo "Built $(TARGET)"

main.o: main.c
	$(CC) $(CFLAGS) -c main.c -o main.o

clean:
	rm -f main.o $(TARGET)

.PHONY: all clean
