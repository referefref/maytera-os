# userland/apps/bc - Gavin Howard's bc (bc-gh 7.0.3), installed as /APPS/BC.
# Tier 2 item #11 of docs/PORTABILITY_HOMEBREW_SNAPCRAFT_ASSESSMENT.md (#745).
#
# THIS APP HAS NO SOURCE OF ITS OWN, LIKE userland/apps/darkhttpd and
# userland/apps/myman. bc is a single PROGRAM: userland/ports/bc/PORT compiles
# the upstream engine (main() included) into libbc.a, and this Makefile links
# crt0 + that archive + libc into the ELF. Vendoring bc's source into this
# directory would put upstream code back in the repository, which the mports
# mechanism exists to avoid, and would need its own licence coverage. One
# recipe, one binary, the version number in exactly one place (the PORT
# manifest).
#
# This REPLACES the old integer-only homegrown bc that used to live here
# (main.c, #354): bc-gh is the full bc language with a real fractional scale,
# user-defined functions and the -l math library.

# GNU make takes its default goal from the first explicit target it sees, and an
# `include` is expanded IN PLACE. Pin it before the include or a target inside
# ports.mk (its trailing print-% rule) becomes the default goal and a bare
# `make` builds the wrong thing and exits 0 (blame.md, the default-goal trap).
.DEFAULT_GOAL := all

CC = gcc
LD = ld

USERLAND_DIR = ../..
LIBC_DIR = ../../libc
PORTS_DIR = ../../ports

include $(PORTS_DIR)/ports.mk

TARGET = bc
CRT0 = $(LIBC_DIR)/crt0.o
LIBC = $(LIBC_DIR)/libc.a
LIBBC = $(MPORTS_LIBDIR)/libbc.a

all: $(TARGET)

# The port is built by userland/ports/mports.sh, which build-golden.sh runs
# before any app. Building this app standalone in a fresh tree must still be able
# to produce it, so this is LOUD and fails if mports fails: not a fallback that
# quietly substitutes something else.
$(LIBBC):
	@echo "bc: libbc.a not built yet; running the ports driver"
	bash $(PORTS_DIR)/mports.sh build bc

# The archive carries main(); crt0 references main, so the linker pulls the whole
# bc program in. Link order: libbc.a (calls str*/malloc/math/read/write out of
# libc), then libc.a last.
$(TARGET): $(CRT0) $(LIBBC) $(LIBC)
	$(LD) $(MPORTS_LDFLAGS) -o $@ $(CRT0) $(LIBBC) $(LIBC)
	@echo "Built $(TARGET)"

clean:
	rm -f $(TARGET)

.PHONY: all clean
