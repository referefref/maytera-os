# Makefile for terminal - MayteraOS user-space application
#
# GNU make takes its default goal from the first explicit target it sees and an
# `include` is expanded IN PLACE, so ports.mk's own print-% target would become
# the default goal and a bare `make` would build the wrong thing and exit 0.
# That is how golden build 937 shipped an app that was never linked (see
# userland/apps/sed/Makefile). Pin it BEFORE the include.
.DEFAULT_GOAL := all

CC = gcc
LD = ld

# Paths
LIBC_DIR = ../../libc
USERLAND_DIR = ../..
PORTS_DIR = ../../ports

# #221: find-in-scrollback's regex mode links userland/ports/musl-regex's
# libregex.a, the SAME MIT POSIX engine /APPS/GREP, /APPS/VI and /APPS/SED
# already link. #87 records that the vendored busybox vi shipped a PRIVATE
# LGPL-3.0 GNU regex copy and that this port exists to retire exactly that, so
# a fourth private engine here would re-open a licence exposure the tree has
# already closed.
include $(PORTS_DIR)/ports.mk

# Compiler flags
CFLAGS = -ffreestanding -fstack-protector-strong -mstack-protector-guard=global -fPIE -mno-red-zone          -nostdlib -nostdinc -fno-builtin          -Wall -Wextra -O2 -g          -I$(LIBC_DIR) -isystem /usr/lib/gcc/x86_64-linux-gnu/12/include

# Linker flags
LDFLAGS = -pie --no-dynamic-linker -z notext -nostdlib -z max-page-size=0x1000 -T $(USERLAND_DIR)/user-pie.ld

# Output
TARGET = terminal

# Objects
OBJS = main.o term_util.o term_grid.o term_scrollback.o term_parse.o \
       term_render.o term_theme.o term_prefs.o term_profile.o term_pty.o \
       term_shell.o term_notify.o term_layout.o term_emu.o term_select.o \
       term_search.o term_menu.o

# Libraries
CRT0 = $(LIBC_DIR)/crt0.o
LIBC = $(LIBC_DIR)/libc.a
LIBREGEX = $(MPORTS_LIBDIR)/libregex.a

# Default target
all: $(TARGET)

# The port is built by userland/ports/mports.sh, which build-golden.sh runs
# before any app. Building this app standalone in a fresh tree therefore has to
# be able to produce it. This rule is LOUD and it fails if mports fails; it is
# not a fallback that silently substitutes something else.
$(LIBREGEX):
	@echo "terminal: libregex.a not built yet; running the ports driver"
	bash $(PORTS_DIR)/mports.sh build musl-regex

# Link the executable. libregex.a AFTER the objects and libc.a LAST, because
# the engine calls malloc/memcpy and the ctype predicates out of libc.
$(TARGET): $(OBJS) $(CRT0) $(LIBC) $(LIBREGEX)
	$(LD) $(LDFLAGS) -o $@ $(CRT0) $(OBJS) $(LIBREGEX) $(LIBC)
	@echo "Built $(TARGET)"
	@ls -l $(TARGET)

# Compile C source. -MMD -MP: a header-only change must recompile, or a stale
# object silently re-links and the fix "does not take" (blame.md, #600/#568).
%.o: %.c | $(LIBREGEX)
	$(CC) $(CFLAGS) $(MPORTS_INCLUDE) -MMD -MP -c $< -o $@

# Clean
clean:
	rm -f $(OBJS) $(OBJS:.o=.d) $(TARGET)

-include $(OBJS:.o=.d)

.PHONY: all clean
