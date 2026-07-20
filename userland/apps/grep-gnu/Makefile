# Makefile for the genuine GNU grep 2.5.4 port to MayteraOS.
# Builds grep.c search.c kwset.c dfa.c (the real grep binary; egrep/fgrep
# behaviour is selected at runtime from argv[0]) plus grep's bundled gnulib
# (lib/) and a small MayteraOS compatibility layer (compat/).

CC = gcc
LD = ld

LIBC_DIR     = ../../libc
USERLAND_DIR = ../..
GCC_INC      = /usr/lib/gcc/x86_64-linux-gnu/12/include

# Legacy third-party code: relax warnings (like the DOOM port). Use malloc-based
# regex, no strict aliasing (the engines type-pun heavily).
# -mcmodel=large: the user image is linked at 0x80000000 (2GB).  Absolute
# 32-bit-signed relocations (R_X86_64_32S) cannot represent addresses >= 2GB,
# so the large model (64-bit absolute relocations) is required, matching the
# other MayteraOS apps (cat, tail, msh).
#
# -O0: the bundled matcher engines (dfa.c, regex.c, kwset.c, search.c) are
# 1990s code that contains constructs which are technically undefined behaviour
# (most notably cwexec()'s 'accept' register, initialised only under #ifdef
# lint, and various type-punned size computations).  At -O2 gcc 12 exploits
# that UB and miscompiles them: cwexec() leaves 'accept' NULL (NULL deref on
# grep -o) and dfa.c's state allocation runs away (calloc overruns the heap on
# grep -E / -w / anchors).  Building the whole port at -O0 keeps the legacy
# engines correct; grep's data sets are tiny so the speed cost is irrelevant.
CFLAGS = -ffreestanding -fno-stack-protector -fno-pic -mno-red-zone \
         -nostdlib -nostdinc -fno-builtin -fno-strict-aliasing -mcmodel=large \
         -O0 -g -w -DHAVE_CONFIG_H \
         -Icompat -I$(LIBC_DIR) -Isrc -Ilib \
         -isystem $(GCC_INC)

# --allow-multiple-definition: compat/shim.o intentionally redefines exit()/
# atexit() to take precedence over the libc.a copies (it flushes stdio on the
# way out).  shim.o is listed before libc.a, so ld keeps shim's definitions.
LDFLAGS = -nostdlib -z max-page-size=0x1000 --allow-multiple-definition \
          -T $(USERLAND_DIR)/user.ld

TARGET = grep

CRT0 = $(LIBC_DIR)/crt0.o
LIBC = $(LIBC_DIR)/libc.a

SRC_OBJS = src/grep.o src/search.o src/kwset.o src/dfa.o

LIB_OBJS = lib/regex.o lib/fnmatch.o lib/getopt.o lib/getopt1.o \
           lib/error.o lib/obstack.o lib/exclude.o lib/savedir.o \
           lib/quotearg.o lib/hard-locale.o lib/isdir.o lib/xmalloc.o \
           lib/xstrtoumax.o lib/strtol.o lib/strtoul.o lib/strtoull.o \
           lib/strtoumax.o

COMPAT_OBJS = compat/ctype.o compat/shim.o

OBJS = $(COMPAT_OBJS) $(SRC_OBJS) $(LIB_OBJS)

all: $(TARGET)

$(TARGET): $(OBJS) $(CRT0) $(LIBC)
	$(LD) $(LDFLAGS) -o $@ $(CRT0) $(OBJS) $(LIBC)
	@echo "Built $(TARGET) (GNU grep 2.5.4 port)"

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(TARGET)

.PHONY: all clean
