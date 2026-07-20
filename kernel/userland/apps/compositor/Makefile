CC = gcc
LD = ld

LIBC_DIR = ../../libc

# Detect GCC include path for stdint.h etc. in freestanding mode
GCC_ISYSTEM := $(shell $(CC) -print-file-name=include 2>/dev/null)

CFLAGS = -m64 -ffreestanding -fno-builtin -nostdinc -nostdlib \
         -fno-stack-protector -mno-red-zone -Wall -Wextra \
         -I$(LIBC_DIR) -I. -O2 \
         -isystem $(GCC_ISYSTEM)

LDFLAGS = -T ../../user.ld -nostdlib -z max-page-size=0x1000

SRCS = main.c draw.c icons.c login.c desktop.c taskbar.c \
       startmenu.c clock.c contextmenu.c screensaver.c wallpaper.c
OBJS = $(SRCS:.c=.o)

CRT0 = $(LIBC_DIR)/crt0.o
LIBC = $(LIBC_DIR)/libc.a

all: COMPOSIT

COMPOSIT: $(OBJS) $(CRT0) $(LIBC)
	$(LD) $(LDFLAGS) -o $@ $(CRT0) $(OBJS) $(LIBC)
	@echo "Built COMPOSIT"
	@ls -l COMPOSIT

%.o: %.c compositor.h
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f *.o COMPOSIT

install: COMPOSIT
	cp COMPOSIT ../../../COMPOSIT

.PHONY: all clean install
