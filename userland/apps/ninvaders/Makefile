# ninvaders - the upstream ninvaders 0.1.1 ncurses Space Invaders clone, as
# /APPS/NINVADERS. Tier 3 item #17 of
# docs/PORTABILITY_HOMEBREW_SNAPCRAFT_ASSESSMENT.md (#745): "myman,
# ninvaders, moon-buggy ... near-zero individual cost once ncurses lands.
# Useful specifically as VOLUME PROOF that the recipe system works".
#
# THIS APP HAS NO SOURCE OF ITS OWN, LIKE userland/apps/darkhttpd. ninvaders
# is a small PROGRAM (six translation units, main() in nInvaders.c):
# userland/ports/ninvaders/PORT compiles them into libninvaders.a, and this
# Makefile links crt0 + that archive + libncurses.a + libc into the ELF.
# Vendoring the sources into this directory would put upstream source back in
# the repository, which the mports mechanism exists to avoid, and would need
# its own licence coverage. One recipe, one binary, the version number in
# exactly one place (the PORT manifest).
#
# NO RUNTIME DATA FILES. Unlike myman, ninvaders needs nothing beyond the
# terminal itself: no maze/tile/sprite text files, no sound.
#
# LICENCE NOTE (see ATTRIBUTION.md and userland/ports/ninvaders/PORT):
# ninvaders is GPL-2.0-or-later. Statically combining it here with the
# permissively-licensed libncurses.a and libc.a means the resulting
# /APPS/NINVADERS binary as a WHOLE must be offered under GPL-2.0-or-later;
# neither ncurses nor libc is thereby made GPL (same shape as /APPS/VI).

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

TARGET = NINVADERS
CRT0 = $(LIBC_DIR)/crt0.o
LIBC = $(LIBC_DIR)/libc.a
LIBNINVADERS = $(MPORTS_LIBDIR)/libninvaders.a
LIBNCURSES = $(MPORTS_LIBDIR)/libncurses.a

all: $(TARGET)

# The ports are built by userland/ports/mports.sh, which build-golden.sh runs
# before any app. Building this app standalone in a fresh tree therefore has
# to be able to produce them. LOUD, and it fails if mports fails.
$(LIBNINVADERS) $(LIBNCURSES):
	@echo "ninvaders: mports library not built yet; running the ports driver"
	bash $(PORTS_DIR)/mports.sh build ninvaders

# The archive carries main() (in nInvaders.o); crt0 references main, so the
# linker pulls the whole set in. Link order: libninvaders.a, then
# libncurses.a (ninvaders calls into it), then libc.a last.
$(TARGET): $(CRT0) $(LIBC) $(LIBNINVADERS) $(LIBNCURSES)
	$(LD) $(MPORTS_LDFLAGS) -o $@ $(CRT0) $(LIBNINVADERS) $(LIBNCURSES) $(LIBC)
	@echo "Built $(TARGET)"

clean:
	rm -f $(TARGET)

.PHONY: all clean
