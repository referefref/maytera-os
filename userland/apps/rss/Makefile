# Makefile for rss - MayteraOS RSS/Atom/RDF feed reader.
# The feed PARSER is Rust (rss_rs.rs -> librss_rs.a), linked into the C app ELF
# using the same pinned-rustc userland recipe as Maytera Arena (#491). See
# apps/arena/ARENA_BSP_PLAN.md for the full rationale of the flags below.

CC = gcc
LD = ld
LIBC_DIR = ../../libc

CFLAGS = -ffreestanding -fstack-protector-strong -mstack-protector-guard=global -fPIE -mno-red-zone \
         -nostdlib -nostdinc -fno-builtin -Wall -Wextra -O2 -g \
         -I. -I$(LIBC_DIR) -isystem /usr/lib/gcc/x86_64-linux-gnu/12/include

LDFLAGS = -pie --no-dynamic-linker -z notext -nostdlib -z max-page-size=0x1000 -T ../../user-pie.ld

TARGET = rss
OBJS = main.o
CRT0 = $(LIBC_DIR)/crt0.o
LIBC = $(LIBC_DIR)/libc.a

# --- USERLAND Rust feed parser --------------------------------------------
# Toolchain PINNED via rust-toolchain.toml (channel 1.97.0, target
# x86_64-unknown-none). The version gate below FAILS the build on drift.
#
# CORRECTED 2026-08-16 (#153, found by grepping the fleet for the exact
# pattern ticket #89 fixed in arena/Makefile, after that same pattern turned
# out to have been left unfixed in taskmanager/Makefile too - see blame.md).
# The comment this replaced ("matches the C app ABI: -mcmodel=large,
# -fno-pic, linked at 0x80000000") was STALE: CFLAGS above is -fPIE and
# LDFLAGS is -pie, not -fno-pic/static, so that rationale had already stopped
# being true (the #640 PIE fleet conversion) without this file being updated.
# -C code-model=large + -C relocation-model=pic together are actively BROKEN
# on this toolchain: rustc's large-model+PIC codegen calls every extern
# function - malloc/realloc included, and this file calls both directly for
# its GlobalAlloc impl, plus every Vec<u8>/Vec<(Vec<u8>,Vec<u8>)> growth the
# parser does - through a per-callsite computed GOT-base register that this
# ld resolves to a WRONG delta, jumping into garbage. Same mechanism as #89
# (arena) and #153 (taskmgr, where it was confirmed by disassembly: the
# broken movabs-landmark call sequence became an ordinary
# `call *offset(%rip)` once code-model=large was dropped). RSS's parser
# allocates via Vec on essentially every parsed tag, so this was not a
# latent risk - it means Feeds page-faulted on the first real feed it tried
# to parse. Default (small) code model + relocation-model=pic gives ordinary
# GOT-relative calls instead, which the loader's R_X86_64_RELATIVE fixups
# resolve correctly; this crate is nowhere near the small model's +-2GB
# reach limit.
RUSTC            = rustc
RUST_PIN_VERSION = 1.97.0
RUST_SRC         = rss_rs.rs
RUST_LIB         = librss_rs.a
RUSTFLAGS = --edition 2021 --crate-type staticlib --target x86_64-unknown-none \
            -C opt-level=2 -C panic=abort \
            -C relocation-model=pic

all: $(TARGET)

$(RUST_LIB): $(RUST_SRC) rust-toolchain.toml
	@echo "Compiling userland Rust feed parser $(RUST_LIB) (pinned rustc $(RUST_PIN_VERSION))..."
	@ver=`$(RUSTC) --version | awk '{print $$2}'`; \
	 if [ "$$ver" != "$(RUST_PIN_VERSION)" ]; then \
	   echo "ERROR: active rustc $$ver != pinned $(RUST_PIN_VERSION) (reproducibility gate)."; \
	   exit 1; \
	 fi
	$(RUSTC) $(RUSTFLAGS) -o $@ $<

# librss_rs.a goes AFTER the C objects (which reference rss_parse/rss_free) and
# BEFORE libc, so ld pulls the Rust members on demand and resolves their
# malloc/free/realloc/memcpy references against libc (last).
$(TARGET): $(OBJS) $(CRT0) $(RUST_LIB) $(LIBC)
	$(LD) $(LDFLAGS) -o $@ $(CRT0) $(OBJS) $(RUST_LIB) $(LIBC)
	@echo "Built $(TARGET)"
	@ls -l $(TARGET)

%.o: %.c rss_rs.h
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(RUST_LIB) $(TARGET)

install: $(TARGET)
	mkdir -p ../../../apps
	cp $(TARGET) ../../../apps/

.PHONY: all clean install
