# lua - the real upstream Lua 5.4 interpreter, as /APPS/LUA.
#
# THIS APP HAS NO SOURCE OF ITS OWN, AND THAT IS THE POINT. Upstream's
# src/lua.c is the interpreter's main(); userland/ports/lua/PORT excludes it
# from liblua.a (a library cannot contain a main()) and installs it with
# `install_sources=src/lua.c`, so it lands at $(MPORTS_SRCDIR)/lua.c after the
# port's patch series has run. Vendoring a copy of lua.c into this directory
# would put upstream source back in the repository, which is exactly what the
# mports mechanism exists to avoid, and it would need its own licence coverage.
#
# So: one recipe produces BOTH a linkable liblua.a and a usable /APPS/LUA, and
# the version number appears in exactly one place (userland/ports/lua/PORT).

# GNU make takes its default goal from the first explicit target it sees, and an
# `include` is expanded IN PLACE. Pin it before the include (blame.md, the
# default-goal trap that shipped an unlinked app in golden 937).
.DEFAULT_GOAL := all

CC = gcc
LD = ld

USERLAND_DIR = ../..
LIBC_DIR = ../../libc
PORTS_DIR = ../../ports
BUILD_DIR = build

include $(PORTS_DIR)/ports.mk

TARGET = LUA
LUA_MAIN = $(MPORTS_SRCDIR)/lua.c
OBJECTS = $(BUILD_DIR)/lua.o
CRT0 = $(LIBC_DIR)/crt0.o
LIBC = $(LIBC_DIR)/libc.a
LIBLUA = $(MPORTS_LIBDIR)/liblua.a

all: $(TARGET)

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

# Both the archive and the interpreter main come out of the same port build, so
# one rule produces both. LOUD, and it fails if mports fails: it is not a
# fallback that quietly substitutes something else.
$(LIBLUA) $(LUA_MAIN):
	@echo "lua: the Lua port is not built yet; running the ports driver"
	bash $(PORTS_DIR)/mports.sh build lua

# -Wno-unused-parameter ONLY: upstream's lua.c is compiled unmodified, and its
# signal handler and the standard main(argc, argv) shape trip -Wextra on this
# tree's stricter profile. Every other warning stays on. It is spelled out here
# rather than hidden in the port's cflags because it applies to THIS
# translation unit, not to the library.
$(BUILD_DIR)/lua.o: $(LUA_MAIN) | $(BUILD_DIR)
	$(CC) $(MPORTS_CFLAGS) $(MPORTS_INCLUDE) -Wno-unused-parameter -MMD -MP -c $< -o $@

# Link order matters: liblua.a after lua.o, libc.a last.
$(TARGET): $(OBJECTS) $(CRT0) $(LIBC) $(LIBLUA)
	$(LD) $(MPORTS_LDFLAGS) -o $@ $(CRT0) $(OBJECTS) $(LIBLUA) $(LIBC)
	@echo "Built $(TARGET)"

clean:
	rm -rf $(BUILD_DIR) $(TARGET)

-include $(BUILD_DIR)/lua.d

.PHONY: all clean
