# Makefile for curaslice (CuraEngine 15.04 legacy, ported to MayteraOS userland).
#
# This is the FIRST C++ app in the userland tree. It keeps the libstdc++ HEADER
# search path (vector/algorithm/map are header-only templates) but links NONE of
# the libstdc++ runtime: operator new/delete and the C++ ABI stubs come from
# cxxsupp.cpp, everything else from libc.a. See CURAENGINE_PORT_PLAN.md
# section 3 and BUILD-NOTES.md.
#
# Do NOT pass -nostdinc or -nostdinc++: we WANT the toolchain's C++ headers.
# -I../../libc is placed first so <stdio.h>/<string.h>/<math.h> resolve to the
# MayteraOS libc, not glibc. -nostdlib applies at LINK time only.

CXX = g++
LD  = ld

LIBC_DIR     = ../../libc
USERLAND_DIR = ../..
BUILD_DIR    = build

# Plan section 3.1 flags. -mcmodel=large is mandatory: user.ld's 0x80000000 base
# is negative as a signed i32, so absolute 32-bit refs to large statics/rodata
# truncate without it (blame #444). -fno-threadsafe-statics drops __cxa_guard_*.
# Header strategy (PORT-STATUS.md): compile against the HOST toolchain headers
# (glibc prototypes + libstdc++ templates); the -nostdlib link against the
# MayteraOS libc.a resolves the real symbols. Do NOT add -I$(LIBC_DIR) here: it
# would shadow the host <stdio.h> family and conflict with libstdc++'s
# include_next chain. -D_FORTIFY_SOURCE=0 keeps glibc from routing printf/mem*
# through __*_chk symbols our libc does not provide.
CXXFLAGS = -std=gnu++11 -fno-exceptions -fno-rtti \
           -fno-threadsafe-statics -fno-stack-protector -fno-pic -mno-red-zone \
           -mcmodel=large -fno-builtin -O2 -g -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0 \
           -Ilibs -Isrc

# -nostdlib: no crt/libc/libstdc++ auto-link. -T user.ld is REQUIRED or the ELF
# loads at ld's default 0x400000 and the loader page-faults on spawn.
LDFLAGS = -nostdlib -z max-page-size=0x1000 -T $(USERLAND_DIR)/user.ld

TARGET = curaslice

# VPATH lets a single pattern rule find sources across the vendored subdirs.
# All basenames are unique, so objects flatten into build/ without collision.
VPATH = src:src/modelFile:src/utils:libs/clipper:.

OBJS = \
    $(BUILD_DIR)/cxxsupp.o \
    $(BUILD_DIR)/main.o \
    $(BUILD_DIR)/settings.o \
    $(BUILD_DIR)/slicer.o \
    $(BUILD_DIR)/layerPart.o \
    $(BUILD_DIR)/inset.o \
    $(BUILD_DIR)/skin.o \
    $(BUILD_DIR)/infill.o \
    $(BUILD_DIR)/skirt.o \
    $(BUILD_DIR)/raft.o \
    $(BUILD_DIR)/support.o \
    $(BUILD_DIR)/bridge.o \
    $(BUILD_DIR)/comb.o \
    $(BUILD_DIR)/pathOrderOptimizer.o \
    $(BUILD_DIR)/polygonOptimizer.o \
    $(BUILD_DIR)/optimizedModel.o \
    $(BUILD_DIR)/gcodeExport.o \
    $(BUILD_DIR)/timeEstimate.o \
    $(BUILD_DIR)/modelFile.o \
    $(BUILD_DIR)/gettime.o \
    $(BUILD_DIR)/logoutput.o \
    $(BUILD_DIR)/socket.o \
    $(BUILD_DIR)/clipper.o

CRT0 = $(LIBC_DIR)/crt0.o
LIBC = $(LIBC_DIR)/libc.a

# libstdc++ red-black-tree helpers (std::map / std::set out-of-line ops:
# _Rb_tree_increment/decrement/insert_and_rebalance/rebalance_for_erase). These
# operate only on the POD _Rb_tree_node_base, so the single object links cleanly
# with no other libstdc++ runtime. Extracted from the host libstdc++.a.
STDCXX_A := $(shell $(CXX) -print-file-name=libstdc++.a)
TREE_O = $(BUILD_DIR)/tree.o

all: $(BUILD_DIR) $(TARGET)

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(TREE_O): | $(BUILD_DIR)
	cd $(BUILD_DIR) && ar x $(STDCXX_A) tree.o

$(TARGET): $(OBJS) $(TREE_O) $(CRT0) $(LIBC)
	$(LD) $(LDFLAGS) -o $@ $(CRT0) $(OBJS) $(TREE_O) $(LIBC)
	@echo "Built $(TARGET) (CuraEngine 15.04 on MayteraOS)"

$(BUILD_DIR)/%.o: %.cpp | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -c $< -o $@

clean:
	rm -rf $(BUILD_DIR) $(TARGET)

.PHONY: all clean
