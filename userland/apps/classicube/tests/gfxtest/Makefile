# Offscreen ClassiCube graphics harness (#28, graphics lane).
#
# -DMOS_CC_STANDALONE_GFXTEST is what lets this test build against a PRISTINE
# vendor/ tree with no engine patches applied. It makes gfx/cc_maytera_config.h
# define CC_BUILD_MANUAL and take over platform detection. The REAL APP BUILD
# MUST NOT DEFINE IT: CC_BUILD_MANUAL makes Core.h:189 skip its auto-detect
# block, which would silently discard the platform lane's PLAT_MAYTERA branch
# and every setting in it. See the header comment in cc_maytera_config.h.
#
#   make host   - build + run on the build server, dumps BMPs, prints timings.
#                 This is the PRIMARY EVIDENCE path: no window, no compositor,
#                 no VM.
#   make mos    - cross-check that the SAME rasteriser sources compile and link
#                 under the real MayteraOS userland flags (see ../../../../
#                 apps/arena/Makefile for the reference flag set). Produces an
#                 ELF; it is not run here.
#
# This Makefile builds ONLY the graphics lane's own test. It does not build the
# ClassiCube app; that belongs to the app lane.

VENDOR   = ../../vendor/ClassiCube/src
GFX      = ../../gfx
CONFIG   = $(GFX)/cc_maytera_config.h

# The rasteriser and the maths it needs, all upstream and unmodified.
CC_SRCS  = $(VENDOR)/Graphics_SoftGPU.c $(VENDOR)/ExtMath.c $(VENDOR)/Vectors.c
# Our portable harness.
GT_SRCS  = gfxtest_scene.c gfxtest_support.c

OUT      = out

# ---------------------------------------------------------------- host ----
HOST_CC      = gcc
HOST_CFLAGS  = -O2 -g -std=gnu99 -Wall -I$(VENDOR) -DMOS_CC_STANDALONE_GFXTEST -include $(CONFIG)
HOST_OBJS    = $(addprefix $(OUT)/host_,$(notdir $(CC_SRCS:.c=.o))) \
               $(addprefix $(OUT)/host_,$(GT_SRCS:.c=.o)) $(OUT)/host_main_host.o

$(OUT):
	@mkdir -p $(OUT)

$(OUT)/host_%.o: $(VENDOR)/%.c | $(OUT)
	$(HOST_CC) $(HOST_CFLAGS) -c $< -o $@

$(OUT)/host_%.o: %.c | $(OUT)
	$(HOST_CC) $(HOST_CFLAGS) -c $< -o $@

$(OUT)/gfxtest_host: $(HOST_OBJS)
	$(HOST_CC) $(HOST_OBJS) -o $@ -lm

host: $(OUT)/gfxtest_host
	@echo
	./$(OUT)/gfxtest_host $(OUT)

# ----------------------------------------------------------- MayteraOS ----
# Flags copied from userland/apps/arena/Makefile, the shipping TinyGL app, so
# this proves the rasteriser survives the real userland compilation model:
# freestanding, no stdinc, no red zone, stack protector, PIE at the user.ld
# base. -msse -msse2 is what makes hardware float legal in Ring 3; the
# soft-float/-mno-sse restriction is a KERNEL constraint and does not apply
# to userland apps. Verified by reading that Makefile, not assumed.
MOS_LIBC   = ../../../../libc
MOS_CC     = gcc
MOS_CFLAGS = -fPIE -ffreestanding -fstack-protector-strong \
             -mstack-protector-guard=global -mno-red-zone -msse -msse2 \
             -nostdlib -nostdinc -fno-builtin -O2 -g \
             -I$(VENDOR) -I$(MOS_LIBC) -DMOS_CC_STANDALONE_GFXTEST -include $(CONFIG) \
             -isystem /usr/lib/gcc/x86_64-linux-gnu/12/include
MOS_LDFLAGS = -pie --no-dynamic-linker -z notext -nostdlib \
              -z max-page-size=0x1000 -T ../../../../user-pie.ld

MOS_OBJS = $(addprefix $(OUT)/mos_,$(notdir $(CC_SRCS:.c=.o))) \
           $(addprefix $(OUT)/mos_,$(GT_SRCS:.c=.o)) $(OUT)/mos_main_maytera.o

$(OUT)/mos_%.o: $(VENDOR)/%.c | $(OUT)
	$(MOS_CC) $(MOS_CFLAGS) -c $< -o $@

$(OUT)/mos_%.o: %.c | $(OUT)
	$(MOS_CC) $(MOS_CFLAGS) -c $< -o $@

$(OUT)/CCGFXTST: $(MOS_OBJS)
	ld $(MOS_LDFLAGS) $(MOS_LIBC)/crt0.o $(MOS_OBJS) $(MOS_LIBC)/libc.a -o $@

mos: $(OUT)/CCGFXTST
	@echo "MayteraOS harness ELF:"; ls -la $(OUT)/CCGFXTST

clean:
	rm -rf $(OUT)

.PHONY: host mos clean
