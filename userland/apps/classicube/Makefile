# MayteraOS ClassiCube port (#28) - build wiring.
#
# WHAT THIS BUILDS. ClassiCube is unusually self-contained: every platform
# .c in its tree is wrapped in an #if on a backend id from Core.h, so the
# WHOLE vendor src/*.c wildcard is compiled and the files that do not apply
# to us collapse to empty translation units. That is why there is no source
# list here and no per-platform exclusion: adding our Platform_/Window_/
# Graphics_/Audio_ files to THIS directory is all that is needed for them to
# be picked up (see MOS_SRCS below).
#
# See PORT-STATUS.md for the measured libc gap analysis, the backend-viability
# matrix, and the verification plan.

CC = gcc
LD = ld
LIBC_DIR      = ../../libc
LIBGL_DIR     = ../../libgl
USERLAND_DIR  = ../..
BUILD_DIR     = build

# Reproducibility, same rule as userland/apps/assaultcube/Makefile: prefer the
# in-repo vendored engine source so a `git archive` of the repo builds with no
# external stage, and fall back to the the build container stage clone if vendor/ is absent.
# fetch-upstream.sh populates vendor/ClassiCube from the pinned commit and
# its `verify` subcommand proves the vendored tree still equals that commit.
CC_MK_DIR    := $(dir $(lastword $(MAKEFILE_LIST)))
CC_VENDOR    ?= $(firstword $(wildcard $(CC_MK_DIR)vendor/ClassiCube/src) <workspace>)

# The engine source is COMPILED FROM A STAGED COPY under $(BUILD_DIR)/src, not
# from vendor/ in place. Core.h needs a MayteraOS platform branch, and patching
# the vendored file would leave the git tree dirty after every build, which
# build/repo-guard.sh rejects outright. Staging keeps vendor/ byte-identical to
# the pinned upstream commit and confines the patch to a build artifact that
# `make clean` removes.
SRC_DIR := $(BUILD_DIR)/src

# --- Backend selection -------------------------------------------------------
# Ids are GREPPED from the vendor Core.h, never guessed (an earlier pass of this
# port "tested GL1" by passing 1, which is actually SOFTGPU, and got a clean
# result that meant nothing). Current values in Core.h:
#     CC_GFX_BACKEND_SOFTGPU 1   GL1 2   GL2 3   GL11 7   SOFTMIN 8   SOFTFP 9
#     CC_AUD_BACKEND_OPENAL  1   NULL 4   MAYTERA 6 (added by the staged patch)
#     CC_SSL_BACKEND_NONE    1   BEARSSL 2
# MEASURED VIABILITY (see PORT-STATUS.md): SOFTGPU and SOFTMIN need nothing
# beyond the platform layer; GL1 resolves every gl* call against our existing
# TinyGL libgl.a and needs only the 8 GLContext_* glue functions; GL2 (49
# missing shader entry points) and GL11 (38, incl. display lists and fog) are
# NOT viable against TinyGL. SOFTFP does not compile under -nostdinc.
GFX_BACKEND ?= 1
# 6 = CC_AUD_BACKEND_MAYTERA, the id engine-patches/coreh-maytera.py adds and
# Audio_Maytera.c is guarded on. It MUST be set here and not left to Core.h's
# DEFAULT_AUD_BACKEND: this Makefile passes -DCC_AUD_BACKEND on the command
# line, and Core.h only applies its default when the macro is unset. Leaving
# this at 4 (NULL) compiles the whole MayteraOS audio backend out of existence
# and still links clean.
AUD_BACKEND ?= 6
SSL_BACKEND ?= 1

TARGET = CLASSICUBE

