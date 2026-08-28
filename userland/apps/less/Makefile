# Makefile for less (#745 local 108, second batch).
#
# less links userland/ports/musl-regex's libregex.a: the SAME MIT POSIX engine
# /APPS/GREP, /APPS/SED and /APPS/VI link, built once by the mports driver.
# A fourth private regex would be exactly the second-copy defect the ports
# mechanism exists to end (docs/MPORTS.md, owner rule 1).
#
# GNU make takes its default goal from the first explicit target it sees, and an
# `include` is expanded IN PLACE, so ports.mk's own print-% target would become
# the default goal and a bare `make` would build the wrong thing and exit 0.
# That is how golden build 937 shipped an app that was never linked. Pin it
# BEFORE the include.
.DEFAULT_GOAL := all

CC = gcc
LD = ld

USERLAND_DIR = ../..
LIBC_DIR = ../../libc
PORTS_DIR = ../../ports

include $(PORTS_DIR)/ports.mk

CRT0 = $(LIBC_DIR)/crt0.o
LIBC = $(LIBC_DIR)/libc.a
LIBREGEX = $(MPORTS_LIBDIR)/libregex.a

# less.elf, not less: build-golden.sh resolves the /APPS entry from the binary
# basename, and the image entry has always been /APPS/LESS from this name.
TARGET = less.elf
SRCS = main.c
OBJS = $(SRCS:.c=.o)

all: $(TARGET)

# The port is built by userland/ports/mports.sh, which build-golden.sh runs
# before any app. Building this app standalone in a fresh tree therefore has to
# be able to produce it. This rule is LOUD and it fails if mports fails; it is
# not a fallback that silently substitutes something else.
$(LIBREGEX):
	@echo "less: libregex.a not built yet; running the ports driver"
	bash $(PORTS_DIR)/mports.sh build musl-regex

# Link order: libregex.a after main.o and libc.a last, because the engine calls
# malloc/memcpy and the ctype predicates out of libc.
$(TARGET): $(OBJS) $(CRT0) $(LIBC) $(LIBREGEX)
	$(LD) $(MPORTS_LDFLAGS) -o $@ $(CRT0) $(OBJS) $(LIBREGEX) $(LIBC)

# -MMD -MP: a header-only change must recompile, or a stale object silently
# re-links and the fix "does not take" (blame.md, #600 and #568).
%.o: %.c | $(LIBREGEX)
	$(CC) $(MPORTS_CFLAGS) $(MPORTS_INCLUDE) -MMD -MP -c $< -o $@

clean:
	rm -f *.o *.d $(TARGET)

-include $(SRCS:.c=.d)

.PHONY: all clean
