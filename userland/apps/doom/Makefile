# Makefile for DOOM - MayteraOS userland port

CC = gcc
LD = ld

# Paths
LIBC_DIR = ../../libc

# Compiler flags - freestanding for OS dev
CFLAGS = -DDOOM_DEBUG=1 -ffreestanding -fno-stack-protector -fno-pic -mno-red-zone -msse -msse2 \
         -nostdlib -nostdinc -fno-builtin -O2 -g -mcmodel=large -fno-pie \
         -I. -I$(LIBC_DIR) -isystem /usr/lib/gcc/x86_64-linux-gnu/12/include \
         -DNORMALUNIX -DLINUX -DMAXINT=0x7FFFFFFF -DMININT=0x80000000 -w

# Linker flags
LDFLAGS = -nostdlib -z max-page-size=0x1000 -T ../../user.ld

# Output - uppercase for FAT filesystem
TARGET = DOOM.ELF

# Source files
SRCS = am_map.c d_items.c d_main.c d_net.c doomdef.c doomstat.c \
       dstrings.c f_finale.c f_wipe.c g_game.c hu_lib.c hu_stuff.c \
       i_maytera.c i_net.c i_sound.c i_system.c i_video.c info.c \
       m_argv.c m_bbox.c m_cheat.c m_fixed.c m_menu.c m_misc.c \
       m_random.c m_swap.c p_ceilng.c p_doors.c p_enemy.c p_floor.c \
       p_inter.c p_lights.c p_map.c p_maputl.c p_mobj.c p_plats.c \
       p_pspr.c p_saveg.c p_setup.c p_sight.c p_spec.c p_switch.c \
       p_telept.c p_tick.c p_user.c r_bsp.c r_data.c r_draw.c \
       r_main.c r_plane.c r_segs.c r_sky.c r_things.c s_sound.c \
       sounds.c st_lib.c st_stuff.c tables.c v_video.c w_wad.c \
       wi_stuff.c z_zone.c doom_stubs.c

OBJS = $(SRCS:.c=.o)

# Libraries
CRT0 = $(LIBC_DIR)/crt0.o
LIBC = $(LIBC_DIR)/libc.a

# Default target
all: $(TARGET)

# Link
$(TARGET): $(OBJS) $(CRT0) $(LIBC)
	$(LD) $(LDFLAGS) -o $@ $(CRT0) $(OBJS) $(LIBC)
	@echo "Built $(TARGET)"
	@ls -lh $(TARGET)

# Compile
%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

# Clean
clean:
	rm -f $(OBJS) $(TARGET) doom

.PHONY: all clean
