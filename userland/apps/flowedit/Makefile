# Makefile for flowedit - Maytera Flow VISUAL EDITOR (Milestone 1).
# Installs as /APPS/FLOW. Reuses the runner's node+edge model + YAML parser by
# compiling userland/apps/flowrun/flow.c straight into this binary (one model,
# not a second one), so it also links libyaml like the runner does.
#
# CFLAGS/LDFLAGS copied from userland/apps/flowrun/Makefile (which itself copied
# apps/files) plus the libyaml port's public header on the include path.

CC = gcc
LD = ld

# Paths
LIBC_DIR = ../../libc
USERLAND_DIR = ../..
PORTS_DIR = ../../ports/out
FLOWRUN_DIR = ../flowrun

# Compiler flags
CFLAGS = -ffreestanding -fstack-protector-strong -mstack-protector-guard=global -fPIE -mno-red-zone \
         -nostdlib -nostdinc -fno-builtin -Wall -Wextra -O2 -g \
         -I$(LIBC_DIR) -I$(FLOWRUN_DIR) -I$(PORTS_DIR)/include \
         -isystem /usr/lib/gcc/x86_64-linux-gnu/12/include

# Linker flags
LDFLAGS = -pie --no-dynamic-linker -z notext -nostdlib -z max-page-size=0x1000 -T $(USERLAND_DIR)/user-pie.ld

# Output (uppercase to match the /APPS install name; declared in
# build/golden-new-apps.list as "FLOW !near-miss-ok" - FLOW is a prefix of the
# existing FLOWRUN, which is the deliberate IP/IP6-style waiver case).
TARGET = FLOW

# Objects. flow.o is the reused runner core.
OBJS = main.o flow.o flowfeed.o

# Libraries
CRT0 = $(LIBC_DIR)/crt0.o
LIBC = $(LIBC_DIR)/libc.a
LIBYAML = $(PORTS_DIR)/lib/libyaml.a

all: $(TARGET)

# BUILD the libyaml port, do not assume it is there. userland/ports/out/ is
# gitignored build output, so a clean checkout has neither
# $(PORTS_DIR)/include/yaml.h nor $(PORTS_DIR)/lib/libyaml.a. Without this rule
# the FIRST COMPILE fails with "yaml.h: No such file or directory", which reads
# like a missing system header rather than a port nobody built, and this app is
# then simply ABSENT from the image. The only thing in the tree that built
# libyaml was userland/apps/yamltest, a TEST app that build.sh reaches
# alphabetically after every shipping consumer. Same rule
# userland/apps/zstdtest/Makefile already uses for libzstd.
PORTS_SRC = ../../ports
$(LIBYAML):
	@echo "$(TARGET): libyaml.a not built yet; running the ports driver"
	bash $(PORTS_SRC)/mports.sh build libyaml


# libyaml before libc so its malloc/memcpy references resolve out of libc.a.
$(TARGET): $(OBJS) $(CRT0) $(LIBC) $(LIBYAML)
	$(LD) $(LDFLAGS) -o $@ $(CRT0) $(OBJS) $(LIBYAML) $(LIBC)
	@echo "Built $(TARGET)"
	@ls -l $(TARGET)

