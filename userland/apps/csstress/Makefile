CC = gcc
LD = ld
LIBC_DIR = ../../libc
BUILD_DIR = build
CFLAGS = -ffreestanding -fno-stack-protector -fno-pic -mno-red-zone -mcmodel=large \
         -nostdlib -nostdinc -fno-builtin -Wall -Wextra -O2 -g \
         -I$(LIBC_DIR) -isystem /usr/lib/gcc/x86_64-linux-gnu/12/include
LDFLAGS = -nostdlib -z max-page-size=0x1000 -T ../../user.ld
CRT0 = $(LIBC_DIR)/crt0.o
LIBC = $(LIBC_DIR)/libc.a

all: $(BUILD_DIR) csstress

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

csstress: $(BUILD_DIR)/main.o $(CRT0) $(LIBC)
	$(LD) $(LDFLAGS) -o $@ $(CRT0) $(BUILD_DIR)/main.o $(LIBC)

$(BUILD_DIR)/main.o: main.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -rf $(BUILD_DIR) csstress
.PHONY: all clean
