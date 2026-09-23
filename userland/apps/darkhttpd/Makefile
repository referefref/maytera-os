# darkhttpd - the upstream darkhttpd 1.16 static web server, as /APPS/DARKHTTPD.
# Tier 2 item #12 of docs/PORTABILITY_HOMEBREW_SNAPCRAFT_ASSESSMENT.md (#745).
#
# THIS APP HAS NO SOURCE OF ITS OWN, LIKE userland/apps/lua. darkhttpd is a
# single-file PROGRAM: userland/ports/darkhttpd/PORT compiles darkhttpd.c
# (main() included) into libdarkhttpd.a, and this Makefile links crt0 + that
# archive + libc into the ELF. Vendoring darkhttpd.c into this directory would
# put upstream source back in the repository, which the mports mechanism exists
# to avoid, and would need its own licence coverage. One recipe, one binary, the
# version number in exactly one place (the PORT manifest).

# GNU make takes its default goal from the first explicit target it sees, and an
# `include` is expanded IN PLACE. Pin it before the include or a target inside
# ports.mk becomes the default goal (blame.md, the default-goal trap that
# shipped an unlinked app).
.DEFAULT_GOAL := all

CC = gcc
LD = ld

USERLAND_DIR = ../..
LIBC_DIR = ../../libc
PORTS_DIR = ../../ports

include $(PORTS_DIR)/ports.mk

TARGET = DARKHTTPD
CRT0 = $(LIBC_DIR)/crt0.o
LIBC = $(LIBC_DIR)/libc.a
LIBDARKHTTPD = $(MPORTS_LIBDIR)/libdarkhttpd.a

all: $(TARGET)

# The port is built by userland/ports/mports.sh, which build-golden.sh runs
# before any app. Building this app standalone in a fresh tree therefore has to
# be able to produce it. LOUD, and it fails if mports fails.
$(LIBDARKHTTPD):
	@echo "darkhttpd: libdarkhttpd.a not built yet; running the ports driver"
	bash $(PORTS_DIR)/mports.sh build darkhttpd

# The archive carries main(); crt0 references main, so the linker pulls the
# whole darkhttpd object in. Link order: libdarkhttpd.a after crt0, libc.a last,
# because darkhttpd calls socket/malloc/str*/open out of libc.
$(TARGET): $(CRT0) $(LIBC) $(LIBDARKHTTPD)
	$(LD) $(MPORTS_LDFLAGS) -o $@ $(CRT0) $(LIBDARKHTTPD) $(LIBC)
	@echo "Built $(TARGET)"

clean:
	rm -f $(TARGET)

.PHONY: all clean
