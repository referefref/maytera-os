# Makefile for gbemu - MayteraOS Game Boy emulator (clean-room from the public
# hardware spec; see gb.rs header). RUST ONLY, following the established
# no-C-twin pattern (taskmanager #179, setup #185): a second implementation
# beside the shipping one is a defect waiting to happen, not insurance.
#
# TARGET NOTES, identical reasoning to taskmanager/setup/fmtest (read those
# Makefiles for the full measured history; not repeated here):
#   --target x86_64-unknown-none is ALREADY soft-float and already
#     code-model=kernel/non-PIC by default.
#   -C relocation-model=pic, and DO NOT add -C code-model=large: the #89/#153
#     defect is that large+PIC codegen computes a per-callsite GOT-base
#     register this linker resolves to a wrong delta, jumping every extern
#     call into garbage. Default (small) + pic gives ordinary
#     call [rip+disp32], which the loader's R_X86_64_RELATIVE fixups resolve
#     correctly. gbemu (core + glue) is nowhere near the small model's 2GB
#     reach.
#   -C panic=abort: no unwinder in Ring 3; the panic handler in main.rs exits.

LD = ld
RUSTC = rustc
RUST_PIN = 1.97.0

LIBC_DIR = ../../libc

LDFLAGS = -pie --no-dynamic-linker -z notext -nostdlib -z max-page-size=0x1000 -T ../../user-pie.ld

RUSTFLAGS = --edition 2021 --crate-type staticlib --target x86_64-unknown-none             -C relocation-model=pic             -C panic=abort -C opt-level=2

TARGET = gbemu
RLIB   = librustgbemu.a

CRT0 = $(LIBC_DIR)/crt0.o
LIBC = $(LIBC_DIR)/libc.a

all: no-c-twin $(TARGET)

no-c-twin:
	@if ls *.c >/dev/null 2>&1; then \
	  echo "ERROR: a C source file has appeared in userland/apps/gbemu:"; \
	  ls -1 *.c | sed "s/^/         /"; \
	  echo "This app is Rust only (main.rs + gb.rs). See the taskmanager/setup"; \
	  echo "Makefiles for why a C twin beside a Rust app is a recorded defect"; \
	  echo "class (#179/#185), not a rollback path."; \
	  exit 1; \
	fi

# gb.rs is #[path]-included by main.rs (and by hosttest/main.rs), so it is a
# real prerequisite: without it, editing the CPU/PPU core would not trigger a
# relink and the shipping binary would silently keep testing an older core.
$(RLIB): main.rs gb.rs
	@ver=$$($(RUSTC) --version | cut -d" " -f2); \
	 if [ "$$ver" != "$(RUST_PIN)" ]; then \
	   echo "ERROR: active rustc $$ver != pinned $(RUST_PIN) (reproducibility gate)."; \
	   exit 1; \
	 fi
	$(RUSTC) $(RUSTFLAGS) -o $@ main.rs

# FORCE, so a plain 'make' ALWAYS relinks (see taskmanager/setup Makefiles for
# why an up-to-date check that a foreign writer can fool is not paranoia here).
$(TARGET): $(RLIB) $(CRT0) $(LIBC) FORCE
	$(LD) $(LDFLAGS) -o $@ $(CRT0) $(RLIB) $(LIBC)
	@echo "Built $(TARGET) (Rust)"
	@ls -l $(TARGET)

clean:
	rm -f *.o $(RLIB) $(TARGET)

FORCE:

.PHONY: all clean FORCE no-c-twin
