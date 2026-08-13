# zlibtest - the running proof for the mports zlib port (local queue 91).
#
# This is the FIRST app Makefile in the tree that gets its flags from
# userland/ports/ports.mk instead of pasting them (see that file for why).

# GNU make takes its default goal from the first explicit target it sees. An
# `include` is expanded IN PLACE, so a target inside ports.mk would become the
# default goal and a bare `make` would build the wrong thing and exit 0 - which
# is exactly how golden build 937 shipped an app that was never linked
# (blame.md, "the default-goal trap"). Pin it before the include, not after.
.DEFAULT_GOAL := all

CC = gcc
LD = ld

USERLAND_DIR = ../..
LIBC_DIR = ../../libc
PORTS_DIR = ../../ports
BUILD_DIR = build

include $(PORTS_DIR)/ports.mk

TARGET = ZLIBTEST
OBJECTS = $(BUILD_DIR)/main.o
CRT0 = $(LIBC_DIR)/crt0.o
LIBC = $(LIBC_DIR)/libc.a
LIBZ = $(MPORTS_LIBDIR)/libz.a

all: $(TARGET)

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

# The port is built by userland/ports/mports.sh, which build-golden.sh runs
# before any app. Building this app standalone in a fresh tree therefore has to
# be able to produce it, or `make` here would fail with a confusing "no such
# file" on a path nobody has heard of. This rule is LOUD: it says what it is
# doing and it fails if mports fails. It is not a fallback that silently
# substitutes something else.
$(LIBZ):
	@echo "zlibtest: libz.a not built yet; running the ports driver"
	bash $(PORTS_DIR)/mports.sh build zlib

# -MMD -MP: a header-only change must recompile. A Makefile without header
# dependency tracking silently re-links a stale object and the fix "does not
# take" (blame.md, #600 and #568).
$(BUILD_DIR)/main.o: main.c | $(BUILD_DIR) $(LIBZ)
	$(CC) $(MPORTS_CFLAGS) $(MPORTS_INCLUDE) -MMD -MP -c $< -o $@

# Link order matters: libz.a must come after main.o, and libc.a last, because
# libz calls memcpy/memset/malloc out of libc.
$(TARGET): $(OBJECTS) $(CRT0) $(LIBC) $(LIBZ)
	$(LD) $(MPORTS_LDFLAGS) -o $@ $(CRT0) $(OBJECTS) $(LIBZ) $(LIBC)
	@echo "Built $(TARGET)"

clean:
	rm -rf $(BUILD_DIR) $(TARGET)

-include $(BUILD_DIR)/main.d

.PHONY: all clean
