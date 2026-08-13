# Makefile for the first-boot setup wizard (OOBE) - Rust (#745)
#
# Mirrors the taskmgr (#487) recipe exactly, including the pinned-rustc
# reproducibility gate. The C main.c is kept as the rollback path only and is
# NOT part of `all` (strangler pattern).

CC = gcc
LD = ld
RUSTC = rustc
RUST_PIN = 1.97.0

LIBC_DIR = ../../libc

CFLAGS = -ffreestanding -fstack-protector-strong -mstack-protector-guard=global -fPIE -mno-red-zone \
         -nostdlib -nostdinc -fno-builtin -Wall -Wextra -O2 -g \
         -I$(LIBC_DIR) -isystem /usr/lib/gcc/x86_64-linux-gnu/12/include

LDFLAGS = -pie --no-dynamic-linker -z notext -nostdlib -z max-page-size=0x1000 -T ../../user-pie.ld

# -C code-model=large      : x86_64-unknown-none defaults to code-model=kernel
# -C relocation-model=pic  : userland is PIE (user-pie.ld); the launched-app
#                            gate REJECTS a non-PIE binary
# -C panic=abort           : no unwinder in Ring 3
# The target is already soft-float (+soft-float,-sse,-sse2), which is required:
# this kernel never saves FPU state across a context switch.
RUSTFLAGS = --edition 2021 --crate-type staticlib --target x86_64-unknown-none \
            -C relocation-model=pic \
            -C panic=abort -C opt-level=2

TARGET = SETUP
RLIB   = librustsetup.a

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

# Rollback path only: rebuild the pre-Rust C app. Not part of `all`.
c-rollback: main.c $(CRT0) $(LIBC)
	$(CC) $(CFLAGS) -c main.c -o main.o
	$(LD) $(LDFLAGS) -o $(TARGET) $(CRT0) main.o $(LIBC)
	@echo "Built $(TARGET) (C rollback)"

clean:
	rm -rf build $(TARGET) $(RLIB) main.o

.PHONY: all clean c-rollback