# --- Flags -------------------------------------------------------------------
# Modelled on userland/apps/glcube/Makefile, the closest existing app (freestanding
# C + libc + optional TinyGL). PIE, not the old fixed 0x80000000 base: userland
# moved to ET_DYN under user-pie.ld in #640, and -fPIE is why no -mcmodel=large
# is needed here.
#
# FLOAT: userland is HARDWARE float. x86-64 gcc has -msse/-msse2 on by default
# and -mfpmath=sse; nothing in the userland flag set disables it (only the KERNEL
# passes -mno-sse -mno-sse2). -msse -msse2 are stated explicitly below so the
# choice is visible in the file rather than inherited silently.
CFLAGS = -fPIE -ffreestanding -fstack-protector-strong -mstack-protector-guard=global \
         -mno-red-zone -msse -msse2 -nostdlib -nostdinc -fno-builtin -O3 -g \
         -DPLAT_MAYTERA \
         -DCC_GFX_BACKEND=$(GFX_BACKEND) \
         -DCC_AUD_BACKEND=$(AUD_BACKEND) \
         -DCC_SSL_BACKEND=$(SSL_BACKEND) \
         -I. -I$(SRC_DIR) -I$(LIBC_DIR) -I$(LIBGL_DIR)/include \
         -isystem /usr/lib/gcc/x86_64-linux-gnu/12/include

LDFLAGS = -pie --no-dynamic-linker -z notext -nostdlib -z max-page-size=0x1000 \
          -T $(USERLAND_DIR)/user-pie.ld

CRT0  = $(LIBC_DIR)/crt0.o
LIBC  = $(LIBC_DIR)/libc.a
LIBGL = $(LIBGL_DIR)/libgl.a

# Link libgl only for the GL backends (2 = GL1, 3 = GL2, 7 = GL11).
GL_BACKENDS = 2 3 7
ifneq ($(filter $(GFX_BACKEND),$(GL_BACKENDS)),)
GL_LIB = $(LIBGL)
else
GL_LIB =
endif

