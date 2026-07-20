# Makefile for libhelp - MayteraOS help subsystem static library (task #267)
# Freestanding build with the same flags userland apps use.

CC = gcc
AR = ar

LIBC_DIR = ../libc

CFLAGS = -ffreestanding -fno-builtin -nostdinc -nostdlib -fno-stack-protector \
         -mno-red-zone -O2 -Wall -Wextra \
         -isystem /usr/lib/gcc/x86_64-linux-gnu/12/include \
         -I$(LIBC_DIR)

# Parser/model objects (freestanding-clean, no GUI deps) plus the GUI helpers.
OBJS = help_doc.o mhlp.o hlp.o help_open.o help_search.o help_ui.o

TARGET = libhelp.a

all: $(TARGET)

$(TARGET): $(OBJS)
	$(AR) rcs $@ $(OBJS)
	@echo "Built $(TARGET)"
	@ls -l $(TARGET)

%.o: %.c help.h help_internal.h help_ui.h
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(TARGET)

.PHONY: all clean
