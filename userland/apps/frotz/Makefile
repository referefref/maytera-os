# frotz - the dumb/terminal Frotz Z-machine interpreter, as /APPS/FROTZ.
# See userland/ports/frotz/PORT: no ncurses dependency (the dumb interface
# is plain stdio), no bundled story file (matches the gbemu no-ROM
# precedent) - launch with a path argument to a .z1-.z8 story file.

.DEFAULT_GOAL := all

CC = gcc
LD = ld

USERLAND_DIR = ../..
LIBC_DIR = ../../libc
PORTS_DIR = ../../ports

include $(PORTS_DIR)/ports.mk

TARGET = FROTZ
CRT0 = $(LIBC_DIR)/crt0.o
LIBC = $(LIBC_DIR)/libc.a
LIBFROTZ = $(MPORTS_LIBDIR)/libfrotz.a

all: $(TARGET)

$(LIBFROTZ):
	@echo "frotz: mports library not built yet; running the ports driver"
	bash $(PORTS_DIR)/mports.sh build frotz

$(TARGET): $(CRT0) $(LIBC) $(LIBFROTZ)
	$(LD) $(MPORTS_LDFLAGS) -o $@ $(CRT0) $(LIBFROTZ) $(LIBC)
	@echo "Built $(TARGET)"

clean:
	rm -f $(TARGET)

.PHONY: all clean
