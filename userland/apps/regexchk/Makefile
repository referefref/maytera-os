# regexchk - the running proof for the mports musl-regex port (#745 local 97).
#
# The fourth app in the tree to get its flags from userland/ports/ports.mk
# instead of pasting them (zlibtest, pcre2check and luacheck came first).

# GNU make takes its default goal from the first explicit target it sees, and an
# `include` is expanded IN PLACE, so a target inside ports.mk would become the
# default goal and a bare `make` would build the wrong thing and exit 0. That is
# how golden build 937 shipped an app that was never linked. Pin it BEFORE the
# include.
.DEFAULT_GOAL := all

CC = gcc
LD = ld

USERLAND_DIR = ../..
LIBC_DIR = ../../libc
PORTS_DIR = ../../ports
BUILD_DIR = build

include $(PORTS_DIR)/ports.mk

TARGET = REGEXCHK
OBJECTS = $(BUILD_DIR)/main.o
CRT0 = $(LIBC_DIR)/crt0.o
LIBC = $(LIBC_DIR)/libc.a
LIBREGEX = $(MPORTS_LIBDIR)/libregex.a

all: $(TARGET)

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

# The port is built by userland/ports/mports.sh, which build-golden.sh runs
# before any app. Building this app standalone in a fresh tree therefore has to
# be able to produce it. This rule is LOUD and it fails if mports fails; it is
# not a fallback that silently substitutes something else.
$(LIBREGEX):
	@echo "regexchk: libregex.a not built yet; running the ports driver"
	bash $(PORTS_DIR)/mports.sh build musl-regex

# -MMD -MP: a header-only change must recompile, or a stale object silently
# re-links and the fix "does not take" (blame.md, #600 and #568).
$(BUILD_DIR)/main.o: main.c | $(BUILD_DIR) $(LIBREGEX)
	$(CC) $(MPORTS_CFLAGS) $(MPORTS_INCLUDE) -MMD -MP -c $< -o $@

# Link order matters: libregex.a after main.o and libc.a last, because the
# engine calls malloc/memcpy and the ctype predicates out of libc.
$(TARGET): $(OBJECTS) $(CRT0) $(LIBC) $(LIBREGEX)
	$(LD) $(MPORTS_LDFLAGS) -o $@ $(CRT0) $(OBJECTS) $(LIBREGEX) $(LIBC)
	@echo "Built $(TARGET)"

clean:
	rm -rf $(BUILD_DIR) $(TARGET)

-include $(BUILD_DIR)/main.d

.PHONY: all clean
