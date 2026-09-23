# myman - the upstream myman 0.7.0 ncurses Pac-Man clone, as /APPS/MYMAN.
# Tier 3 item #17 of docs/PORTABILITY_HOMEBREW_SNAPCRAFT_ASSESSMENT.md (#745):
# "myman, ninvaders, moon-buggy ... near-zero individual cost once ncurses
# lands. Useful specifically as VOLUME PROOF that the recipe system works".
#
# THIS APP HAS NO SOURCE OF ITS OWN, LIKE userland/apps/darkhttpd. myman is a
# single-file PROGRAM: userland/ports/myman/PORT compiles src/myman.c
# (main() included) into libmyman.a, and this Makefile links crt0 + that
# archive + libncurses.a + libc into the ELF. Vendoring myman.c into this
# directory would put upstream source back in the repository, which the
# mports mechanism exists to avoid, and would need its own licence coverage.
# One recipe, one binary, the version number in exactly one place (the PORT
# manifest).
#
# RUNTIME DATA. Unlike darkhttpd, this binary needs three small text files at
# runtime (the "square" variant's tile/sprite/maze data): /APPS/chr/khr1.txt,
# /APPS/spr/spr1.txt, /APPS/lvl/maze.txt. myman's own fopen_datafile()
# resolves a relative data path against argv[0]'s directory when a plain
# relative open fails, and the Terminal resolves a launched program's argv[0]
# to its full /APPS path before spawning it, so /APPS/MYMAN finds
# /APPS/chr/khr1.txt etc. with no MayteraOS-specific plumbing. The three
# files themselves are placed by build/build-golden.sh's MYMAN block, copied
# from userland/ports/out/src/{khr1.txt,spr1.txt,maze.txt} (installed there
# by the PORT's `install_sources`, extracted from the same sha256-pinned
# tarball - never vendored into git).

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

TARGET = MYMAN
CRT0 = $(LIBC_DIR)/crt0.o
LIBC = $(LIBC_DIR)/libc.a
LIBMYMAN = $(MPORTS_LIBDIR)/libmyman.a
LIBNCURSES = $(MPORTS_LIBDIR)/libncurses.a

all: $(TARGET)

# The ports are built by userland/ports/mports.sh, which build-golden.sh runs
# before any app. Building this app standalone in a fresh tree therefore has
# to be able to produce them. LOUD, and it fails if mports fails.
$(LIBMYMAN) $(LIBNCURSES):
	@echo "myman: mports library not built yet; running the ports driver"
	bash $(PORTS_DIR)/mports.sh build myman

# The archive carries main(); crt0 references main, so the linker pulls the
# whole myman object in. Link order: libmyman.a, then libncurses.a (myman
# calls into it), then libc.a last (both call str*/malloc/fopen out of it).
$(TARGET): $(CRT0) $(LIBC) $(LIBMYMAN) $(LIBNCURSES)
	$(LD) $(MPORTS_LDFLAGS) -o $@ $(CRT0) $(LIBMYMAN) $(LIBNCURSES) $(LIBC)
	@echo "Built $(TARGET)"

clean:
	rm -f $(TARGET)

.PHONY: all clean
