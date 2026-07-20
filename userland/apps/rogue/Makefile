# Makefile for Rogue on MayteraOS

CC = gcc
LD = ld

LIBC_DIR = ../../libc

CFLAGS = -ffreestanding -fno-stack-protector -fno-pic -mno-red-zone -mcmodel=large \
         -nostdlib -nostdinc -fno-builtin -O2 -g \
         -I. -Icompat -I$(LIBC_DIR) \
         -isystem /usr/lib/gcc/x86_64-linux-gnu/12/include \
         -DMAYTERA -DHAVE_CONFIG_H -DSAVEDIR=\"/SAVES\" -DSCOREFILE=\"/ROGUE.SCR\" \
         -DNUMSCORES=10 -DNUMNAME=80 -DWIZARD=0 \
         -Wno-implicit-function-declaration \
         -Wno-implicit-int \
         -Wno-int-conversion \
         -Wno-incompatible-pointer-types \
         -Wno-unused-function

LDFLAGS = -nostdlib -z max-page-size=0x1000 -T ../../user.ld

TARGET = rogue
OBJS = armor.o chase.o command.o curses.o daemon.o daemons.o extern.o fight.o init.o io.o list.o mach_dep.o main.o maytera_mdport.o misc.o monsters.o move.o new_level.o options.o pack.o passages.o potions.o rings.o rip.o rogue_stdio.o rooms.o save.o scrolls.o state.o sticks.o things.o vers.o weapons.o wizard.o xcrypt.o

CRT0 = $(LIBC_DIR)/crt0.o
LIBC = $(LIBC_DIR)/libc.a

all: $(TARGET)

$(TARGET): $(OBJS) $(CRT0) $(LIBC)
	$(LD) $(LDFLAGS) -o $@ $(CRT0) $(OBJS) $(LIBC)
	@echo "Built $(TARGET)"
	@ls -lh $(TARGET)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(TARGET)

.PHONY: all clean
