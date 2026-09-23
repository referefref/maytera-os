# jq - the real upstream jq 1.8.1 JSON processor, as /APPS/JQ (#745, Tier 2 #10).
#
# THIS APP HAS NO SOURCE OF ITS OWN, AND THAT IS DELIBERATE. jq's CLI main() is
# compiled by the mports recipe (userland/ports/jq) and archived INTO libjq.a,
# because main.c includes jq-private and generated headers (util.h,
# src/version.h, src/config_opts.inc) whose compile flags must match the library
# exactly. Keeping main() in the archive means those flags live in ONE place
# (the port), not duplicated here where they could drift. crt0's reference to
# main pulls main.o out of the archive, which pulls the rest of jq, Oniguruma
# and decNumber. So this Makefile only links.
#
# One recipe therefore produces BOTH a linkable libjq.a and a usable /APPS/JQ,
# and the version number appears in exactly one place (userland/ports/jq/PORT).

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

TARGET = JQ
CRT0 = $(LIBC_DIR)/crt0.o
LIBC = $(LIBC_DIR)/libc.a
LIBJQ = $(MPORTS_LIBDIR)/libjq.a

all: $(TARGET)

# The port is built by userland/ports/mports.sh, which build-golden.sh runs
# before any app. Building this app standalone in a fresh tree therefore has to
# be able to produce it. LOUD, and it fails if mports fails: it is not a
# fallback that quietly substitutes something else.
$(LIBJQ):
	@echo "jq: libjq.a not built yet; running the ports driver"
	bash $(PORTS_DIR)/mports.sh build jq

# Link order: libjq.a (which defines main and pulls in jq/Oniguruma/decNumber)
# after crt0, libc.a last because jq calls memcpy/malloc/str*/math and the
# file/time entry points out of libc.
$(TARGET): $(CRT0) $(LIBC) $(LIBJQ)
	$(LD) $(MPORTS_LDFLAGS) -o $@ $(CRT0) $(LIBJQ) $(LIBC)
	@echo "Built $(TARGET)"

clean:
	rm -f $(TARGET)

.PHONY: all clean
