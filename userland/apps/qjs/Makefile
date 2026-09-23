# userland/apps/qjs - the QuickJS-ng CLI, installed as /APPS/QJS (#745, Tier 2
# item #8). Unlike the jq app (whose main() is archived in the port), qjs has
# its own small driver, main.c, which links against libquickjs.a built by the
# mports recipe at userland/ports/quickjs. See main.c for why we do not use
# upstream's qjs.c (it needs generated gen/repl.c / gen/standalone.c).
#
# GNU make takes its default goal from the first explicit target it sees, and an
# `include` is expanded IN PLACE. Pin it before the include or a target inside
# ports.mk (its trailing print-% rule) becomes the default goal and a bare
# `make` builds the wrong thing and exits 0 (blame.md, the default-goal trap).
.DEFAULT_GOAL := all

CC = gcc
LD = ld

USERLAND_DIR = ../..
LIBC_DIR = ../../libc
PORTS_DIR = ../../ports

include $(PORTS_DIR)/ports.mk

TARGET = QJS
CRT0 = $(LIBC_DIR)/crt0.o
LIBC = $(LIBC_DIR)/libc.a
LIBQJS = $(MPORTS_LIBDIR)/libquickjs.a

all: $(TARGET)

# The port is built by userland/ports/mports.sh, which build-golden.sh runs
# before any app. Building this app standalone in a fresh tree must still be able
# to produce it, so this is LOUD and fails if mports fails: not a fallback that
# quietly substitutes something else.
$(LIBQJS):
	@echo "qjs: libquickjs.a not built yet; running the ports driver"
	bash $(PORTS_DIR)/mports.sh build quickjs

main.o: main.c $(LIBQJS)
	$(CC) $(MPORTS_CFLAGS) $(MPORTS_INCLUDE) -c main.c -o main.o

# Link order: our object, then libquickjs.a (which calls memcpy/malloc/str*/math
# out of libc), then libc.a last.
$(TARGET): $(CRT0) main.o $(LIBQJS) $(LIBC)
	$(LD) $(MPORTS_LDFLAGS) -o $@ $(CRT0) main.o $(LIBQJS) $(LIBC)
	@echo "Built $(TARGET)"

clean:
	rm -f $(TARGET) main.o

.PHONY: all clean
