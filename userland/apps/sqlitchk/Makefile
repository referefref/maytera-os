# sqlitchk - the running proof for the mports SQLite port (Tier 1 item #4).
#
# Flags come from userland/ports/ports.mk, never pasted (see that file for why).

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
BUILD_DIR = build

include $(PORTS_DIR)/ports.mk

TARGET = SQLITCHK
OBJECTS = $(BUILD_DIR)/main.o
CRT0 = $(LIBC_DIR)/crt0.o
LIBC = $(LIBC_DIR)/libc.a
LIBSQLITE = $(MPORTS_LIBDIR)/libsqlite3.a

all: $(TARGET)

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

# The port is built by userland/ports/mports.sh, which build-golden.sh runs
# before any app. Building this app standalone in a fresh tree therefore has to
# be able to produce it. LOUD, and it fails if mports fails.
$(LIBSQLITE):
	@echo "sqlitchk: libsqlite3.a not built yet; running the ports driver"
	bash $(PORTS_DIR)/mports.sh build sqlite

# -MMD -MP: a header change must recompile (blame.md, #600/#568).
$(BUILD_DIR)/main.o: main.c | $(BUILD_DIR) $(LIBSQLITE)
	$(CC) $(MPORTS_CFLAGS) $(MPORTS_INCLUDE) -MMD -MP -c $< -o $@

# Link order: libsqlite3.a after main.o, libc.a last, because SQLite calls
# memcpy/malloc/str* and the file/time entry points out of libc.
$(TARGET): $(OBJECTS) $(CRT0) $(LIBC) $(LIBSQLITE)
	$(LD) $(MPORTS_LDFLAGS) -o $@ $(CRT0) $(OBJECTS) $(LIBSQLITE) $(LIBC)
	@echo "Built $(TARGET)"

clean:
	rm -rf $(BUILD_DIR) $(TARGET)

-include $(BUILD_DIR)/main.d

.PHONY: all clean
