# Makefile for libSDL.a - the MayteraOS SDL 1.2 compatibility backend
# (task #745, docs/PORTABILITY_HOMEBREW_SNAPCRAFT_ASSESSMENT.md Tier 2 #7).
#
# Same compile profile as userland/libgl (TinyGL, #319): -fno-pic/-fno-pie +
# -mcmodel=large rather than the ports.mk PIE profile, because this library's
# GL path links directly against libgl.a objects built that way, and mixing
# PIC and non-PIC-large objects in one archive is the kind of thing that only
# fails at the worst possible time. Consumer apps (see
# userland/apps/sdl12test/Makefile) still link as a PIE at the user.ld fixed
# base, exactly like userland/apps/glcube already does with libgl.a.
CC = gcc
AR = ar
LIBC_DIR = ../libc
LIBGL_DIR = ../libgl

CFLAGS = -ffreestanding -fstack-protector-strong -mstack-protector-guard=global \
         -fno-pic -fno-pie -mno-red-zone -msse -msse2 \
         -nostdlib -nostdinc -fno-builtin -O2 -g -mcmodel=large \
         -Wall -Wextra -Wno-unused-parameter \
         -I. -Iinclude -Iinclude/SDL -I$(LIBC_DIR) -I$(LIBGL_DIR)/include \
         -isystem /usr/lib/gcc/x86_64-linux-gnu/12/include

SRCS = $(wildcard src/*.c)
OBJS = $(SRCS:.c=.o)

all: libSDL.a

libSDL.a: $(OBJS)
	$(AR) rcs $@ $(OBJS)
	@echo "Built libSDL.a"
	@ls -l libSDL.a
	@echo "--- symbol check (a sample of the public API this backend must define) ---"
	@nm libSDL.a | grep -E " T (SDL_Init|SDL_SetVideoMode|SDL_CreateRGBSurface|SDL_UpperBlit|SDL_Flip|SDL_PollEvent|SDL_GL_SwapBuffers|SDL_OpenAudio|SDL_CreateThread|SDL_CreateMutex)$$" | sort

src/%.o: src/%.c src/sdl_priv.h
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f src/*.o libSDL.a

.PHONY: all clean
