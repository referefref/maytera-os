# trunchk - the running proof for #745 local 109 (O_TRUNC on ext2 and FAT).
#
# Modelled on userland/apps/regexchk/Makefile, minus the ports include: this
# app links nothing but libc.a, because everything it tests is either a shipped
# binary it spawns or a libc function.
.DEFAULT_GOAL := all

CC = gcc
LD = ld

USERLAND_DIR = ../..
LIBC_DIR = ../../libc

CFLAGS = -ffreestanding -fstack-protector-strong -mstack-protector-guard=global -fPIE -mno-red-zone \
         -nostdlib -nostdinc -fno-builtin -Wall -Wextra -O2 -g \
         -I$(LIBC_DIR) -isystem /usr/lib/gcc/x86_64-linux-gnu/12/include

LDFLAGS = -pie --no-dynamic-linker -z notext -nostdlib -z max-page-size=0x1000 \
          -T $(USERLAND_DIR)/user-pie.ld

TARGET = TRUNCHK
OBJS = main.o
CRT0 = $(LIBC_DIR)/crt0.o
LIBC = $(LIBC_DIR)/libc.a

all: $(TARGET)

$(TARGET): $(OBJS) $(CRT0) $(LIBC)
	$(LD) $(LDFLAGS) -o $@ $(CRT0) $(OBJS) $(LIBC)
	@echo "Built $(TARGET)"

# -MMD -MP: a header-only change must recompile, or a stale object silently
# re-links and the fix "does not take" (blame.md, #600 and #568).
main.o: main.c
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

clean:
	rm -f $(OBJS) main.d $(TARGET)

-include main.d

.PHONY: all clean
