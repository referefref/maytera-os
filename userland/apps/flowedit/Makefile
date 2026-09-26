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

# libyaml before libc so its malloc/memcpy references resolve out of libc.a.
$(TARGET): $(OBJS) $(CRT0) $(LIBC) $(LIBYAML)
	$(LD) $(LDFLAGS) -o $@ $(CRT0) $(OBJS) $(LIBYAML) $(LIBC)
	@echo "Built $(TARGET)"
	@ls -l $(TARGET)

main.o: main.c flowfeed.h
	$(CC) $(CFLAGS) -c $< -o $@

flowfeed.o: flowfeed.c flowfeed.h
	$(CC) $(CFLAGS) -c flowfeed.c -o flowfeed.o

# The reused runner core, compiled here from its single source of truth.
flow.o: $(FLOWRUN_DIR)/flow.c $(FLOWRUN_DIR)/flow.h
	$(CC) $(CFLAGS) -c $(FLOWRUN_DIR)/flow.c -o flow.o

# Network-independent HOST unit test of the feed client's parse + integrity
# + validate-before-install logic (see feedtest.c). Each TU is compiled with
# its own include set so flow.c's host build is byte-for-byte the flowrun one.
HOSTCC = gcc
SYS_YAML = /lib/x86_64-linux-gnu/libyaml-0.so.2
feedtest: flowfeed.c feedtest.c flowfeed.h $(FLOWRUN_DIR)/flow.c $(FLOWRUN_DIR)/flow.h $(LIBC_DIR)/sha256.c
	$(HOSTCC) -DFLOWFEED_HOSTTEST -O2 -Wall -Wextra -g -I. -I$(FLOWRUN_DIR) -I$(LIBC_DIR) -c feedtest.c -o ht_feedtest.o
	$(HOSTCC) -DFLOWFEED_HOSTTEST -O2 -Wall -Wextra -g -I. -c flowfeed.c -o ht_flowfeed.o
	$(HOSTCC) -O2 -I$(LIBC_DIR) -c $(LIBC_DIR)/sha256.c -o ht_sha256.o
	$(HOSTCC) -O2 -I$(FLOWRUN_DIR) -I$(PORTS_DIR)/include -c $(FLOWRUN_DIR)/flow.c -o ht_flow.o
	$(HOSTCC) -o feedtest ht_feedtest.o ht_flowfeed.o ht_sha256.o ht_flow.o $(SYS_YAML)
	@echo "Built feedtest"

clean:
	rm -f $(OBJS) $(TARGET) feedtest ht_*.o

.PHONY: all clean feedtest
