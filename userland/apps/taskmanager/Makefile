# Makefile for taskmanager - MayteraOS Task Manager (#487/#349). RUST ONLY.
#
# THERE IS NO C BUILD OF THIS APP, AND THERE MUST NOT BE ONE AGAIN (#179, owner
# request, 2026-08-20). main.c was the pre-#487 C twin, kept "for rollback".
# It never served a rollback. It cost two real defects in a single week:
#
#   #145 fixed the idle-as-consumer CPU-ranking defect in the twin but left its
#     pid-indexed baseline (`if (pid < 64) prev_ticks[pid]`) in place. The one
#     file whose stated job was to carry the fix so a rollback could not quietly
#     reintroduce the bug carried HALF of it, and said in a comment that it
#     carried all of it. #178 had to find that.
#   #178 found that `make c` and the default `make` WROTE THE SAME FILENAME,
#     $(TARGET). Once anyone had run `make c`, the C binary was newer than every
#     prerequisite, a plain `make` printed "Nothing to be done for 'all'" and
#     left the ROLLBACK COPY sitting there under the shipping name. The golden
#     build runs a plain `make` in each app dir. That was a silent path to
#     SHIPPING THE ROLLBACK BINARY AS IF IT WERE THE RUST APP.
#
# Git is the rollback. The C source is recoverable at commit 33a296ea and is
# named in the CHANGELOG entry for #179. A second implementation living beside
# the shipping one is not insurance; it is a second thing to keep correct, and
# the record above is what happens when it is not kept correct.
#
# The `no-c-twin` guard and the `c` tombstone below are not decoration. They
# make the reintroduction FAIL A BUILD instead of shipping.
#
# TARGET NOTES (each verified against `rustc --print target-spec-json`, not
# assumed; getting any of these wrong produces an image that links and then
# misbehaves):
#   --target x86_64-unknown-none is ALREADY soft-float (+soft-float,-sse,-sse2).
#     Keep it. (An older version of this comment justified it with "the kernel
#     never saves FPU state across a context switch, sse_save/sse_restore have
#     zero callers". The zero-callers half is true, the conclusion is NOT:
#     proc/context_switch.asm does fxsave64/fxrstor64 on every switch, so FPU
#     state IS preserved. The reason to stay soft-float here is the target spec
#     itself plus keeping this app's arithmetic fixed-point and identical to the
#     kernel-side twin it reads from, not a missing save/restore.)
#   The same target ALSO defaults to:
#     code-model=kernel -> WRONG for a Ring-3 image linked at 0x80000000 by
#                          user.ld. This does NOT need -C code-model=large: the
#                          #89 fix (see arena/Makefile) found that rustc's large-
#                          model + PIC codegen computes a per-callsite GOT-base
#                          register (lea rip-landmark; movabs r10,<delta>; add
#                          r10,landmark; call [r10+off]) that this ld resolves
#                          to a WRONG delta, jumping every extern call (the
#                          very first thing main() does, syscall1, included)
#                          into garbage. taskmgr hit exactly this (ticket #153,
#                          2026-08-16, iMac "task manager is not opening"):
#                          the disassembly of main() shows the broken
#                          movabs-GOT-base sequence wrapping the syscall1()
#                          call that creates the window, so it never got that
#                          far. Default (small) code model + relocation-
#                          model=pic gives ordinary call [rip+disp32] GOT-
#                          relative calls instead, which the loader's
#                          R_X86_64_RELATIVE fixups resolve correctly.
#                          taskmgr (~1MB) is nowhere near the small model's
#                          +-2GB reach limit.
#     PIE               -> WRONG; user apps are -fno-pic and statically linked.
#                          Overridden with -C relocation-model=pic.

LD = ld
RUSTC = rustc
RUST_PIN = 1.97.0

LIBC_DIR = ../../libc

LDFLAGS = -pie --no-dynamic-linker -z notext -nostdlib -z max-page-size=0x1000 -T ../../user-pie.ld

RUSTFLAGS = --edition 2021 --crate-type staticlib --target x86_64-unknown-none \
            -C relocation-model=pic \
            -C panic=abort -C opt-level=2

TARGET = taskmgr
RLIB   = librusttaskmgr.a

CRT0 = $(LIBC_DIR)/crt0.o
LIBC = $(LIBC_DIR)/libc.a

