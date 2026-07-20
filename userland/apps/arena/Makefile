# Maytera Arena - TinyGL 3D arena FPS. All module *.c are auto-collected.
CC = gcc
LD = ld
LIBC_DIR  = ../../libc
LIBGL_DIR = ../../libgl

CFLAGS = -ffreestanding -fno-stack-protector -fno-pic -mno-red-zone -msse -msse2 \
         -nostdlib -nostdinc -fno-builtin -O2 -g -mcmodel=large -fno-pie \
         -I. -I$(LIBC_DIR) -I$(LIBGL_DIR)/include \
         -isystem /usr/lib/gcc/x86_64-linux-gnu/12/include

LDFLAGS = -nostdlib -z max-page-size=0x1000 -T ../../user.ld

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
# The builtin x86_64-unknown-none target ships a precompiled core + alloc (no
# build-std needed), but its default code/relocation model does NOT match the
# userland app ABI, so we OVERRIDE to match the C CFLAGS above:
#   -C code-model=large     <-> -mcmodel=large (app is LINKED AT 0x80000000;
#                               small model overflows R_X86_64_32S at 2^31)
#   -C relocation-model=static <-> -fno-pic/-fno-pie (static link)
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
            -C code-model=large -C relocation-model=static

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
