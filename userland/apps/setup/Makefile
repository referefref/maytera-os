# Makefile for the first-boot setup wizard (OOBE) - #745. RUST ONLY.
#
# THERE IS NO C BUILD OF THIS APP, AND THERE MUST NOT BE ONE AGAIN (#185,
# 2026-08-20). main.c was the pre-Rust C twin, kept "for rollback". It never
# served a rollback, and it carried the identical shape the owner had just had
# removed from the Task Manager one day earlier (#179):
#
#   THE TARGET COLLISION. `c-rollback` and the default `all` WROTE THE SAME
#     FILENAME, $(TARGET) = SETUP. Measured in this directory before the
#     removal: the Rust wizard is 1,225,768 bytes md5
#     2ed74c01b264e20823cec1415258aa2c; `make c-rollback` left 208,072 bytes
#     md5 910a42b30d0ba5fc5d4b059ddfcb9d54 under that same name, and the very
#     next plain `make` printed "Nothing to be done for 'all'." and left the C
#     binary sitting there. build/build-golden.sh runs a PLAIN `make` in every
#     app dir. That was a silent path to shipping the rollback binary as the
#     FIRST-BOOT EXPERIENCE, which is the single most visible screen the OS
#     has. Exactly the defect #178 found in taskmanager, in the app where it
#     would have cost the most.
#
#   THE TWIN WAS EIGHT DAYS AND TWENTY-THREE COMMITS STALE. main.c last
#     changed at 45896b84 (2026-08-10) and 668 lines long; main.rs had 23
#     further commits and stands at 7,698 lines. What a rollback would have
#     restored, page for page: 10 pages instead of 11 (no PG_APPSW, the
#     apps/widgets first-boot page, #15); the Network page FORCED ON, when the
#     shipping wizard deliberately holds it off behind NETWORK_PAGE_ENABLED
#     (ae3385b4); no Skip to Desktop / Restart / Shut Down corner controls and
#     no /CONFIG/SETUPSKIP (#136); no login_mode written at all, so the
#     list-vs-typed sign-in choice #745 added silently vanishes; the old flat
#     card instead of the glass design system that docs/UI_GLASS_DESIGN_SYSTEM
#     .md names main.rs as the REFERENCE IMPLEMENTATION of; and no
#     win_invalidate() present commit (#155), so it rides the kernel's legacy
#     compatibility path rather than the current contract.
#
# Git is the rollback. The C source is recoverable at commit 3f7dd887 and is
# named in the CHANGELOG entry for #185. A second implementation beside the
# shipping one is not insurance; it is a second thing to keep correct, and
# #145 (a fix that reached the twin at half strength while a comment claimed
# it was whole) is what happens when it is not kept correct.
#
# The `no-c-twin` guard and the `c` / `c-rollback` tombstone below are not
# decoration. They make a reintroduction FAIL A BUILD instead of ship.
#
# TARGET NOTES:
#   --target x86_64-unknown-none is ALREADY soft-float (+soft-float,-sse,-sse2).
#   It also defaults to code-model=kernel and non-PIC, both WRONG here:
#   userland is PIE (user-pie.ld) and the launched-app gate rejects a non-PIE
#   binary, so -C relocation-model=pic. Do NOT add -C code-model=large: the #89
#   (arena) / #153 (taskmgr, confirmed by disassembly) defect is that rustc's
#   large-model + PIC codegen computes a per-callsite GOT-base register that
#   this ld resolves to a WRONG delta, jumping every extern call into garbage.
#   The default small model reaches +-2GB, far more than this app needs.

LD = ld
RUSTC = rustc
RUST_PIN = 1.97.0

LIBC_DIR = ../../libc

LDFLAGS = -pie --no-dynamic-linker -z notext -nostdlib -z max-page-size=0x1000 -T ../../user-pie.ld

RUSTFLAGS = --edition 2021 --crate-type staticlib --target x86_64-unknown-none \
            -C relocation-model=pic \
            -C panic=abort -C opt-level=2

TARGET = SETUP
RLIB   = librustsetup.a

CRT0 = $(LIBC_DIR)/crt0.o
LIBC = $(LIBC_DIR)/libc.a

all: no-c-twin $(TARGET)

# #185: structural, not a convention. If a C source file ever reappears in this
# directory the build STOPS HERE, before anything is linked, instead of leaving
# a second implementation to be discovered later by the defect it grew. There
# is deliberately no rule below that can compile a .c, so a reappearing twin is
# inert as well as loud; this is what makes it loud.
no-c-twin:
	@if ls *.c >/dev/null 2>&1; then \
	  echo "ERROR: a C source file has reappeared in userland/apps/setup:"; \
	  ls -1 *.c | sed 's/^/         /'; \
	  echo "This app is Rust only (main.rs). The C twin was removed at #185 because"; \
	  echo "'make c-rollback' overwrote the shipping binary under its own name and a"; \
	  echo "plain 'make' then reported it up to date, and because the twin was 23"; \
	  echo "commits behind the app it claimed to be a rollback for. Read the header"; \
	  echo "of this Makefile. Delete the file; git history is the rollback."; \
	  exit 1; \
	fi

$(RLIB): main.rs
	@ver=`$(RUSTC) --version | awk '{print $$2}'`; \
	 if [ "$$ver" != "$(RUST_PIN)" ]; then \
	   echo "ERROR: active rustc $$ver != pinned $(RUST_PIN) (reproducibility gate)."; \
	   exit 1; \
	 fi
	$(RUSTC) $(RUSTFLAGS) -o $@ main.rs

# FORCE, so a plain 'make' ALWAYS relinks. With the C target gone nothing else
# writes $(TARGET) any more, but relinking costs under a second, and an
# up-to-date check that a foreign writer can fool is the exact mechanism that
# would have shipped the wrong wizard.
$(TARGET): $(RLIB) $(CRT0) $(LIBC) FORCE
	$(LD) $(LDFLAGS) -o $@ $(CRT0) $(RLIB) $(LIBC)
	@echo "Built $(TARGET) (Rust)"
	@ls -l $(TARGET)

# #185: tombstone. 'make c-rollback' used to build the C twin OVER the shipping
# binary. It must fail loudly and say why, not fail with make's bare "No rule to
# make target", and above all it must never again quietly do something. The 'c'
# spelling used by userland/apps/taskmanager is covered too, so neither habit
# works in either directory.
c c-rollback:
	@echo "ERROR: there is no C build of the setup wizard and there will not be one."
	@echo "The C twin (main.c) was REMOVED at #185. This target used to overwrite the"
	@echo "shipping Rust binary under the same filename SETUP, which the next plain"
	@echo "'make' then reported as up to date - and build-golden.sh runs a plain"
	@echo "'make', so that shipped the rollback binary as the first-boot experience."
	@echo "It was also 23 commits stale. Build with plain 'make'. To inspect the old"
	@echo "C app, read it out of git history (commit 3f7dd887); do not restore it here."
	@exit 1

clean:
	rm -rf build *.o $(RLIB) $(TARGET)

FORCE:

.PHONY: all c c-rollback clean FORCE no-c-twin