all: no-c-twin $(TARGET)

# #179: structural, not a convention. If a C source file ever reappears in this
# directory the build STOPS HERE, before anything is linked, instead of leaving
# a second implementation to be discovered months later by the defect it grew.
# There is deliberately no rule that can compile a .c in this directory, so a
# reappearing twin is inert as well as loud; this makes it loud.
no-c-twin:
	@if ls *.c >/dev/null 2>&1; then \
	  echo "ERROR: a C source file has reappeared in userland/apps/taskmanager:"; \
	  ls -1 *.c | sed 's/^/         /'; \
	  echo "This app is Rust only (main.rs). The C twin was removed at #179 by owner"; \
	  echo "request, because it shipped a half-fix (#145) and could silently overwrite"; \
	  echo "the shipping binary under its own name (#178). Read the header of this"; \
	  echo "Makefile. Delete the file; git history is the rollback."; \
	  exit 1; \
	fi

# #188: logic.rs is `include!`d by main.rs, so it MUST be a prerequisite. Left
# off, a change to a hit-test coordinate would leave the stale .a in place and
# `make` would relink the OLD logic under the shipping name - the same shape of
# silent-wrong-binary failure #178 records below, arriving by a different route.
$(RLIB): main.rs logic.rs
	@ver=`$(RUSTC) --version | awk '{print $$2}'`; \
	 if [ "$$ver" != "$(RUST_PIN)" ]; then \
	   echo "ERROR: active rustc $$ver != pinned $(RUST_PIN) (reproducibility gate)."; \
	   exit 1; \
	 fi
	$(RUSTC) $(RUSTFLAGS) -o $@ main.rs

# FORCE, so a plain 'make' ALWAYS relinks. Kept from #178: with the C target
# gone nothing else writes $(TARGET) any more, but relinking costs about a
# second and an up-to-date check that can be fooled by a foreign writer is the
# mechanism that nearly shipped the wrong binary once already.
$(TARGET): $(RLIB) $(CRT0) $(LIBC) FORCE
	$(LD) $(LDFLAGS) -o $@ $(CRT0) $(RLIB) $(LIBC)
	@echo "Built $(TARGET) (Rust)"
	@ls -l $(TARGET)

# #179: tombstone. 'make c' used to build the C twin OVER the shipping binary.
# It must fail loudly and say why, not fail with make's bare "No rule to make
# target", and above all it must never again quietly do something. Same for the
# 'c-rollback' spelling used by userland/apps/setup, so neither habit works here.
c c-rollback:
	@echo "ERROR: there is no C build of taskmgr and there will not be one."
	@echo "The C twin (main.c) was REMOVED at #179 by owner request. It shipped a"
	@echo "half-fix (#145) and this target used to overwrite the shipping Rust"
	@echo "binary under the same filename, which a plain 'make' then reported as"
	@echo "up to date (#178). Build with plain 'make'. To inspect the old C app,"
	@echo "read it out of git history (commit 33a296ea), do not restore it here."
	@exit 1

# #188: host-side assertions for the pure logic in logic.rs - the geometry that
# decides whether a click lands, and the row ordering.
#
# This is NOT a second build of the app and it does NOT write $(TARGET); it
# builds an ordinary HOST binary from logic_test.rs, which `include!`s the very
# same logic.rs that main.rs does. It is not part of `all`, so build-golden.sh's
# plain `make` never runs it and can never be confused by its output.
#
# It exists because the defect this app keeps growing is "the control renders
# and does nothing", and a screenshot cannot tell that apart from a working
# control. Each assertion has a RED ARM: a verbatim transcription of the
# shipping hit-test at 3e38449b, asserted to give the WRONG answer for the same
# input. Revert the fix and the green half fails; paste the fix into the red arm
# and the red half fails.
LOGIC_TEST_BIN = logic-test-host

logic-test: logic.rs logic_test.rs
	$(RUSTC) --edition 2021 -O -o $(LOGIC_TEST_BIN) logic_test.rs
	./$(LOGIC_TEST_BIN)

clean:
	rm -f *.o $(RLIB) $(TARGET) $(LOGIC_TEST_BIN)

FORCE:

.PHONY: all c c-rollback clean FORCE no-c-twin logic-test
