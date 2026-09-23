# snake - bsd-games 2.17 snake, as /APPS/SNAKE. Same shape as hangman; see
# userland/apps/hangman/Makefile's header for the pattern this follows.

.DEFAULT_GOAL := all

CC = gcc
LD = ld

USERLAND_DIR = ../..
LIBC_DIR = ../../libc
PORTS_DIR = ../../ports

include $(PORTS_DIR)/ports.mk

TARGET = SNAKE
CRT0 = $(LIBC_DIR)/crt0.o
LIBC = $(LIBC_DIR)/libc.a
LIBSNAKE = $(MPORTS_LIBDIR)/libsnake.a
LIBNCURSES = $(MPORTS_LIBDIR)/libncurses.a

all: $(TARGET)

$(LIBSNAKE) $(LIBNCURSES):
	@echo "snake: mports library not built yet; running the ports driver"
	bash $(PORTS_DIR)/mports.sh build snake

$(TARGET): $(CRT0) $(LIBC) $(LIBSNAKE) $(LIBNCURSES)
	$(LD) $(MPORTS_LDFLAGS) -o $@ $(CRT0) $(LIBSNAKE) $(LIBNCURSES) $(LIBC)
	@echo "Built $(TARGET)"

clean:
	rm -f $(TARGET)

.PHONY: all clean
