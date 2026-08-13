# Makefile for install (#306) - MayteraOS "Install to Disk" user-space app.
#
# TARGET is uppercase INSTALL on purpose. build-golden.sh's app-staging loop
# (build/build-golden.sh, "for appdir in /tmp/appstage/*/") resolves the
# on-image /APPS name by matching the built binary's own filename against
# whatever already exists on the asset base; when there is no existing entry
# (true here - this app has never shipped before) it falls back to the built
# binary's exact name. Naming the link output INSTALL makes the standard,
# already-automatic pipeline land it at /APPS/INSTALL with no changes to
# build-golden.sh at all.

CC = gcc
LD = ld

LIBC_DIR = ../../libc
USERLAND_DIR = ../..

CFLAGS = -ffreestanding -fstack-protector-strong -mstack-protector-guard=global -fPIE -mno-red-zone \
         -nostdlib -nostdinc -fno-builtin -Wall -Wextra -O2 -g \
         -I$(LIBC_DIR) -isystem /usr/lib/gcc/x86_64-linux-gnu/12/include

LDFLAGS = -pie --no-dynamic-linker -z notext -nostdlib -z max-page-size=0x1000 -T $(USERLAND_DIR)/user-pie.ld

TARGET = INSTALL

OBJS = main.o

CRT0 = $(LIBC_DIR)/crt0.o
LIBC = $(LIBC_DIR)/libc.a

all: $(TARGET)

$(TARGET): $(OBJS) $(CRT0) $(LIBC)
	$(LD) $(LDFLAGS) -o $@ $(CRT0) $(OBJS) $(LIBC)
	@echo "Built $(TARGET)"
	@ls -l $(TARGET)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(TARGET)

.PHONY: all clean
