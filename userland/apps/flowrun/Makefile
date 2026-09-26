# Makefile for flowrun - Maytera Flow workflow runner (Milestone 1)

CC = gcc
LD = ld

# Paths
LIBC_DIR = ../../libc
USERLAND_DIR = ../..
PORTS_DIR = ../../ports/out

# Compiler flags (copied from userland/apps/files/Makefile, plus the libyaml
# port's public header on the include path).
CFLAGS = -ffreestanding -fstack-protector-strong -mstack-protector-guard=global -fPIE -mno-red-zone \
         -nostdlib -nostdinc -fno-builtin -Wall -Wextra -O2 -g \
         -I$(LIBC_DIR) -I$(PORTS_DIR)/include -isystem /usr/lib/gcc/x86_64-linux-gnu/12/include

# Linker flags
LDFLAGS = -pie --no-dynamic-linker -z notext -nostdlib -z max-page-size=0x1000 -T $(USERLAND_DIR)/user-pie.ld

# Output
TARGET = FLOWRUN

# Objects
OBJS = main.o flow.o

# Libraries
CRT0 = $(LIBC_DIR)/crt0.o
LIBC = $(LIBC_DIR)/libc.a
LIBYAML = $(PORTS_DIR)/lib/libyaml.a

# Default target
all: $(TARGET)

# Link the executable. libyaml before libc so its malloc/memcpy references
# resolve out of libc.a.
$(TARGET): $(OBJS) $(CRT0) $(LIBC) $(LIBYAML)
	$(LD) $(LDFLAGS) -o $@ $(CRT0) $(OBJS) $(LIBYAML) $(LIBC)
	@echo "Built $(TARGET)"
	@ls -l $(TARGET)

# Compile C source
%.o: %.c flow.h
	$(CC) $(CFLAGS) -c $< -o $@

# Network-independent HOST unit test of the parser + marshaller + executor.
# Uses the SYSTEM gcc + the system libyaml runtime (the port's header is API
# compatible), NOT the freestanding build. See hosttest.c.
HOSTCC = gcc
SYS_YAML = /lib/x86_64-linux-gnu/libyaml-0.so.2
hosttest: flow.c hosttest.c flow.h
	$(HOSTCC) -DFLOWRUN_HOSTTEST -O2 -Wall -Wextra -g -I$(PORTS_DIR)/include \
	    -o hosttest hosttest.c flow.c $(SYS_YAML)
	@echo "Built hosttest"

# Clean
clean:
	rm -f $(OBJS) $(TARGET) hosttest

.PHONY: all clean hosttest
