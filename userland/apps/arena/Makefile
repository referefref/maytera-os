# Maytera Arena - TinyGL 3D arena FPS. All module *.c are auto-collected.
CC = gcc
LD = ld
LIBC_DIR  = ../../libc
LIBGL_DIR = ../../libgl

CFLAGS = -fPIE -ffreestanding -fstack-protector-strong -mstack-protector-guard=global -mno-red-zone -msse -msse2 \
         -nostdlib -nostdinc -fno-builtin -O2 -g \
         -I. -I$(LIBC_DIR) -I$(LIBGL_DIR)/include \
         -isystem /usr/lib/gcc/x86_64-linux-gnu/12/include

LDFLAGS = -pie --no-dynamic-linker -z notext -nostdlib -z max-page-size=0x1000 -T ../../user-pie.ld

TARGET = ARENA
# pthread + clone trampoline are missing from the stale shared libc.a, so we
# pull the libc source for them directly into this app (arena_pthread.c +
# arena_clone.asm) and link them here.
SRCS = $(wildcard *.c)
ASMS = $(wildcard *.asm)
OBJS = $(SRCS:.c=.o) $(ASMS:.asm=.o)
CRT0  = $(LIBC_DIR)/crt0.o
LIBC  = $(LIBC_DIR)/libc.a
LIBGL = $(LIBGL_DIR)/libgl.a

# --- USERLAND Rust (#491 Stage 0: Rust in the Arena Ring-3 ELF, NO behavior
# change) ------------------------------------------------------------------
# Toolchain is PINNED for reproducibility via rust-toolchain.toml (channel
# 1.97.0, target x86_64-unknown-none - the SAME rustc the kernel port pins).
# The version gate below FAILS the build if the active rustc drifts, so a
# `rustup update` can never silently change the golden Arena ELF.
#
# ticket 89 (2026-08-13): DO NOT ADD -C code-model=large BACK HERE. The old
# comment claiming "-C code-model=large <-> -mcmodel=large ... -C
# relocation-model=pic <-> -fno-pic/-fno-pie (static link)" was already wrong
# on its face (the CFLAGS above are -fPIE, not -fno-pic/-fno-pie) and, more
# importantly, the large+pic combination it set up PRODUCED A BROKEN BINARY:
# LLVM's large-model-PIC codegen calls external functions (malloc/realloc/
# __rust_no_alloc_shim_is_unstable_v2, ...) through a per-callsite computed
# "GOT base" register (lea rip-landmark; movabs r12,<link-time delta>; add
# r12,landmark; call [r12+<offset>]), driven by R_X86_64_GOTPC64/R_X86_64_GOT64
# relocations that this toolchain's `ld` resolves into WRONG movabs immediates
# for -mcmodel=large objects (measured: off by a constant but wrong delta, so
# the computed "GOT base" landed ~0x770 bytes into unrelated .rodata/texture
# data instead of the real GOT). Every one of those calls jumps into garbage,
# deterministically, on the FIRST Vec growth performed by Rust code, which is
# exactly why /APPS/ARENA page-faulted immediately on launch (Ring-3 #PF, an
# instruction-fetch fault, CS=0x23 i.e. genuinely mid-execution, not a bad
# process bootstrap): `arena_rs_selftest()` (called unconditionally at
# startup) pushes into a Vec<u32>, RawVec grows, and finish_grow's indirect
# call through the miscomputed GOT base lands at whatever raw bytes happen to
# sit at that wrong offset.
#
# The x86_64-unknown-none target's default (small) code model does NOT hit
# this: `-C relocation-model=pic` alone produces ordinary `call [rip+disp32]`
# GOT-relative calls (a single R_X86_64_GOTPCREL-style fixup per call site,
# not a synthesized GOT-base register), and readelf/objdump on a test build
# confirmed the disassembly points at exactly the same GOT slots the linker
# independently populated with R_X86_64_RELATIVE fixups for malloc/realloc/
# __rust_no_alloc_shim_is_unstable_v2. The whole Arena Rust crate (arena_rs.rs
# + bsp.rs) is a few hundred KB, nowhere near the +-2GB small-model reach
# limit, so nothing in THIS crate needs the large model. The large model was
# only ever there to mirror the C side's -mcmodel=large (needed because the
# non-PIE C code used to link at a fixed 0x80000000+ base); now that both C
# and Rust are PIE (#640), that constraint does not apply to either language,
# and this link (`ld -pie ... user-pie.ld`) already succeeds with the C
# objects at -mcmodel=large and the Rust object at the default (small) model
# mixed in the same executable - PIE relocations for both are resolved via
# the same R_X86_64_RELATIVE mechanism our loader (kernel/exec/elf.c) applies
# at load time, which does not care what code model produced them.
#
#   -C relocation-model=pic <-> -fPIE (PIE executable, kernel applies the
#                               R_X86_64_RELATIVE fixups at load time)
#   -C panic=abort          (no unwinding; panic routes to userland abort())
RUSTC            = rustc
RUST_PIN_VERSION = 1.97.0
RUST_SRC         = arena_rs.rs
# #491 Stage 1: bsp.rs is `mod bsp;` inside arena_rs.rs (one crate root), so it
# is not on the rustc command line but IS a rebuild dependency.
RUST_SRCS        = arena_rs.rs bsp.rs
RUST_LIB         = libarena_rs.a
RUSTFLAGS = --edition 2021 --crate-type staticlib --target x86_64-unknown-none \
            -C opt-level=2 -C panic=abort \
            -C relocation-model=pic

all: $(TARGET)

$(RUST_LIB): $(RUST_SRCS) rust-toolchain.toml
	@echo "Compiling userland Rust static lib $(RUST_LIB) (pinned rustc $(RUST_PIN_VERSION))..."
	@ver=`$(RUSTC) --version | awk '{print $$2}'`; \
	 if [ "$$ver" != "$(RUST_PIN_VERSION)" ]; then \
	   echo "ERROR: active rustc $$ver != pinned $(RUST_PIN_VERSION) (reproducibility gate)."; \
	   echo "       Install the pin: rustup toolchain install $(RUST_PIN_VERSION) --profile minimal &&"; \
	   echo "       rustup target add --toolchain $(RUST_PIN_VERSION) x86_64-unknown-none"; \
	   exit 1; \
	 fi
	$(RUSTC) $(RUSTFLAGS) -o $@ $<

# librarena_rs.a is linked AFTER the C objects (which reference arena_rs_selftest)
# and BEFORE libgl/libc, so ld pulls the Rust member on demand and then resolves
# its malloc/free/realloc/memcpy references against libc (last). No -u needed:
# main.c has a real C -> Rust call, so the archive member is pulled naturally.
$(TARGET): $(OBJS) $(CRT0) $(RUST_LIB) $(LIBC) $(LIBGL)
	$(LD) $(LDFLAGS) -o $@ $(CRT0) $(OBJS) $(RUST_LIB) $(LIBGL) $(LIBC)
	@echo "Built $(TARGET)"
	@ls -l $(TARGET)

%.o: %.c game.h mathx.h
	$(CC) $(CFLAGS) -c $< -o $@

%.o: %.asm
	nasm -f elf64 $< -o $@

clean:
	rm -f $(OBJS) $(RUST_LIB) $(TARGET)

.PHONY: all clean
