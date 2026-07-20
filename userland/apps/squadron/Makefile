# Maytera Squadron - 2D vertical shmup (userland compositor app).
CC = gcc
LD = ld
LIBC_DIR = ../../libc

CFLAGS = -ffreestanding -fno-stack-protector -fno-pic -mno-red-zone -msse -msse2 \
         -nostdlib -nostdinc -fno-builtin -Wall -Wextra -O2 -g -mcmodel=large -fno-pie \
         -I$(LIBC_DIR) -isystem /usr/lib/gcc/x86_64-linux-gnu/12/include

LDFLAGS = -nostdlib -z max-page-size=0x1000 -T ../../user.ld

TARGET = SQUADRON
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
