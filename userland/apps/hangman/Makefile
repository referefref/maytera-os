# hangman - the classic bsd-games 2.17 hangman, as /APPS/HANGMAN. Part of the
# tuigames batch (owner request 2026-09-18), same shape as myman/ninvaders/
# moon-buggy: userland/ports/hangman/PORT compiles the upstream sources (plus
# our own bsdgame_compat.c/bsdgame_words.c) into libhangman.a, and this
# Makefile links crt0 + that archive + libncurses.a + libc into the ELF.
#
# NO RUNTIME DATA. Unlike myman, this binary needs no data files: the word
# list is compiled in (see userland/ports/hangman/bsdgame_words.c and the
# PORT's patch series), so build/build-golden.sh needs no asset-overlay block
# for this app, only the two golden-new-apps.list / ATTRIBUTION.md entries.

.DEFAULT_GOAL := all

CC = gcc
LD = ld

USERLAND_DIR = ../..
LIBC_DIR = ../../libc
PORTS_DIR = ../../ports

include $(PORTS_DIR)/ports.mk

TARGET = HANGMAN
CRT0 = $(LIBC_DIR)/crt0.o
LIBC = $(LIBC_DIR)/libc.a
LIBHANGMAN = $(MPORTS_LIBDIR)/libhangman.a
LIBNCURSES = $(MPORTS_LIBDIR)/libncurses.a

all: $(TARGET)

$(LIBHANGMAN) $(LIBNCURSES):
	@echo "hangman: mports library not built yet; running the ports driver"
	bash $(PORTS_DIR)/mports.sh build hangman

$(TARGET): $(CRT0) $(LIBC) $(LIBHANGMAN) $(LIBNCURSES)
	$(LD) $(MPORTS_LDFLAGS) -o $@ $(CRT0) $(LIBHANGMAN) $(LIBNCURSES) $(LIBC)
	@echo "Built $(TARGET)"

clean:
	rm -f $(TARGET)

.PHONY: all clean
