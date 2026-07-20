# Makefile for rss - MayteraOS RSS/Atom/RDF feed reader.
# The feed PARSER is Rust (rss_rs.rs -> librss_rs.a), linked into the C app ELF
# using the same pinned-rustc userland recipe as Maytera Arena (#491). See
# apps/arena/ARENA_BSP_PLAN.md for the full rationale of the flags below.

CC = gcc
LD = ld
LIBC_DIR = ../../libc

CFLAGS = -ffreestanding -fno-stack-protector -fno-pic -mno-red-zone -mcmodel=large \
         -nostdlib -nostdinc -fno-builtin -Wall -Wextra -O2 -g \
         -I. -I$(LIBC_DIR) -isystem /usr/lib/gcc/x86_64-linux-gnu/12/include

LDFLAGS = -nostdlib -z max-page-size=0x1000 -T ../../user.ld

TARGET = rss
OBJS = main.o
CRT0 = $(LIBC_DIR)/crt0.o
LIBC = $(LIBC_DIR)/libc.a

# --- USERLAND Rust feed parser --------------------------------------------
# Toolchain PINNED via rust-toolchain.toml (channel 1.97.0, target
# x86_64-unknown-none). The version gate below FAILS the build on drift.
# Code/relocation model overrides match the C app ABI (-mcmodel=large,
# -fno-pic, linked at 0x80000000); default small model overflows R_X86_64_32S.
RUSTC            = rustc
RUST_PIN_VERSION = 1.97.0
RUST_SRC         = rss_rs.rs
RUST_LIB         = librss_rs.a
RUSTFLAGS = --edition 2021 --crate-type staticlib --target x86_64-unknown-none \
            -C opt-level=2 -C panic=abort \
            -C code-model=large -C relocation-model=static

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
