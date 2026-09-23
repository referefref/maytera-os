# Makefile for stellarium - MayteraOS Planetarium (Rust, hardware float).
#
# WHY THIS APP IS DIFFERENT FROM EVERY OTHER RUST APP IN THE TREE:
#   The star-field maths is f64 trigonometry (sin/cos/asin/atan2, stereographic
#   projection). The stock userland Rust target x86_64-unknown-none is
#   +soft-float,-sse,-sse2 - correct for apps that read fixed-point kernel data,
#   wrong for an app that IS the float. So this app builds against a LOCAL
#   hardware-float target (x86_64-maytera-user.json: -mmx off, +sse,+sse2 on,
#   softfloat ABI removed) and FFI's libm (sin/cos/...) out of libc.a, which is
#   ABI-sound because the target restores the SysV xmm f64 convention. The
#   `no-softfloat-libcall` gate below PROVES the result emits real mulsd/addsd/
#   sqrtsd and links ZERO __*df3 soft-float helpers.
#
#   A custom target has no prebuilt std, so this needs -Z build-std (cargo), not
#   the bare `rustc` line the other apps use. RUSTC_BOOTSTRAP=1 unlocks the -Z
#   flags on the pinned STABLE rustc without a nightly toolchain.
#
# DO NOT call SYS_WIN_GET_SIZE at startup from this build: it wedges the process
# (see src/lib.rs and blame.md, 2026-09-06). The app tracks size via EVENT_RESIZE.

RUSTC   = rustc
CARGO   = cargo
LD      = ld
RUST_PIN = 1.97.0
TARGET_JSON = x86_64-maytera-user.json
TARGET_NAME = x86_64-maytera-user

LIBC_DIR = ../../libc
CRT0 = $(LIBC_DIR)/crt0.o
LIBC = $(LIBC_DIR)/libc.a

RLIB = target/$(TARGET_NAME)/release/libstellarium.a
BIN  = stellarium

LDFLAGS = -pie --no-dynamic-linker -z notext -nostdlib -z max-page-size=0x1000 -T ../../user-pie.ld

all: no-c-twin $(BIN)

# This app is Rust only. A reappearing .c must fail the build, not ship a twin
# (same rule as taskmanager #179).
no-c-twin:
	@if ls *.c >/dev/null 2>&1; then \
	  echo "ERROR: a C source file has reappeared in apps/stellarium; this app is Rust only."; \
	  ls -1 *.c | sed 's/^/         /'; exit 1; \
	fi

$(RLIB): src/lib.rs src/stars.rs Cargo.toml $(TARGET_JSON)
	@ver=`$(RUSTC) --version | awk '{print $$2}'`; \
	 if [ "$$ver" != "$(RUST_PIN)" ]; then \
	   echo "ERROR: active rustc $$ver != pinned $(RUST_PIN) (reproducibility gate)."; exit 1; \
	 fi
	RUSTC_BOOTSTRAP=1 $(CARGO) build --release \
	  -Z json-target-spec -Z build-std=core,compiler_builtins \
	  --target $(TARGET_JSON)

$(BIN): $(RLIB) $(CRT0) $(LIBC) FORCE
	$(LD) $(LDFLAGS) -o $@ $(CRT0) $(RLIB) $(LIBC)
	@$(MAKE) --no-print-directory no-softfloat-libcall
	@echo "Built $(BIN) (Rust, hardware float)"
	@ls -l $(BIN)

# HARDWARE-FLOAT INVARIANT: the whole point of the custom target. If the build
# ever silently falls back to soft-float, __adddf3/__subdf3/__muldf3/__divdf3
# (and friends) reappear as libgcc/compiler-builtins libcalls. Fail the build if
# any survive in the app's own archive.
no-softfloat-libcall: $(RLIB)
	@n=`nm $(RLIB) 2>/dev/null | grep -cE " U __(add|sub|mul|div|gt|lt|ge|le|eq|ne)df[0-9]"`; \
	 if [ "$$n" != "0" ]; then \
	   echo "ERROR: $$n soft-float libcall(s) leaked into stellarium - hardware-float target regressed."; \
	   nm $(RLIB) | grep -E " U __(add|sub|mul|div|gt|lt|ge|le|eq|ne)df[0-9]"; exit 1; \
	 fi

c c-rollback:
	@echo "ERROR: there is no C build of stellarium and there will not be one."; exit 1

clean:
	rm -f $(BIN); rm -rf target

FORCE:
.PHONY: all clean FORCE no-c-twin no-softfloat-libcall c c-rollback
