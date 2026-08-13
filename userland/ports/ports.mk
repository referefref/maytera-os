# userland/ports/ports.mk - THE single definition of the MayteraOS userland
# cross-build flag set, for mports recipes and for every app that links one.
#
# WHY THIS FILE EXISTS. Before it, the flag line
#
#   -ffreestanding -fstack-protector-strong -mstack-protector-guard=global
#   -fPIE -mno-red-zone -nostdlib -nostdinc -fno-builtin -Wall -Wextra -O2 -g
#   -I../../libc -isystem /usr/lib/gcc/x86_64-linux-gnu/12/include
#
# was copy-pasted into ~145 app Makefiles. Every copy is a place the set can
# drift, and docs/PORTABILITY_HOMEBREW_SNAPCRAFT_ASSESSMENT.md section 8.2 names
# that duplication as one of the reasons a ports system is needed at all
# ("each port carries a bespoke Makefile with the -fPIE ... -T ../../user-pie.ld
# incantation copy-pasted. That should be one ports.mk include").
#
# This file does NOT retro-fit the existing apps: rewriting 145 Makefiles in the
# same pass that introduces the mechanism would make both changes unreviewable.
# It is the definition NEW ports and NEW port-consuming apps use, and the place
# an existing app can be migrated to one at a time.
#
# ONE DEFINITION, TWO CONSUMERS. mports.sh (shell) does not re-spell these flags;
# it asks make for them:
#     make -s -f ports.mk print-MPORTS_CFLAGS
# so the shell driver and every Makefile are guaranteed to be compiling with the
# same thing. If you change a flag, change it here and nowhere else.

# Directory of THIS file. ABSOLUTE on purpose: mports.sh compiles each upstream
# translation unit with `cd <unpacked srcdir>` so that upstream's own relative
# includes resolve, and a relative -I../../libc would then point somewhere
# inside the tarball. $(abspath) is the difference between "it built" and "it
# built against the wrong headers".
MPORTS_MK_DIR := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
USERLAND_DIR  ?= $(abspath $(MPORTS_MK_DIR)/..)
LIBC_DIR      ?= $(abspath $(USERLAND_DIR)/libc)

# Where mports installs headers and static libraries. Build output, gitignored.
MPORTS_OUT ?= $(MPORTS_MK_DIR)/out

# The compile profile. Identical to userland/apps/hello/Makefile (the minimal
# non-GUI app template) plus nothing. Userland is REAL SSE2 hardware float:
# -mno-sse is a KERNEL-only flag and must never appear here.
MPORTS_CFLAGS = -ffreestanding \
                -fstack-protector-strong -mstack-protector-guard=global \
                -fPIE -mno-red-zone \
                -nostdlib -nostdinc -fno-builtin \
                -Wall -Wextra -O2 -g \
                -I$(LIBC_DIR) \
                -isystem /usr/lib/gcc/x86_64-linux-gnu/12/include

# What a consumer adds to reach a port's installed headers and libraries.
MPORTS_INCLUDE = -I$(MPORTS_OUT)/include
MPORTS_LIBDIR  = $(MPORTS_OUT)/lib

# The Ring 3 PIE link profile.
MPORTS_LDFLAGS = -pie --no-dynamic-linker -z notext -nostdlib \
                 -z max-page-size=0x1000 -T $(USERLAND_DIR)/user-pie.ld

# Query hook used by mports.sh so the shell never re-spells a flag.
print-%: ; @echo '$($*)'
.PHONY: print-%
