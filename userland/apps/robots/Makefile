# robots - bsd-games 2.17 robots, as /APPS/ROBOTS. Same shape as hangman; see
# userland/apps/hangman/Makefile's header for the pattern this follows.

.DEFAULT_GOAL := all

CC = gcc
LD = ld

USERLAND_DIR = ../..
LIBC_DIR = ../../libc
PORTS_DIR = ../../ports

include $(PORTS_DIR)/ports.mk

TARGET = ROBOTS
CRT0 = $(LIBC_DIR)/crt0.o
LIBC = $(LIBC_DIR)/libc.a
LIBROBOTS = $(MPORTS_LIBDIR)/librobots.a
LIBNCURSES = $(MPORTS_LIBDIR)/libncurses.a

all: $(TARGET)

$(LIBROBOTS) $(LIBNCURSES):
	@echo "robots: mports library not built yet; running the ports driver"
	bash $(PORTS_DIR)/mports.sh build robots

$(TARGET): $(CRT0) $(LIBC) $(LIBROBOTS) $(LIBNCURSES)
	$(LD) $(MPORTS_LDFLAGS) -o $@ $(CRT0) $(LIBROBOTS) $(LIBNCURSES) $(LIBC)
	@echo "Built $(TARGET)"

clean:
	rm -f $(TARGET)

.PHONY: all clean
