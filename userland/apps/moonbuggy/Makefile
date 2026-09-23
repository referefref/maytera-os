# moonbuggy - the upstream moon-buggy 1.0.51 ncurses side-scroller, as
# /APPS/MOONBUGY. Tier 3 item #17 of
# docs/PORTABILITY_HOMEBREW_SNAPCRAFT_ASSESSMENT.md: "myman, ninvaders,
# moon-buggy ... near-zero individual cost once ncurses lands. Useful
# specifically as VOLUME PROOF that the recipe system works". The third and
# last of the three named ncurses terminal games.
#
# THIS APP HAS NO SOURCE OF ITS OWN, LIKE userland/apps/ninvaders and
# userland/apps/darkhttpd. moon-buggy is a PROGRAM (26 translation units,
# main() in main.c): userland/ports/moonbuggy compiles them into
# libmoonbugy.a (build=script, because upstream generates config.h, copying.h
# and buggy.h), and this Makefile links crt0 + that archive + libncurses.a +
# libc into the ELF. Vendoring the sources here would put upstream source back
# in the repository, which mports exists to avoid.
#
# NO RUNTIME DATA FILES. Unlike myman, moon-buggy needs nothing beyond the
# terminal: the buggy sprites are baked into buggy.h at build time (upstream's
# img.sed), not read at runtime. The highscore file (SCORE_DIR/mbscore, i.e.
# /HOME/mbscore) is created by the game itself on first run; nothing to ship.
#
# LICENCE NOTE (see ATTRIBUTION.md and userland/ports/moonbuggy/PORT):
# moon-buggy is GPL-2.0-or-later. Statically combining it here with the
# permissively-licensed libncurses.a and libc.a means the resulting
# /APPS/MOONBUGY binary as a WHOLE must be offered under GPL-2.0-or-later;
# neither ncurses nor libc is thereby made GPL (same shape as /APPS/NINVADERS).

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

TARGET = MOONBUGY
CRT0 = $(LIBC_DIR)/crt0.o
LIBC = $(LIBC_DIR)/libc.a
LIBMOONBUGY = $(MPORTS_LIBDIR)/libmoonbugy.a
LIBNCURSES = $(MPORTS_LIBDIR)/libncurses.a

all: $(TARGET)

# The ports are built by userland/ports/mports.sh, which build-golden.sh runs
# before any app. Building this app standalone in a fresh tree therefore has
# to be able to produce them. LOUD, and it fails if mports fails.
$(LIBMOONBUGY) $(LIBNCURSES):
	@echo "moonbuggy: mports library not built yet; running the ports driver"
	bash $(PORTS_DIR)/mports.sh build moonbuggy

# The archive carries main() (in mb_main.o); crt0 references main, so the
# linker pulls the whole set in. Link order: libmoonbugy.a, then libncurses.a
# (moon-buggy calls into it), then libc.a last.
$(TARGET): $(CRT0) $(LIBC) $(LIBMOONBUGY) $(LIBNCURSES)
	$(LD) $(MPORTS_LDFLAGS) -o $@ $(CRT0) $(LIBMOONBUGY) $(LIBNCURSES) $(LIBC)
	@echo "Built $(TARGET)"

clean:
	rm -f $(TARGET)

.PHONY: all clean
