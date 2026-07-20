# Makefile for taskmanager - MayteraOS Task Manager (#487/#349). RUST.
#
# The app is now Rust (main.rs). The previous 152-line C implementation is kept
# as main.c for rollback ONLY; `make c` rebuilds it if the Rust ever has to be
# backed out. Nothing in the default path compiles main.c.
#
# TARGET NOTES (each verified against `rustc --print target-spec-json`, not
# assumed; getting any of these wrong produces an image that links and then
# misbehaves):
#   --target x86_64-unknown-none is ALREADY soft-float (+soft-float,-sse,-sse2).
#     That is REQUIRED, not incidental: this kernel never saves FPU state across
#     a context switch (sse_save/sse_restore have zero callers), so every
#     computation in the app is fixed-point integer.
#   The same target ALSO defaults to:
#     code-model=kernel -> WRONG for a Ring-3 image linked at 0x80000000 by
#                          user.ld. The C here uses -mcmodel=large; the Rust
#                          matches with -C code-model=large.
#     PIE               -> WRONG; user apps are -fno-pic and statically linked.
#                          Overridden with -C relocation-model=static.

CC = gcc
LD = ld
RUSTC = rustc
RUST_PIN = 1.97.0

LIBC_DIR = ../../libc

CFLAGS = -ffreestanding -mcmodel=large -fno-stack-protector -fno-pic -mno-red-zone -nostdlib -nostdinc -fno-builtin -Wall -Wextra -O2 -g -I$(LIBC_DIR) -isystem /usr/lib/gcc/x86_64-linux-gnu/12/include

LDFLAGS = -nostdlib -z max-page-size=0x1000 -T ../../user.ld

RUSTFLAGS = --edition 2021 --crate-type staticlib --target x86_64-unknown-none \
            -C code-model=large -C relocation-model=static \
            -C panic=abort -C opt-level=2

TARGET = taskmanager
RLIB   = librusttaskmgr.a

CRT0 = $(LIBC_DIR)/crt0.o
LIBC = $(LIBC_DIR)/libc.a

all: $(TARGET)

$(RLIB): main.rs
	@ver=`$(RUSTC) --version | awk '{print $$2}'`; \
	 if [ "$$ver" != "$(RUST_PIN)" ]; then \
	   echo "ERROR: active rustc $$ver != pinned $(RUST_PIN) (reproducibility gate)."; \
	   exit 1; \
	 fi
	$(RUSTC) $(RUSTFLAGS) -o $@ main.rs

$(TARGET): $(RLIB) $(CRT0) $(LIBC)
	$(LD) $(LDFLAGS) -o $@ $(CRT0) $(RLIB) $(LIBC)
	@echo "Built $(TARGET) (Rust)"
	@ls -l $(TARGET)

# Rollback path only: rebuild the pre-#487 C app. Not part of `all`.
c: main.o $(CRT0) $(LIBC)
	$(LD) $(LDFLAGS) -o $(TARGET) $(CRT0) main.o $(LIBC)
	@echo "Built $(TARGET) (C rollback)"

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f main.o $(RLIB) $(TARGET)

.PHONY: all c clean
