# Makefile for browser - MayteraOS Web Browser (NetSurf render pipeline)

CC = gcc
LD = ld

# Paths
LIBC_DIR = ../../libc
LIB_DIR = ../../lib
NS = /opt/maytera/netsurf-port
DUK = /opt/maytera/duktape

# NetSurf core-lib include dirs + freestanding shims
NS_INC = -I$(NS) \
         -I$(NS)/libdom/include \
         -I$(NS)/libcss/include \
         -I$(NS)/libhubbub/include \
         -I$(NS)/libwapcaplet/include \
         -I$(NS)/libparserutils/include \
         -I$(NS)/shim-include

# NetSurf static archives (order does not matter with --start-group)
NS_LIBS = $(NS)/libwapcaplet.a $(NS)/libparserutils.a $(NS)/libhubbub.a \
          $(NS)/libcss.a $(NS)/libdom.a

# Duktape JS engine objects. Built by $(DUK)/build.sh, which encodes two
# non-obvious requirements (duktape/shim must precede the NetSurf shim on the
# include path, and the platform macros must be undefined). Read the comments in
# that script before changing any of this. These were prebuilt binaries with no
# build rule until task #520; all three now reproduce byte-identically from
# source, so the browser is buildable from a clean tree again.
DUK_OBJS = $(DUK)/duktape.o $(DUK)/duk_support.o $(DUK)/duk_dom.o

# Compiler flags. -mcmodel=large matches the .o in the NetSurf archives (apps
# load at 0x80000000 where the small model overflows 32-bit relocations).
CFLAGS = -ffreestanding -fno-stack-protector -fno-pic -mno-red-zone -mcmodel=large \
         -nostdlib -nostdinc -fno-builtin -Wall -Wextra -O2 -g \
         -I$(LIBC_DIR) -I$(DUK) $(NS_INC) -isystem /usr/lib/gcc/x86_64-linux-gnu/12/include

# --no-relax: required so small/large-model GOTPCREL relocs resolve at 2GB.
LDFLAGS = -nostdlib -z max-page-size=0x1000 --no-relax -T ../../user.ld

TARGET = browser

# Local objects: app + NetSurf bindings + our layout engine
OBJS = main.o $(NS)/dom_hubbub_bind.o $(NS)/css_select_bind.o $(NS)/layout.o

CRT0 = $(LIBC_DIR)/crt0.o
LIBC = $(LIBC_DIR)/libc.a

all: $(TARGET)

$(TARGET): main.o $(NS)/dom_hubbub_bind.o $(NS)/css_select_bind.o $(NS)/layout.o $(DUK_OBJS) $(CRT0) $(LIBC)
	$(LD) $(LDFLAGS) -o $@ $(CRT0) main.o $(NS)/dom_hubbub_bind.o \
	    $(NS)/css_select_bind.o $(NS)/layout.o \
	    --start-group $(NS_LIBS) $(DUK_OBJS) $(LIBC) --end-group
	@echo "Built $(TARGET)"
	@ls -lh $(TARGET)

main.o: main.c
	$(CC) $(CFLAGS) -c main.c -o main.o

# Regenerate the Duktape objects from source. cc.sh is deliberately NOT used
# here: it hardcodes -I$(SHIM) -I$(LIBC) ahead of its own "$$@", so duktape/shim
# could never take precedence over the NetSurf shim's competing time.h.
$(DUK)/duktape.o $(DUK)/duk_support.o $(DUK)/duk_dom.o: $(DUK)/build.sh $(DUK)/duktape.c $(DUK)/duk_support.c $(DUK)/duk_dom.c
	cd $(DUK) && ./build.sh

# Build the NetSurf binding/layout objects via the project cc.sh wrapper so they
# get the exact same freestanding flags as the archives.
$(NS)/dom_hubbub_bind.o: $(NS)/dom_hubbub_bind.c
	cd $(NS) && ./cc.sh $(NS_INC) -c dom_hubbub_bind.c -o dom_hubbub_bind.o
$(NS)/css_select_bind.o: $(NS)/css_select_bind.c
	cd $(NS) && ./cc.sh $(NS_INC) -c css_select_bind.c -o css_select_bind.o
$(NS)/layout.o: $(NS)/layout.c
	cd $(NS) && ./cc.sh $(NS_INC) -c layout.c -o layout.o

clean:
	rm -f main.o $(TARGET)

install: $(TARGET)
	mkdir -p ../../../apps
	cp $(TARGET) ../../../apps/BROWSER.ELF
	@echo "Installed to ../../../apps/BROWSER.ELF"

.PHONY: all clean install
