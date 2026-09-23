# adventure - the modern open-source Colossal Cave Adventure
# (gitlab.com/esr/open-adventure), as /APPS/ADVENTURE. See
# userland/ports/openadv/PORT: links libedit (readline/add_history) which
# transitively needs musl-regex + ncurses.

.DEFAULT_GOAL := all

CC = gcc
LD = ld

USERLAND_DIR = ../..
LIBC_DIR = ../../libc
PORTS_DIR = ../../ports

include $(PORTS_DIR)/ports.mk

TARGET = ADVENTURE
CRT0 = $(LIBC_DIR)/crt0.o
LIBC = $(LIBC_DIR)/libc.a
LIBOPENADV = $(MPORTS_LIBDIR)/libopenadv.a
LIBEDIT = $(MPORTS_LIBDIR)/libedit.a
LIBNCURSES = $(MPORTS_LIBDIR)/libncurses.a
LIBREGEX = $(MPORTS_LIBDIR)/libregex.a

all: $(TARGET)

$(LIBOPENADV) $(LIBEDIT) $(LIBNCURSES) $(LIBREGEX):
	@echo "adventure: mports library not built yet; running the ports driver"
	bash $(PORTS_DIR)/mports.sh build openadv

# Link order: libopenadv.a (calls into libedit), libedit.a (calls into
# ncurses + regex), libncurses.a, libregex.a, libc.a last.
$(TARGET): $(CRT0) $(LIBC) $(LIBOPENADV) $(LIBEDIT) $(LIBNCURSES) $(LIBREGEX)
	$(LD) $(MPORTS_LDFLAGS) -o $@ $(CRT0) $(LIBOPENADV) $(LIBEDIT) $(LIBNCURSES) $(LIBREGEX) $(LIBC)
	@echo "Built $(TARGET)"

clean:
	rm -f $(TARGET)

.PHONY: all clean
