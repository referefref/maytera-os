# Makefile for XaoS - MayteraOS userland port (#745,
# docs/PORTABILITY_HOMEBREW_SNAPCRAFT_ASSESSMENT.md Tier 3 #16).
#
# Built as a userland APP (like userland/apps/doom), not through the
# userland/ports mports system: XaoS's own upstream build already produces
# an application binary, not a linkable library, and the ENTIRE point of
# this port is a small platform driver (src/ui/ui-drv/maytera/ui_maytera.c)
# bridging its `struct ui_driver` abstraction to SYS_WIN_*, the same shape
# as the DOOM platform layer (i_maytera.c/i_video.c). See README.md.
#
# Upstream XaoS 3.6 (release-3.6 tag), GPL-2.0-or-later. Vendored source
# under src/{engine,filter,ui,ui-hlp,util,include}, UNMODIFIED except for
# drivers.c (one add-a-driver hunk, clearly marked) and the exclusion of
# util/png.c (needs libpng/zlib, not needed for the render path) and the
# sffe/ optional formula-parser subtree (SFFE_USING is never defined, so
# XaoS's normal built-in formula table, e.g. the default Mandelbrot z^2+c,
# is what runs; see ATTRIBUTION.md).

CC = gcc
LD = ld

LIBC_DIR = ../../libc
XSRC = src

CFLAGS = -fPIE -ffreestanding -fstack-protector-strong -mstack-protector-guard=global -mno-red-zone -msse -msse2 \
         -nostdlib -nostdinc -fno-builtin -O2 -g \
         -I. -I$(XSRC)/include -I$(LIBC_DIR) -isystem /usr/lib/gcc/x86_64-linux-gnu/12/include \
         -w

LDFLAGS = -pie --no-dynamic-linker -z notext -nostdlib -z max-page-size=0x1000 -T ../../user-pie.ld

TARGET = XAOS

# These five lists are copied VERBATIM from upstream's own
# src/{engine,filter,ui-hlp,util}/Makefile.in SRCS (the real, autoconf-driven
# build), not reconstructed by directory listing: several files in each of
# these directories (docalc.c, 3dd.c, btraced.c, edge2d.c, paletted.c,
# rotated.c, stard.c, stereod.c, zoomd.c, grlibd.c, font2/3/23.c, xfont*.c,
# ...) are #include'd BY their paired .c file (a template-instantiation style
# this 1997-era codebase uses for its per-formula inner loops) and must NOT
# be compiled as their own translation unit, only upstream's own Makefile.in
# says which is which.
ENGINE_SRCS = $(addprefix $(XSRC)/engine/, \
	formulas.c fractal.c btrace.c palettef.c emboss.c star.c anti.c \
	dither.c edge.c edge2.c rotate.c zoom.c blur.c interlace.c \
	itersmall.c stereogram.c 3d.c subwindow.c plane.c julia.c i386.c)

FILTER_SRCS = $(addprefix $(XSRC)/filter/, \
	image.c palette.c random.c grlib.c font.c filter.c)

UI_SRCS = $(addprefix $(XSRC)/ui/, \
	dialog.c drivers.c fparams.c filesel.c param.c pipecmd.c ui.c \
	uihelp.c uimenu.c) \
	$(XSRC)/ui/ui-drv/maytera/ui_maytera.c

UIHLP_SRCS = $(addprefix $(XSRC)/ui-hlp/, \
	autopilot.c ui_helper.c menu.c play.c messg.c render.c playtext.c \
	save.c wstack.c)

# util/png.c EXCLUDED: needs libpng/zlib (see the file header note above).
UTIL_SRCS = $(addprefix $(XSRC)/util/, \
	catalog.c thread.c xstring.c help.c xerror.c xshl.c xldio.c xstdio.c \
	xmenu.c timers.c)

SRCS = xaos_compat.c $(ENGINE_SRCS) $(FILTER_SRCS) $(UI_SRCS) $(UIHLP_SRCS) $(UTIL_SRCS)
OBJS = $(SRCS:.c=.o)

CRT0 = $(LIBC_DIR)/crt0.o
LIBC = $(LIBC_DIR)/libc.a

all: $(TARGET)

$(TARGET): $(OBJS) $(CRT0) $(LIBC)
	$(LD) $(LDFLAGS) -o $@ $(CRT0) $(OBJS) $(LIBC)
	@echo "Built $(TARGET)"
	@ls -lh $(TARGET)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(TARGET)

.PHONY: all clean
