# tui2048 - the classic single-file terminal 2048 (mevdschee/2048.c), as
# /APPS/TUI2048. Named tui2048 rather than plain "2048" because a leading
# digit is an awkward app/target identifier across this tree's tooling
# (Makefile TARGET, golden-new-apps.list, Start-menu item id); the upstream
# project itself has no reserved name to collide with.
#
# No ncurses dependency at all (see userland/ports/2048/PORT): this is the
# only game in the tuigames batch that links libc alone.

.DEFAULT_GOAL := all

CC = gcc
LD = ld

USERLAND_DIR = ../..
LIBC_DIR = ../../libc
PORTS_DIR = ../../ports

include $(PORTS_DIR)/ports.mk

TARGET = TUI2048
CRT0 = $(LIBC_DIR)/crt0.o
LIBC = $(LIBC_DIR)/libc.a
LIB2048 = $(MPORTS_LIBDIR)/lib2048.a

all: $(TARGET)

$(LIB2048):
	@echo "tui2048: mports library not built yet; running the ports driver"
	bash $(PORTS_DIR)/mports.sh build 2048

$(TARGET): $(CRT0) $(LIBC) $(LIB2048)
	$(LD) $(MPORTS_LDFLAGS) -o $@ $(CRT0) $(LIB2048) $(LIBC)
	@echo "Built $(TARGET)"

clean:
	rm -f $(TARGET)

.PHONY: all clean
