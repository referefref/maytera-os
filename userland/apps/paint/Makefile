# Makefile for Maytera Studio (the layered image editor grown from Paint).
# The desktop launches /apps/paint, so the TARGET stays "paint".

CC = gcc
LD = ld

LIBC_DIR = ../../libc
USERLAND_DIR = ../..
BUILD_DIR = build

# -mcmodel=large: user.ld base 0x80000000 (=2^31) is negative as signed i32, so
# absolute 32-bit (R_X86_64_32S) refs to large statics/rodata truncate; large
# model fixes it (see blame #444). -T user.ld is REQUIRED or the ELF loads at
# ld's default 0x400000 and the loader page-faults on spawn.
CFLAGS = -ffreestanding -fno-stack-protector -fno-pic -mno-red-zone -mcmodel=large \
         -nostdlib -nostdinc -fno-builtin -Wall -Wextra -O2 -g \
         -I$(LIBC_DIR) -isystem /usr/lib/gcc/x86_64-linux-gnu/12/include

LDFLAGS = -nostdlib -z max-page-size=0x1000 -T $(USERLAND_DIR)/user.ld

TARGET = paint

OBJS = $(BUILD_DIR)/main.o $(BUILD_DIR)/ui.o $(BUILD_DIR)/doc.o \
       $(BUILD_DIR)/select.o $(BUILD_DIR)/tools.o $(BUILD_DIR)/filters.o \
       $(BUILD_DIR)/imgio.o $(BUILD_DIR)/ai.o $(BUILD_DIR)/colorpick.o \
       $(BUILD_DIR)/registry.o $(BUILD_DIR)/color.o $(BUILD_DIR)/render.o $(BUILD_DIR)/fx_blur.o $(BUILD_DIR)/fx_enhance.o $(BUILD_DIR)/fx_edge.o $(BUILD_DIR)/fx_generic.o $(BUILD_DIR)/fx_distort.o $(BUILD_DIR)/fx_light.o $(BUILD_DIR)/fx_noise.o $(BUILD_DIR)/fx_decor.o $(BUILD_DIR)/fx_map.o \
       $(BUILD_DIR)/fx_artistic.o $(BUILD_DIR)/fx_combine.o $(BUILD_DIR)/brushes.o \
       $(BUILD_DIR)/fx_brushstrokes.o $(BUILD_DIR)/fx_sketch.o $(BUILD_DIR)/fx_stylize.o \
       $(BUILD_DIR)/fx_texture.o $(BUILD_DIR)/fx_pixelate.o

CRT0 = $(LIBC_DIR)/crt0.o
LIBC = $(LIBC_DIR)/libc.a

all: $(BUILD_DIR) $(TARGET)

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(TARGET): $(OBJS) $(CRT0) $(LIBC)
	$(LD) $(LDFLAGS) -o $@ $(CRT0) $(OBJS) $(LIBC)
	@echo "Built $(TARGET) (Maytera Studio)"

$(BUILD_DIR)/%.o: %.c studio.h
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -rf $(BUILD_DIR) $(TARGET)

.PHONY: all clean
