CC = gcc
LD = ld
RUSTC = rustc

LIBC_DIR = ../../libc

# Detect GCC include path for stdint.h etc. in freestanding mode
GCC_ISYSTEM := $(shell $(CC) -print-file-name=include 2>/dev/null)

CFLAGS = -fPIE -m64 -ffreestanding -fno-builtin -nostdinc -nostdlib \
         -fstack-protector-strong -mstack-protector-guard=global -mno-red-zone -Wall -Wextra \
         -I$(LIBC_DIR) -I. -I../../libgl/include -O2 \
         -isystem $(GCC_ISYSTEM)

LDFLAGS = -pie --no-dynamic-linker -z notext -T ../../user-pie.ld -nostdlib -z max-page-size=0x1000

SRCS = main.c draw.c icons.c lockscreen.c desktop.c taskbar.c \
       startmenu.c clock.c contextmenu.c screensaver.c screensaver_gfx.c wallpaper.c widgets.c traymenu.c profile.c \
       stickies.c notif.c bt_impl.c screenshot.c vnc.c launcher.c ha_format.c \
       elevate.c iconpicker.c confirmdialog.c perfframe.c volosd.c dragghost.c idleprof.c

# #334 headless GUI verification hook (testhook.c). OFF by default - a plain
# `make`/`make install` (what build/build-golden.sh always uses) builds
# WITHOUT it, so the shipping COMPOSIT contains none of its code or symbols.
# Only `make TESTHOOK=1` (a throwaway verification build, never deployed to
# a golden image) compiles it in. See testhook.c for the full rationale.
ifdef TESTHOOK
CFLAGS += -DMAYTERA_TESTHOOK=1
SRCS += testhook.c
endif

# #223 ROUND 2: throwaway canary/write-site instrumentation for g_fav_count
# (startmenu.c). OFF by default, same idiom as TESTHOOK above - never in the
# golden. `make FAVDEBUG=1` (usually combined with TESTHOOK=1 so the repro
# can be driven by testhook verbs instead of mouse injection).
ifdef FAVDEBUG
CFLAGS += -DMAYTERA_FAVDEBUG=1
endif

OBJS = $(SRCS:.c=.o)

CRT0 = $(LIBC_DIR)/crt0.o
LIBC = $(LIBC_DIR)/libc.a
LIBGL = ../../libgl/libgl.a   # #319 TinyGL (reconciled #336)

# --- USERLAND Rust Start-menu content model (startmenu_model.rs) ----------
# The directive behind this crate: the Start menu must never contain a
# hardcoded entry, in any language, ever - see startmenu_model.rs's header for
# the full rationale. Toolchain PINNED via rust-toolchain.toml (channel
# 1.97.0, target x86_64-unknown-none), same recipe as Arena/RSS/Task Manager
# (userland/apps/arena, userland/apps/rss, userland/apps/taskmanager).
#
# CODE MODEL: deliberately NOT -C code-model=large, unlike those three. Their
# Makefiles justify `code-model=large` as "matches -mcmodel=large on the C
# side" - but MEASURED (grepping their own CFLAGS): none of the three actually
# pass -mcmodel=large to gcc. That comment was stale, copy-pasted forward
# through three Makefiles, and never true. The compositor's own CFLAGS are
# plain `-fPIE` (no -mcmodel override), so this uses rustc's DEFAULT code
# model for x86_64-unknown-none (small/PIC-friendly) with
# -C relocation-model=pic, matching what -fPIE actually produces. Using
# code-model=large here reproduced a real, confirmed hang (#<startmenu-rust>):
# the compositor's process froze at its boot splash with ZERO further
# progress, and checkpoint logging showed it happening on the very FIRST
# statement of the first Rust function ever called - a plain extern "C"
# call-out, before any static/global access - consistent with large-model
# object code computing an absolute callee address that a minimal static-PIE
# loader (no real ld.so; the kernel only fixes up RELATIVE relocations) never
# rebases. Removing code-model=large fixed it; verified red then green on VM
# 2900 (see blame.md).
RUST_PIN_VERSION = 1.97.0
RUST_SRC         = startmenu_model.rs
RUST_LIB         = libstartmenu_model.a
RUSTFLAGS = --edition 2021 --crate-type staticlib --target x86_64-unknown-none \
            -C opt-level=2 -C panic=abort \
            -C relocation-model=pic

all: COMPOSIT

$(RUST_LIB): $(RUST_SRC) rust-toolchain.toml
	@echo "Compiling userland Rust Start-menu model $(RUST_LIB) (pinned rustc $(RUST_PIN_VERSION))..."
	@ver=`$(RUSTC) --version | awk '{print $$2}'`; \
	 if [ "$$ver" != "$(RUST_PIN_VERSION)" ]; then \
	   echo "ERROR: active rustc $$ver != pinned $(RUST_PIN_VERSION) (reproducibility gate)."; \
	   echo "       Install the pin: rustup toolchain install $(RUST_PIN_VERSION) --profile minimal &&"; \
	   echo "       rustup target add --toolchain $(RUST_PIN_VERSION) x86_64-unknown-none"; \
	   exit 1; \
	 fi
	$(RUSTC) $(RUSTFLAGS) -o $@ $<

# libstartmenu_model.a is linked AFTER the C objects (startmenu.c has a real
# call into sm_model_reset() etc., so the archive members are pulled on
# demand) and BEFORE libgl/libc, so ld resolves its malloc/free/realloc/abort
# references against libc last - identical link-order reasoning to Arena.
COMPOSIT: $(OBJS) $(CRT0) $(RUST_LIB) $(LIBGL) $(LIBC)
	$(LD) $(LDFLAGS) -o $@ $(CRT0) $(OBJS) $(RUST_LIB) $(LIBGL) $(LIBC)
	@echo "Built COMPOSIT"
	@ls -l COMPOSIT

%.o: %.c compositor.h
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f *.o $(RUST_LIB) COMPOSIT

install: COMPOSIT
	cp COMPOSIT ../../../COMPOSIT

.PHONY: all clean install
