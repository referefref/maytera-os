# Angband 4.2.6 (GCU/ncurses frontend), as /APPS/ANGBAND.
# Tier 3 item #15 of docs/PORTABILITY_HOMEBREW_SNAPCRAFT_ASSESSMENT.md (#745).
#
# THIS APP HAS NO SOURCE OF ITS OWN, LIKE userland/apps/darkhttpd and
# userland/apps/lua. userland/ports/angband/PORT compiles the whole game
# (main() included) into libangband.a; this Makefile links crt0 + that
# archive + libncurses.a (the GCU frontend's terminal library, from
# userland/ports/ncurses) + libc into the ELF. One recipe, one binary, the
# version number in exactly one place (the PORT manifest).

# GNU make takes its default goal from the first explicit target it sees, and
# an `include` is expanded IN PLACE. Pin it before the include or a target
# inside ports.mk becomes the default goal (blame.md, the default-goal trap
# that shipped an unlinked app).
.DEFAULT_GOAL := all

CC = gcc
LD = ld

USERLAND_DIR = ../..
LIBC_DIR = ../../libc
PORTS_DIR = ../../ports

include $(PORTS_DIR)/ports.mk

TARGET = ANGBAND
CRT0 = $(LIBC_DIR)/crt0.o
LIBC = $(LIBC_DIR)/libc.a
LIBANGBAND = $(MPORTS_LIBDIR)/libangband.a
LIBNCURSES = $(MPORTS_LIBDIR)/libncurses.a

all: $(TARGET)

# The ports are built by userland/ports/mports.sh (needs=libc ncurses in the
# angband PORT pulls ncurses in first), which build-golden.sh runs before any
# app. Building this app standalone in a fresh tree therefore has to be able
# to produce both. LOUD, and it fails if mports fails.
$(LIBANGBAND) $(LIBNCURSES):
	@echo "angband: mports libraries not built yet; running the ports driver"
	bash $(PORTS_DIR)/mports.sh build angband

# The archive carries main(); crt0 references main, so the linker pulls the
# whole Angband object graph in. Link order: libangband.a (calls into
# ncurses' getch/refresh/etc and libc's open/read/malloc/str*), then
# libncurses.a (calls back into libc for the same POSIX surface), then
# libc.a last.
$(TARGET): $(CRT0) $(LIBC) $(LIBANGBAND) $(LIBNCURSES)
	$(LD) $(MPORTS_LDFLAGS) -o $@ $(CRT0) $(LIBANGBAND) $(LIBNCURSES) $(LIBC)
	@echo "Built $(TARGET)"

clean:
	rm -f $(TARGET)

.PHONY: all clean
