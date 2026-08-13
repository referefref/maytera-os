# Makefile for pietest - #640 stage 1 PIE proof app.
#
# This is the FIRST ET_DYN binary in the tree. It is deliberately libc-free
# (see main.c) so that a result says something about the KERNEL loader and not
# about whether libc happens to be position-independent yet.
#
# -fPIE  : RIP-relative code and data references. Also the reason no
#          -mcmodel=large appears here: RIP-relative addressing has no 2GB
#          signed-displacement problem to work around, which is the whole
#          argument for PIE over a fixed high base (#640).
# -pie   : produce ET_DYN with a PT_DYNAMIC + .rela.dyn, which is what
#          kernel/exec/elf.c's relocation engine consumes.

CC = gcc
LD = ld

USERLAND_DIR = ../..

# #651 EXEMPTION - do not "fix" this by adding -fstack-protector-strong.
# pietest deliberately links NO libc (see main.c), and the stack-protector
# runtime (__stack_chk_guard / __stack_chk_fail) lives in libc.a. Turning the
# protector on here produces undefined references at link. Defining a private
# guard inside pietest would fork a shared primitive, which this tree bans,
# and would blunt what pietest exists to prove. It stays unprotected on
# purpose: it is a ~100-line diagnostic harness, not shipped app surface.
CFLAGS = -ffreestanding -fno-stack-protector -fPIE -mno-red-zone \
         -nostdlib -nostdinc -fno-builtin -Wall -Wextra -Werror -O2 -g \
         -isystem /usr/lib/gcc/x86_64-linux-gnu/12/include

LDFLAGS = -nostdlib -pie --no-dynamic-linker -z max-page-size=0x1000 -z notext \
          -T $(USERLAND_DIR)/user-pie.ld

TARGET = pietest
OBJECTS = start.o main.o

all: $(TARGET)

$(TARGET): $(OBJECTS)
	$(LD) $(LDFLAGS) -o $@ $(OBJECTS)
	@echo "Built $(TARGET)"
	@readelf -hd $@ | grep -E 'Type:|RELA|Entry' || true

start.o: start.S
	$(CC) $(CFLAGS) -c $< -o $@

main.o: main.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJECTS) $(TARGET)

.PHONY: all clean
