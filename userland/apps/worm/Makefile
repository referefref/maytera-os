# worm - bsd-games 2.17 worm, as /APPS/WORM. Same shape as hangman; see
# userland/apps/hangman/Makefile's header for the pattern this follows.

.DEFAULT_GOAL := all

CC = gcc
LD = ld

USERLAND_DIR = ../..
LIBC_DIR = ../../libc
PORTS_DIR = ../../ports

include $(PORTS_DIR)/ports.mk

TARGET = WORM
CRT0 = $(LIBC_DIR)/crt0.o
LIBC = $(LIBC_DIR)/libc.a
LIBWORM = $(MPORTS_LIBDIR)/libworm.a
LIBNCURSES = $(MPORTS_LIBDIR)/libncurses.a

all: $(TARGET)

$(LIBWORM) $(LIBNCURSES):
	@echo "worm: mports library not built yet; running the ports driver"
	bash $(PORTS_DIR)/mports.sh build worm

$(TARGET): $(CRT0) $(LIBC) $(LIBWORM) $(LIBNCURSES)
	$(LD) $(MPORTS_LDFLAGS) -o $@ $(CRT0) $(LIBWORM) $(LIBNCURSES) $(LIBC)
	@echo "Built $(TARGET)"

clean:
	rm -f $(TARGET)

.PHONY: all clean
