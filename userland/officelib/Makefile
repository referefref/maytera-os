# officelib - shared static library for the MayteraOS office suite.
# Builds libmoffice.a from every module subdir. Linked by apps/{writer,sheets,slides}.
# See docs/OFFICE_SUITE_ARCHITECTURE.md.
CC = gcc
AR = ar
LIBC_DIR = ../libc
ARC_DIR = ../libarchive
CFLAGS = -ffreestanding -fstack-protector-strong -mstack-protector-guard=global -fPIE -mno-red-zone \
         -nostdlib -nostdinc -fno-builtin -Wall -Wextra -O2 -g \
         -Iinclude -I$(LIBC_DIR) -I$(ARC_DIR) -isystem /usr/lib/gcc/x86_64-linux-gnu/12/include

SRCS = $(wildcard opc/*.c oxml/*.c model/*.c fmt_docx/*.c fmt_odt/*.c \
        fmt_xlsx_ods/*.c fmt_pptx_odp/*.c fmt_ole2/*.c layout/*.c officeui/*.c)
# opc/ wraps the canonical in-memory zip (userland/libarchive/arc.c). Fold arc.o
# into libmoffice.a so apps linking the office library get the zip primitive
# without a separate object (writer/sheets/slides link only libmoffice.a + libc.a).
OBJS = $(SRCS:.c=.o) $(ARC_DIR)/arc.o
TARGET = libmoffice.a

all: $(TARGET)
$(TARGET): $(OBJS)
	$(AR) rcs $@ $(OBJS)
	@ls -l $@
$(ARC_DIR)/arc.o: $(ARC_DIR)/arc.c $(ARC_DIR)/arc.h $(ARC_DIR)/arc_port.h
	$(CC) $(CFLAGS) -c $< -o $@
%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@
clean:
	rm -f $(OBJS) $(TARGET)
.PHONY: all clean
