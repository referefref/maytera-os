# Makefile - DOSRING3: the MayteraOS DOS interpreter, hosted in Ring 3.
#
# WHAT THIS BUILDS AND WHY IT IS NOT A PORT.
# The kernel's DOS subsystem is ~40k lines across kernel/dos/*.c,
# kernel/exec/x86_16.c and ~20k lines of kernel/rustkern/dos*.rs. This app
# compiles THOSE EXACT SOURCES, unmodified, into a Ring-3 ELF. mkgen.sh
# assembles a build tree in gen/ whose sibling directories are ours, which is
# the only way to substitute a Ring-3 implementation behind the kernel sources'
# quoted relative includes. The result is ONE implementation of DOS semantics in
# the tree, shared by the Ring-0 and Ring-3 paths, which is what stops the two
# from drifting into guest-visible disagreement.
#
# MEASURED: all 16 DOS/exec C sources compile for Ring 3 with exactly ONE
# shimmed header (serial.h); the Rust half needs only a different crate root.
CC    = gcc
LD    = ld
RUSTC = rustc
RUST_PIN_VERSION = 1.97.0

LIBC_DIR = ../../libc
GEN      = gen
BUILD    = build
GCCINC  := $(shell ls -d /usr/lib/gcc/x86_64-linux-gnu/*/include | head -1)

# -w: the kernel sources are built here under a DIFFERENT warning regime than
# the kernel's -Wall -Wextra -Werror. They already pass that regime in the
# kernel build; re-litigating it here would only produce noise from the flag
# differences (code model, SSE), not real defects.
CFLAGS = -fPIE -ffreestanding -fstack-protector-strong \
         -mstack-protector-guard=global -mno-red-zone -msse -msse2 \
         -nostdlib -nostdinc -fno-builtin -O2 -g -w \
         -I$(GEN) -isystem $(GCCINC)
# The shim's userland half speaks libc, so it gets the libc include path and
# NOT the kernel one. The two type universes are bridged by primitive-typed
# functions only (see shim/kbridge.h) so they never meet in one TU.
UCFLAGS = -fPIE -ffreestanding -fstack-protector-strong \
          -mstack-protector-guard=global -mno-red-zone -msse -msse2 \
          -nostdlib -nostdinc -fno-builtin -O2 -g -Wall \
          -I$(LIBC_DIR) -isystem $(GCCINC)

RUSTFLAGS = --edition 2021 --crate-type staticlib --target x86_64-unknown-none \
            -C panic=abort -C opt-level=2

LDFLAGS = -pie --no-dynamic-linker -z notext -nostdlib -z max-page-size=0x1000 \
          -T ../../user-pie.ld
# THE INSTALL NAME IS DERIVED FROM THIS, so it must be the name the LAUNCHER
# uses, not a filename with an extension. kernel/proc/dosring3.c launches
# "/APPS/DOSUSER"; build-golden.sh resolves the /APPS entry from this binary's
# basename. Naming it DOSUSER.ELF would ship /APPS/DOSUSER.ELF and the gate
# would launch nothing - the exact shape of the bug where the freshly-built
# compositor landed on /APPS/COMPOSITOR while the kernel went on launching a
# stale /APPS/COMPOSIT. build/golden-new-apps.list declares DOSUSER to match.
TARGET  = DOSUSER

KSRC   := $(wildcard $(GEN)/dos/*.c) $(wildcard $(GEN)/exec/*.c) $(GEN)/string.c
KOBJ   := $(patsubst $(GEN)/%.c,$(BUILD)/k_%.o,$(subst /,_,$(KSRC)))
RUSTLIB = $(BUILD)/libdosrust.a
CRT0    = $(LIBC_DIR)/crt0.o
LIBC    = $(LIBC_DIR)/libc.a

.PHONY: all gen clean symbols
all: $(TARGET)

# The generated tree is a build product, never committed, and is rebuilt every
# time so an upstream kernel/dos change is picked up automatically rather than
# needing a merge.
gen:
	@./mkgen.sh

clean:
	rm -rf $(GEN) $(BUILD) $(TARGET)

# symbols: print exactly what the C shim still owes. This is the SPECIFICATION
# of the shim layer, computed rather than hand-maintained, so a symbol can
# never be silently satisfied by something that returns a plausible value.
symbols: gen $(RUSTLIB)
	@mkdir -p $(BUILD)
	@set -e; for f in $(GEN)/dos/*.c $(GEN)/exec/*.c; do \
	   o=$(BUILD)/$$(echo $$f | sed 's|$(GEN)/||; s|/|_|g; s|\.c$$|.o|'); \
	   $(CC) $(CFLAGS) -c $$f -o $$o; done
	@nm -g --defined-only $(BUILD)/*.o $(RUSTLIB) 2>/dev/null \
	   | awk '$$2 ~ /^[TDBRVWSGi]$$/ {print $$3}' | sort -u > $(BUILD)/defined.txt
	@nm -u $(BUILD)/*.o $(RUSTLIB) 2>/dev/null \
	   | sed 's/^ *U //' | grep -vE '^$$|:' | sort -u > $(BUILD)/undefined.txt
	@comm -23 $(BUILD)/undefined.txt $(BUILD)/defined.txt > $(BUILD)/shimspec.txt
	@echo "C shim surface: $$(wc -l < $(BUILD)/shimspec.txt) symbols"
	@cat $(BUILD)/shimspec.txt

SHIM_K   = $(BUILD)/kshim.o $(BUILD)/volshim.o
SHIM_U   = $(BUILD)/ushim.o $(BUILD)/dosmain.o

$(TARGET): gen $(RUSTLIB)
	@mkdir -p $(BUILD)
	@set -e; for f in $(GEN)/dos/*.c $(GEN)/exec/*.c \
	                  $(GEN)/video/font.c $(GEN)/drivers/keymap.c \
	                  $(GEN)/drivers/audio_resample.c; do \
	   o=$(BUILD)/$$(echo $$f | sed 's|$(GEN)/||; s|/|_|g; s|\.c$$|.o|'); \
	   $(CC) $(CFLAGS) -c $$f -o $$o; done
	$(CC) $(CFLAGS) -Ishim -c shim/kshim.c  -o $(BUILD)/kshim.o
	$(CC) $(CFLAGS) -Ishim -c shim/volshim.c -o $(BUILD)/volshim.o
	$(CC) $(UCFLAGS) -Ishim -c shim/ushim.c   -o $(BUILD)/ushim.o
	$(CC) $(UCFLAGS) -Ishim -c shim/dosmain.c -o $(BUILD)/dosmain.o
	$(LD) $(LDFLAGS) -o $@ $(CRT0) $(BUILD)/*.o $(RUSTLIB) $(LIBC)
	@echo "Built $@"; ls -lh $@

$(RUSTLIB): shim/dosrust.rs | gen
	@mkdir -p $(BUILD)
	@ver=`$(RUSTC) --version | awk '{print $$2}'`; \
	 if [ "$$ver" != "$(RUST_PIN_VERSION)" ]; then \
	   echo "ERROR: rustc $$ver != pinned $(RUST_PIN_VERSION)"; exit 1; fi
	$(RUSTC) $(RUSTFLAGS) -o $@ shim/dosrust.rs