# The bundled example catalog, GENERATED from the runner's examples/ directory so
# there is one source of truth (see mkseeds.sh for the drift this ended). Not
# committed: it is build output, and a committed generated file is just the second
# copy again.
seeds.h: mkseeds.sh $(wildcard $(FLOWRUN_DIR)/examples/*.yml)
	./mkseeds.sh $(FLOWRUN_DIR)/examples seeds.h

main.o: main.c flowfeed.h seeds.h | $(LIBYAML)
	$(CC) $(CFLAGS) -c $< -o $@

flowfeed.o: flowfeed.c flowfeed.h | $(LIBYAML)
	$(CC) $(CFLAGS) -c flowfeed.c -o flowfeed.o

# The reused runner core, compiled here from its single source of truth.
flow.o: $(FLOWRUN_DIR)/flow.c $(FLOWRUN_DIR)/flow.h | $(LIBYAML)
	$(CC) $(CFLAGS) -c $(FLOWRUN_DIR)/flow.c -o flow.o

# Network-independent HOST unit test of the feed client's parse + integrity
# + validate-before-install logic (see feedtest.c). Each TU is compiled with
# its own include set so flow.c's host build is byte-for-byte the flowrun one.
HOSTCC = gcc
SYS_YAML = /lib/x86_64-linux-gnu/libyaml-0.so.2
#
# PREREQUISITE: libyaml-dev (the HOST yaml.h). This is a HOST test and it links
# the HOST libyaml above, so the host header is the matching one.
#
# It previously compiled flow.c with -I$(PORTS_DIR)/include, the CROSS-port
# include dir, while linking the host .so. That include dir does not and will not
# hold yaml.h: the libyaml port fetches upstream and installs no header there, and
# nothing in the tree vendors one. So the rule could only ever work by accident,
# on a machine that happened to have libyaml-dev, and it failed in a clean
# container with a confusing "yaml.h: No such file or directory" that read like a
# missing port rather than a missing host package. Cross include path removed.
#
# secure_zero LIVES IN libc/string.c, and sha256.c calls it. That TU was missing
# from the link, so once the compile was fixed the test still failed with
# "undefined reference to secure_zero". It is linked in below rather than given a
# local definition: a shared primitive gets reused, never re-implemented per
# consumer, and secure_zero in particular must not be re-written casually because
# its whole purpose is to not be optimised away.
feedtest: flowfeed.c feedtest.c flowfeed.h $(FLOWRUN_DIR)/flow.c $(FLOWRUN_DIR)/flow.h $(LIBC_DIR)/sha256.c $(LIBC_DIR)/string.c
	@printf '%s\n' '#include <yaml.h>' | $(HOSTCC) -fsyntax-only -x c - 2>/dev/null \
	  || { echo "feedtest: host yaml.h not found. Install libyaml-dev (this is a HOST test and links the host libyaml)." >&2; exit 2; }
	$(HOSTCC) -DFLOWFEED_HOSTTEST -O2 -Wall -Wextra -g -I. -I$(FLOWRUN_DIR) -I$(LIBC_DIR) -c feedtest.c -o ht_feedtest.o
	$(HOSTCC) -DFLOWFEED_HOSTTEST -O2 -Wall -Wextra -g -I. -c flowfeed.c -o ht_flowfeed.o
	$(HOSTCC) -O2 -I$(LIBC_DIR) -c $(LIBC_DIR)/sha256.c -o ht_sha256.o
	$(HOSTCC) -O2 -I$(LIBC_DIR) -c $(LIBC_DIR)/string.c -o ht_string.o
	# string.c calls memset_fast/memmove_fast, which live in NASM
	# (libc/memcpy_fast.asm), so a host link of string.c is otherwise undefined.
	# userland/libc/tests/run_strncpy.sh solves this with STUBS in the test. The
	# real thing is better and costs one nasm call: the asm is plain x86-64 SysV
	# and assembles to elf64 unchanged, so the host test exercises the SAME
	# implementation the OS ships rather than a stand-in that could diverge.
	nasm -f elf64 $(LIBC_DIR)/memcpy_fast.asm -o ht_memcpy_fast.o
	$(HOSTCC) -O2 -I$(FLOWRUN_DIR) -c $(FLOWRUN_DIR)/flow.c -o ht_flow.o
	$(HOSTCC) -o feedtest ht_feedtest.o ht_flowfeed.o ht_sha256.o ht_string.o ht_memcpy_fast.o ht_flow.o $(SYS_YAML)
	@echo "Built feedtest"

clean:
	rm -f $(OBJS) $(TARGET) feedtest ht_*.o seeds.h

.PHONY: all clean feedtest
