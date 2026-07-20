CC = gcc
LD = ld

LIBC_DIR = ../../libc

# Detect GCC include path for stdint.h etc. in freestanding mode
GCC_ISYSTEM := $(shell $(CC) -print-file-name=include 2>/dev/null)

CFLAGS = -m64 -ffreestanding -fno-builtin -nostdinc -nostdlib \
         -fno-stack-protector -mno-red-zone -Wall -Wextra \
         -I$(LIBC_DIR) -I. -I../../libgl/include -O2 \
         -isystem $(GCC_ISYSTEM)

LDFLAGS = -T ../../user.ld -nostdlib -z max-page-size=0x1000

SRCS = main.c draw.c icons.c login.c desktop.c taskbar.c \
       startmenu.c clock.c contextmenu.c screensaver.c wallpaper.c widgets.c traymenu.c profile.c \
       stickies.c notif.c bt_impl.c screenshot.c vnc.c launcher.c
OBJS = $(SRCS:.c=.o)

CRT0 = $(LIBC_DIR)/crt0.o
LIBC = $(LIBC_DIR)/libc.a
LIBGL = ../../libgl/libgl.a   # #319 TinyGL (reconciled #336)

all: COMPOSIT

COMPOSIT: $(OBJS) $(CRT0) $(LIBGL) $(LIBC)
	$(LD) $(LDFLAGS) -o $@ $(CRT0) $(OBJS) $(LIBGL) $(LIBC)
	@echo "Built COMPOSIT"
	@ls -l COMPOSIT

%.o: %.c compositor.h
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f *.o COMPOSIT

install: COMPOSIT
	cp COMPOSIT ../../../COMPOSIT

.PHONY: all clean install
