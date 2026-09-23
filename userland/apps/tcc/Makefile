# userland/apps/tcc - the TinyCC command-line C compiler, installed as /APPS/TCC
# (#745, Tier 2 item #7 of docs/PORTABILITY_HOMEBREW_SNAPCRAFT_ASSESSMENT.md).
#
# THIS APP HAS NO SOURCE OF ITS OWN, AND THAT IS DELIBERATE. tcc's CLI main() is
# compiled by the mports recipe (userland/ports/tcc) as ONE_SOURCE and archived
# INTO libtcc.a, because tcc.c pulls the whole compiler (tccpp/tccgen/tccelf/
# x86_64-*) into one translation unit whose compile flags must match exactly.
# Keeping main() in the archive means those flags live in ONE place (the port),
# not duplicated here where they could drift. crt0's reference to main pulls
# tcc_main.o out of the archive, which pulls maytera_compat.o (the weak strtold
# shim) with it. So this Makefile only links. Same shape as the jq/darkhttpd/
# angband apps.
#
# The compiler's own runtime (its headers, libtcc1.a, and a copy of libc.a +
# crt0.o for a device-compiled program to link against) is shipped SEPARATELY to
# /APPS/tcc by the port's install_data + build-golden.sh's overlay block; it is
# not part of THIS binary.

# GNU make takes its default goal from the first explicit target it sees, and an
# `include` is expanded IN PLACE. Pin it before the include or a target inside
# ports.mk becomes the default goal and a bare `make` builds the wrong thing and
# exits 0 (blame.md, the default-goal trap that shipped an unlinked app).
.DEFAULT_GOAL := all

CC = gcc
LD = ld

USERLAND_DIR = ../..
LIBC_DIR = ../../libc
PORTS_DIR = ../../ports

include $(PORTS_DIR)/ports.mk

TARGET = TCC
CRT0 = $(LIBC_DIR)/crt0.o
LIBC = $(LIBC_DIR)/libc.a
LIBTCC = $(MPORTS_LIBDIR)/libtcc.a

all: $(TARGET)

# The port is built by userland/ports/mports.sh, which build-golden.sh runs
# before any app. Building this app standalone in a fresh tree therefore has to
# be able to produce it. LOUD, and it fails if mports fails: it is not a
# fallback that quietly substitutes something else.
$(LIBTCC):
	@echo "tcc: libtcc.a not built yet; running the ports driver"
	bash $(PORTS_DIR)/mports.sh build tcc

# Link order: libtcc.a (which defines main and pulls in the whole compiler)
# after crt0, libc.a last because tcc calls memcpy/malloc/str*/math and the
# file/time entry points out of libc.
$(TARGET): $(CRT0) $(LIBC) $(LIBTCC)
	$(LD) $(MPORTS_LDFLAGS) -o $@ $(CRT0) $(LIBTCC) $(LIBC)
	@echo "Built $(TARGET)"

clean:
	rm -f $(TARGET)

.PHONY: all clean
