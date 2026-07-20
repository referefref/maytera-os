# Makefile for help - the MayteraOS Help viewer (task #267)

CC = gcc
LD = ld

LIBC_DIR    = ../../libc
LIBHELP_DIR = ../../libhelp
USERLAND_DIR = ../..

CFLAGS = -ffreestanding -fno-stack-protector -fno-pic -mno-red-zone -mcmodel=large \
         -nostdlib -nostdinc -fno-builtin -Wall -Wextra -O2 -g \
         -I$(LIBC_DIR) -I$(LIBHELP_DIR) \
         -isystem /usr/lib/gcc/x86_64-linux-gnu/12/include

LDFLAGS = -nostdlib -z max-page-size=0x1000 -T $(USERLAND_DIR)/user.ld

TARGET = help
OBJS = main.o

CRT0    = $(LIBC_DIR)/crt0.o
LIBC    = $(LIBC_DIR)/libc.a
LIBHELP = $(LIBHELP_DIR)/libhelp.a

all: $(TARGET)

$(TARGET): $(OBJS) $(CRT0) $(LIBHELP) $(LIBC)
	$(LD) $(LDFLAGS) -o $@ $(CRT0) $(OBJS) $(LIBHELP) $(LIBC)
	@echo "Built $(TARGET)"
	@ls -l $(TARGET)

# Ensure libhelp is built first.
$(LIBHELP):
	$(MAKE) -C $(LIBHELP_DIR)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(TARGET)

.PHONY: all clean
