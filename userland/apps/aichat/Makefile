# Makefile for aichat - MayteraOS LLM chat widget (#185)
#
# flowgen: aichat also authors Maytera Flow workflows (the /flow command). It reuses
# the runner's node model + YAML parser by compiling userland/apps/flowrun/flow.c
# straight into this binary (one parser, not a second one), exactly as the
# /APPS/FLOW editor does, so it also links libyaml. flowgen.c is the generation
# logic (prompt building, LLM-reply extraction, validate-before-save).

CC = gcc
LD = ld

LIBC_DIR = ../../libc
USERLAND_DIR = ../..
PORTS_DIR = ../../ports/out
FLOWRUN_DIR = ../flowrun

CFLAGS = -ffreestanding -fstack-protector-strong -mstack-protector-guard=global -fPIE -mno-red-zone \
         -nostdlib -nostdinc -fno-builtin -Wall -Wextra -O2 -g \
         -I$(LIBC_DIR) -I$(FLOWRUN_DIR) -I$(PORTS_DIR)/include \
         -isystem /usr/lib/gcc/x86_64-linux-gnu/12/include

LDFLAGS = -pie --no-dynamic-linker -z notext -nostdlib -z max-page-size=0x1000 -T ../../user-pie.ld

TARGET = aichat
# flow.o is the reused runner core (parser + model), compiled from its single
# source of truth; flowgen.o is the chat-driven generation logic.
OBJS = main.o conv.o flowgen.o flow.o

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

main.o: main.c conv.h flowgen.h $(FLOWRUN_DIR)/flow.h | $(LIBYAML)
	$(CC) $(CFLAGS) -c main.c -o main.o

conv.o: conv.c conv.h | $(LIBYAML)
	$(CC) $(CFLAGS) -c conv.c -o conv.o

flowgen.o: flowgen.c flowgen.h $(FLOWRUN_DIR)/flow.h | $(LIBYAML)
	$(CC) $(CFLAGS) -c flowgen.c -o flowgen.o

# The reused runner core, compiled here from its single source of truth.
flow.o: $(FLOWRUN_DIR)/flow.c $(FLOWRUN_DIR)/flow.h | $(LIBYAML)
	$(CC) $(CFLAGS) -c $(FLOWRUN_DIR)/flow.c -o flow.o

# Network-independent HOST unit test of the workflow-generation VALIDATION path
# (extraction + flow_parse_bytes gate). Uses the SYSTEM gcc + system libyaml, NOT
# the freestanding build. See flowgen_hosttest.c. Mirrors flowrun's hosttest.
HOSTCC = gcc
SYS_YAML = /lib/x86_64-linux-gnu/libyaml-0.so.2
flowgen-hosttest: flowgen.c flowgen_hosttest.c flowgen.h $(FLOWRUN_DIR)/flow.c $(FLOWRUN_DIR)/flow.h
	$(HOSTCC) -DFLOWRUN_HOSTTEST -O2 -Wall -Wextra -g \
	    -I$(FLOWRUN_DIR) -I$(PORTS_DIR)/include \
	    -o flowgen-hosttest flowgen_hosttest.c flowgen.c $(FLOWRUN_DIR)/flow.c $(SYS_YAML)
	@echo "Built flowgen-hosttest"

clean:
	rm -f $(OBJS) $(TARGET) flowgen-hosttest

.PHONY: all clean flowgen-hosttest
