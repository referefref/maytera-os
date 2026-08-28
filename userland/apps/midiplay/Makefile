# Makefile for midiplay - MayteraOS MIDI player (#183). RUST ONLY.
#
# There is no C twin of this app and there must not be one. #179's record of
# what a "kept for rollback" C copy costs is in userland/apps/taskmanager/
# Makefile: two real defects in one week, and a plain `make` that silently
# shipped the rollback binary because `make c` wrote the same target name. Git
# is the rollback.
#
# TARGET NOTES, each verified against `rustc --print target-spec-json` rather
# than assumed. Getting any of them wrong produces an image that links and then
# misbehaves:
#   --target x86_64-unknown-none is ALREADY soft-float (+soft-float,-sse,-sse2)
#     and this app is integer throughout, which is why. A float here is a
#     libcall, not an instruction.
#   The same target defaults to code-model=kernel, which is WRONG for a Ring-3
#     image, and to PIE. -C relocation-model=pic with the DEFAULT (small) code
#     model is the combination that works: #89/#153 found that large-model PIC
#     codegen computes a per-callsite GOT base that this linker resolves to the
#     wrong delta, sending the very first extern call into garbage.

CC = gcc
LD = ld
RUSTC = rustc
RUST_PIN = 1.97.0

LIBC_DIR = ../../libc
OPL2_DIR = ../../lib/opl2
MIDI_DIR = ../../lib/midi

LDFLAGS = -pie --no-dynamic-linker -z notext -nostdlib -z max-page-size=0x1000 -T ../../user-pie.ld

RUSTFLAGS = --edition 2021 --crate-type staticlib --target x86_64-unknown-none \
            -C relocation-model=pic \
            -C panic=abort -C opt-level=2

TARGET = MIDIPLAY
RLIB   = librustmidiplay.a

CRT0 = $(LIBC_DIR)/crt0.o
LIBC = $(LIBC_DIR)/libc.a

# Both libraries' sources are prerequisites. Without them a change to the FM
# core or to the MIDI parser would not rebuild this app, and the shipped player
# would silently be running an older library than the host harness tests.
OPL2_SRCS = $(OPL2_DIR)/opl2.rs $(OPL2_DIR)/opl2_tables.rs $(OPL2_DIR)/opl2core.rs \
            $(OPL2_DIR)/opl2check.rs $(OPL2_DIR)/opl2selftest.rs
MIDI_SRCS = $(MIDI_DIR)/midi.rs $(MIDI_DIR)/midi_tables.rs $(MIDI_DIR)/smf.rs \
            $(MIDI_DIR)/gm2opl.rs $(MIDI_DIR)/player.rs $(MIDI_DIR)/midiselftest.rs

all: $(TARGET)

$(RLIB): main.rs $(OPL2_SRCS) $(MIDI_SRCS)
	@ver=`$(RUSTC) --version | awk '{print $$2}'`; \
	 if [ "$$ver" != "$(RUST_PIN)" ]; then \
	   echo "ERROR: active rustc $$ver != pinned $(RUST_PIN) (reproducibility gate)."; \
	   exit 1; \
	 fi
	@bash $(OPL2_DIR)/table-gate.sh ../../.. || \
	  { echo "ERROR: OPL2 ROM tables do not match their generating formulas."; exit 1; }
	@bash $(MIDI_DIR)/table-gate.sh ../../.. || \
	  { echo "ERROR: MIDI note/velocity/bend tables do not match their formulas."; exit 1; }
	@bash $(OPL2_DIR)/core-gate.sh ../../.. || \
	  { echo "ERROR: a second FM synthesis core appeared in the tree."; exit 1; }
	$(RUSTC) $(RUSTFLAGS) -o $@ main.rs

# FORCE, so a plain `make` ALWAYS relinks. Not paranoia: see the taskmanager
# Makefile for the case where a stale binary sat under the shipping name and the
# golden build's plain `make` printed "Nothing to be done".
$(TARGET): $(RLIB) $(CRT0) $(LIBC) FORCE
	$(LD) $(LDFLAGS) -o $@ $(CRT0) $(RLIB) $(LIBC)
	@echo "Built $(TARGET) (Rust)"
	@ls -l $(TARGET)

# A C twin of this app must FAIL THE BUILD rather than ship.
no-c-twin:
	@if ls *.c >/dev/null 2>&1; then \
	  echo "ERROR: a C source appeared in userland/apps/midiplay. This app is"; \
	  echo "Rust-only by decision (#183, and the ALL-NEW-KERNEL-AND-APP-CODE-IN-RUST"; \
	  echo "rule). Git is the rollback; a second implementation is not."; \
	  exit 1; \
	fi
	@echo "no-c-twin: OK"

clean:
	rm -f $(RLIB) $(TARGET)

FORCE:

.PHONY: all clean FORCE no-c-twin
