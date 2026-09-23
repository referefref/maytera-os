# Makefile for OTKDEMO - the office UI toolkit example app (NOT shipped; built
# by hand to exercise the toolkit on a throwaway VM). Mirrors apps/writer/Makefile.
#   make                      # plain demo
#   make AUTOMODAL=5          # verification build: opens the Paragraph modal after 5 ticks
CC = gcc
LD = ld
LIBC_DIR = ../../../libc
OFFICELIB = ../..
USERLAND_DIR = ../../..
CFLAGS = -ffreestanding -fstack-protector-strong -mstack-protector-guard=global -fPIE -mno-red-zone \
         -nostdlib -nostdinc -fno-builtin -Wall -Wextra -O2 -g \
         -I$(LIBC_DIR) -I$(OFFICELIB)/include -isystem /usr/lib/gcc/x86_64-linux-gnu/12/include
ifdef AUTOMODAL
CFLAGS += -DOTKDEMO_AUTOMODAL=$(AUTOMODAL)
endif
ifdef NARROW
CFLAGS += -DOTKDEMO_NARROW=1
endif
LDFLAGS = -pie --no-dynamic-linker -z notext -nostdlib -z max-page-size=0x1000 -T $(USERLAND_DIR)/user-pie.ld
TARGET = OTKDEMO
OBJS = otkdemo.o
CRT0 = $(LIBC_DIR)/crt0.o
LIBC = $(LIBC_DIR)/libc.a
LIBOFFICE = $(OFFICELIB)/libmoffice.a
all: $(TARGET)
$(LIBOFFICE):
	$(MAKE) -C $(OFFICELIB)
$(TARGET): $(OBJS) $(CRT0) $(LIBOFFICE) $(LIBC)
	$(LD) $(LDFLAGS) -o $@ $(CRT0) $(OBJS) $(LIBOFFICE) $(LIBC)
	@ls -l $(TARGET)
%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@
clean:
	rm -f $(OBJS) $(TARGET)
.PHONY: all clean