# --- Sources -----------------------------------------------------------------
# Vendor: the entire wildcard, on purpose (see the header comment).
# EXCLUSIONS. Kept to the absolute minimum and each one justified, because an
# excluded engine file is code the port silently does not have.
#
#   Http_Worker.c - Http_Maytera.c replaces the ENTIRE Http subsystem, not just
#     its transport: it runs on the kernel's async HTTP fetcher, which already
#     does TLS, instead of on sockets plus an SSL backend this port does not
#     build. Upstream offers no switch for that. Only the transport half of
#     Http_Worker.c sits behind CC_NET_BACKEND (lines 53-718); the request
#     queue below it (Http_GetResult, Http_CheckProgress, Http_ClearPending,
#     Http_TryCancel) is compiled whenever CC_BUILD_NETWORKING is defined, so
#     no backend id can turn it off. Excluding the file here is visible in the
#     build; a #define that silently drops half a file would not be.
#
# Everything else in the vendor tree is compiled, on purpose: the files that do
# not apply to this platform collapse to empty translation units by themselves.
VENDOR_EXCLUDE = Http_Worker.c
VENDOR_SRCS = $(filter-out $(VENDOR_EXCLUDE),$(notdir $(wildcard $(CC_VENDOR)/*.c)))
VENDOR_OBJS = $(patsubst %.c,$(BUILD_DIR)/vendor_%.o,$(VENDOR_SRCS))

# Ours: every .c dropped into THIS directory, which is how the platform layer
# (Platform_Maytera.c, Window_Maytera.c, Graphics_*.c, Audio_*.c, and the
# Socket_*/Http glue) gets built. No list to keep in sync.
MOS_SRCS = $(wildcard *.c)
MOS_OBJS = $(patsubst %.c,$(BUILD_DIR)/mos_%.o,$(MOS_SRCS))

ALL_OBJS = $(MOS_OBJS) $(VENDOR_OBJS)

.PHONY: all vendor-check stage probe gap linkcheck pixelformat clean

all: $(TARGET)

$(TARGET): $(ALL_OBJS) $(CRT0) $(LIBC) $(GL_LIB)
	$(LD) $(LDFLAGS) -o $@ $(CRT0) $(ALL_OBJS) $(GL_LIB) $(LIBC)
	@echo "Built $@"
	@readelf -h $@ | awk '/Type:|Entry/'
	@ls -l $@

# Stage the engine source, then add the Core.h platform branch. The patch script
# ASSERTS every anchor, so an upstream bump that moves those lines fails loudly
# instead of silently patching the wrong place (it already caught upstream's CRLF
# line endings). Idempotent: re-running is a no-op.
stage: vendor-check
	@mkdir -p $(SRC_DIR)
	@cp -u $(CC_VENDOR)/*.c $(CC_VENDOR)/*.h $(SRC_DIR)/
	@cp -u $(CC_VENDOR)/*.i $(SRC_DIR)/ 2>/dev/null || true
	@python3 engine-patches/coreh-maytera.py $(SRC_DIR)/Core.h
	@touch $(BUILD_DIR)/.staged

$(BUILD_DIR)/.staged: | $(BUILD_DIR)
	@$(MAKE) --no-print-directory stage

vendor-check:
	@if [ ! -d "$(CC_VENDOR)" ]; then \
	    echo "ClassiCube engine source not found: $(CC_VENDOR)"; \
	    echo "Run ./fetch-upstream.sh all to populate vendor/, see PORT-STATUS.md."; \
	    exit 1; \
	fi

$(BUILD_DIR):
	@mkdir -p $(BUILD_DIR)

$(BUILD_DIR)/vendor_%.o: $(SRC_DIR)/%.c | $(BUILD_DIR)/.staged
	$(CC) $(CFLAGS) -c $< -o $@

# The staged sources are produced by the `stage` rule, not by a pattern rule with
# a recipe. Without .PRECIOUS make classes them as intermediate files and deletes
# all ~100 of them at the end of every build, so the next build re-stages from
# scratch. They are build artifacts either way; `make clean` removes them.
.PRECIOUS: $(SRC_DIR)/%.c
$(SRC_DIR)/%.c: | $(BUILD_DIR)/.staged
	@:

$(BUILD_DIR)/mos_%.o: %.c | $(BUILD_DIR)/.staged
	$(CC) $(CFLAGS) -c $< -o $@

# --- Verification targets ----------------------------------------------------

# Compile everything, link nothing. Proves the whole tree is C-clean against our
# freestanding libc.
probe: $(ALL_OBJS)
	@echo "probe: $(words $(ALL_OBJS)) translation units compiled clean."

# THE FORMAT TRAP, measured rather than asserted. Compiles the probe against
# the STAGED Core.h (the one the app really builds with) and the real
# -DPLAT_MAYTERA, prints both channel layouts and the words a non-grey colour
# produces in each, and fails if they are not what the port assumes. Hosted
# build on purpose: it needs printf and it is a developer check, not shipped
# code. The COMPILE-TIME half of the same rule is
# MOS_GFX_ASSERT_PIXEL_FORMAT() in Window_Maytera.c.
pixelformat: $(BUILD_DIR)/.staged
	$(CC) -O2 -std=gnu99 -Wall -DPLAT_MAYTERA -I$(SRC_DIR) \
	    -o $(BUILD_DIR)/pixelprobe tests/pixelformat/pixelprobe.c
	@$(BUILD_DIR)/pixelprobe

# THE LIBC GAP ANALYSIS, reproducible. Prints every symbol the compiled objects
# reference that neither they, nor libc.a, nor libgl.a define, split into the
# ClassiCube platform interface (which the port itself must implement) and a
# genuine libc gap (which must be fixed in userland/libc, never forked here).
gap: probe
	@nm -g --defined-only $(LIBC) 2>/dev/null | awk '$$2~/[TDBRW]/{print $$3}' | sort -u > $(BUILD_DIR)/.libc.txt
	@nm -g --defined-only $(LIBGL) 2>/dev/null | awk '$$2~/[TDBRW]/{print $$3}' | sort -u > $(BUILD_DIR)/.libgl.txt
	@nm -g --defined-only $(BUILD_DIR)/*.o 2>/dev/null | awk '$$2~/[TDBRW]/{print $$3}' | sort -u > $(BUILD_DIR)/.def.txt
	@nm -u $(BUILD_DIR)/*.o 2>/dev/null | awk '$$1=="U"{print $$2}' | sort -u > $(BUILD_DIR)/.und.txt
	@comm -23 $(BUILD_DIR)/.und.txt $(BUILD_DIR)/.def.txt > $(BUILD_DIR)/.m1.txt
	@comm -23 $(BUILD_DIR)/.m1.txt $(BUILD_DIR)/.libc.txt > $(BUILD_DIR)/.m2.txt
	@comm -23 $(BUILD_DIR)/.m2.txt $(BUILD_DIR)/.libgl.txt > $(BUILD_DIR)/missing.txt
	@grep -E '^(Platform_|Window_|Socket_|SockAddr_|Thread_|Mutex_|Waitable_|File_|Directory_|Mem_|Updater_|DateTime_|Stopwatch_|Clipboard_|Gamepads_|OnscreenKeyboard_|DirectUrl_|Resume_|CrashHandler_|DynamicLib_|Process_|Cursor_|ReturnCode_|GLContext_|DisplayInfo$$|WindowInfo$$)' $(BUILD_DIR)/missing.txt > $(BUILD_DIR)/platform_gap.txt || true
	@grep -vE '^(Platform_|Window_|Socket_|SockAddr_|Thread_|Mutex_|Waitable_|File_|Directory_|Mem_|Updater_|DateTime_|Stopwatch_|Clipboard_|Gamepads_|OnscreenKeyboard_|DirectUrl_|Resume_|CrashHandler_|DynamicLib_|Process_|Cursor_|ReturnCode_|GLContext_|DisplayInfo$$|WindowInfo$$|_GLOBAL_OFFSET_TABLE_$$)' $(BUILD_DIR)/missing.txt > $(BUILD_DIR)/libc_gap.txt || true
	@echo "=== ClassiCube platform interface still to implement: $$(wc -l < $(BUILD_DIR)/platform_gap.txt) ==="
	@cat $(BUILD_DIR)/platform_gap.txt
	@echo "=== GENUINE libc gap (must be fixed in userland/libc): $$(wc -l < $(BUILD_DIR)/libc_gap.txt) ==="
	@cat $(BUILD_DIR)/libc_gap.txt

# Proves THE BUILD WIRING end to end without waiting for the platform layer:
# links the real objects against auto-generated stubs for whatever is still
# missing and checks the result is a valid ET_DYN PIE with the shape the kernel
# loader expects. The output deliberately lives under $(BUILD_DIR) and is NOT
# named $(TARGET), because build-golden.sh stages the first top-level ELF it
# finds in an app directory: a stub binary sitting at the top level would be
# shipped as /APPS/CLASSICUBE and would look exactly like a working port.
linkcheck: gap
	@awk '{printf "void %s(void) {}\n", $$1}' $(BUILD_DIR)/missing.txt \
	    | grep -v _GLOBAL_OFFSET_TABLE_ > $(BUILD_DIR)/stubs.c
	@# main() is NOT part of the portable core: upstream defines it in the
	@# PLATFORM file (Platform_Posix.c / Platform_Windows.c), so it never appears
	@# in the undefined-symbol list above (nothing in the tree references it,
	@# only crt0 does) and the link fails at the very end with a bare
	@# "undefined reference to `main'". Platform_Maytera.c must define it.
	@echo 'int main(int argc, char** argv) { (void)argc; (void)argv; return 0; }' >> $(BUILD_DIR)/stubs.c
	@$(CC) $(CFLAGS) -c $(BUILD_DIR)/stubs.c -o $(BUILD_DIR)/stubs.o
	@$(LD) $(LDFLAGS) -o $(BUILD_DIR)/$(TARGET).linkcheck $(CRT0) $(ALL_OBJS) \
	    $(BUILD_DIR)/stubs.o $(GL_LIB) $(LIBC)
	@echo "=== linkcheck ELF shape ==="
	@readelf -h $(BUILD_DIR)/$(TARGET).linkcheck | awk '/Type:|Machine:|Entry/'
	@readelf -lW $(BUILD_DIR)/$(TARGET).linkcheck | awk '/LOAD|DYNAMIC|INTERP/'
	@ls -l $(BUILD_DIR)/$(TARGET).linkcheck

clean:
	rm -rf $(BUILD_DIR) $(TARGET)
