# Makefile for fmsynth - #182 the DOS OPL2 consumer of the one FM core. RUST.
#
# This app is the DOS-side consumer of userland/lib/opl2. It contains no
# synthesis of its own; see main.rs.
#
# TARGET NOTES. Each of these is verified against `rustc --print
# target-spec-json`, not assumed, because getting any of them wrong produces an
# image that links and then misbehaves.
#
#   --target x86_64-unknown-none is ALREADY soft-float:
#       "features": "-mmx,-sse,-sse2,...,+soft-float"
#       "rustc-abi": "softfloat"
#     MEASURED 2026-08-20: `-C target-feature=+sse,+sse2` does NOT override
#     this. A trivial `a * b + 1.5` compiled with and without the flag produced
#     byte-identical code calling __mulsf3/__addsf3 through the GOT. The float
#     ABI is part of the target, not a feature flag.
#
#     That is why the core is integer, and it is worth being precise about:
#     userland C on this project IS hardware SSE2 (`-msse -msse2 -mfpmath=sse`,
#     real mulsd, no soft-float libcalls). Userland RUST, on the pinned target,
#     is not. Do not generalise either fact to the other language.
#
#     NOTE for whoever reads userland/apps/taskmanager/Makefile next: its
#     equivalent comment justifies soft-float with "this kernel never saves FPU
#     state across a context switch (sse_save/sse_restore have zero callers)".
#     The zero-callers half is true; the conclusion is NOT. CLAUDE.md's own
#     correction of 2026-08-10 records that proc/context_switch.asm:63,69 does
#     fxsave64/fxrstor64 on every switch. FPU state IS saved. The soft-float
#     constraint here rests on the TARGET SPEC quoted above, which is a fact
#     that can be re-measured in one command.
#
#   -C relocation-model=pic, and NOT -C code-model=large. The target defaults
#     to code-model=kernel, which is wrong for a Ring-3 image. The obvious fix
#     (large) is the WRONG one: #89/#153 found that rustc's large+PIC codegen
#     computes a per-callsite GOT base that this ld resolves to a wrong delta,
#     sending every extern call into garbage. Default (small) + pic gives
#     ordinary `call [rip+disp32]` which the loader's R_X86_64_RELATIVE fixups
#     resolve correctly. This app is nowhere near the small model's 2 GB reach.

CC = gcc
LD = ld
RUSTC = rustc
RUST_PIN = 1.97.0

LIBC_DIR = ../../libc
OPL2_DIR = ../../lib/opl2

LDFLAGS = -pie --no-dynamic-linker -z notext -nostdlib -z max-page-size=0x1000 -T ../../user-pie.ld

RUSTFLAGS = --edition 2021 --crate-type staticlib --target x86_64-unknown-none \
            -C relocation-model=pic \
            -C panic=abort -C opt-level=2

TARGET = FMSYNTH
RLIB   = librustfmsynth.a

CRT0 = $(LIBC_DIR)/crt0.o
LIBC = $(LIBC_DIR)/libc.a

# The core's sources are prerequisites. Without them a change to opl2core.rs
# would not rebuild this app, and the in-OS arm would silently keep testing an
# older core than the host arm does, which is the two-arms-disagree failure the
# whole one-core design exists to prevent.
OPL2_SRCS = $(OPL2_DIR)/opl2.rs $(OPL2_DIR)/opl2_tables.rs $(OPL2_DIR)/opl2core.rs \
            $(OPL2_DIR)/opl2check.rs $(OPL2_DIR)/opl2selftest.rs

all: $(TARGET)

$(RLIB): main.rs $(OPL2_SRCS)
	@ver=`$(RUSTC) --version | awk '{print $$2}'`; \
	 if [ "$$ver" != "$(RUST_PIN)" ]; then \
	   echo "ERROR: active rustc $$ver != pinned $(RUST_PIN) (reproducibility gate)."; \
	   exit 1; \
	 fi
	@bash $(OPL2_DIR)/table-gate.sh ../../.. || \
	  { echo "ERROR: OPL2 ROM tables do not match their generating formulas."; exit 1; }
	$(RUSTC) $(RUSTFLAGS) -o $@ main.rs

# FORCE, so a plain `make` ALWAYS relinks. See the taskmanager Makefile for why
# this is not paranoia: a stale binary sitting under the shipping name is a
# silent path to shipping the wrong thing, and the golden build runs a plain
# `make` in each app directory.
$(TARGET): $(RLIB) $(CRT0) $(LIBC) FORCE
	$(LD) $(LDFLAGS) -o $@ $(CRT0) $(RLIB) $(LIBC)
	@echo "Built $(TARGET) (Rust)"
	@ls -l $(TARGET)

clean:
	rm -f $(RLIB) $(TARGET)

FORCE:

.PHONY: all clean FORCE
