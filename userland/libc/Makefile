# Makefile for MayteraOS userland libc

CC = gcc
AR = ar

CFLAGS = -m64 -ffreestanding -fno-builtin -nostdinc -nostdlib \
         -fstack-protector-strong -mstack-protector-guard=global \
         -mno-red-zone -fno-pic -mcmodel=large \
         -Wall -Wextra -O2 \
         -isystem /usr/lib/gcc/x86_64-linux-gnu/12/include

# Core sources
SRCS = string.c userconf.c stdio.c stdio_file.c stdlib.c gui.c gui_scroll.c gui_font.c gui_list.c gui_menu.c compositor_client.c \
       errno.c signal.c fcntl.c unistd.c termios.c dirent.c libc_init.c stack_guard.c \
       pwd.c grp.c assoc.c aiclient.c aicap.c notify.c math.c pthread.c \
       ctype.c scanf.c time.c tz.c settingscfg.c assert.c posixextra.c wallpapers.c sha256.c pkgsig.c gui_theme.c gui_palette.c uiscale.c \
       gui_dock.c gui_confirm.c gui_mods.c \
       startmenu_reg.c pkgdest.c aiguard.c poll.c spawn.c \
       contract.c ctinvoke.c getopt.c libgen.c ftw.c utime.c locale.c tmpfile.c system.c wctype.c pipeline.c mtool.c proccpu.c
OBJS = $(SRCS:.c=.o)

# sys/ sources
SYS_SRCS = sys/wait.c sys/ioctl.c sys/stat.c sys/socket.c sys/uio.c sys/utsname.c sys/file.c
SYS_OBJS = $(SYS_SRCS:.c=.o)

ASM_OBJS = setjmp.o syscall.o memcpy_fast.o sig_trampoline.o clone_asm.o

all: crt0.o libc.a

libc.a: $(OBJS) $(SYS_OBJS) $(ASM_OBJS)
	$(AR) rcs $@ $(OBJS) $(SYS_OBJS) $(ASM_OBJS)

crt0.o: crt0.S
	$(CC) $(CFLAGS) -c $< -o $@

setjmp.o: setjmp.asm
	nasm -f elf64 $< -o $@

syscall.o: syscall.asm
	nasm -f elf64 $< -o $@

memcpy_fast.o: memcpy_fast.asm
	nasm -f elf64 $< -o $@

sig_trampoline.o: sig_trampoline.asm
	nasm -f elf64 $< -o $@

clone_asm.o: clone_asm.asm
	nasm -f elf64 $< -o $@

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

sys/%.o: sys/%.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f *.o sys/*.o libc.a

.PHONY: all clean

# The canary handler must NOT be instrumented with a canary. stack_guard.c
# defines __stack_chk_guard and __stack_chk_fail; compiling it with
# -fstack-protector-strong makes the failure path check its own canary, so a
# genuine overflow recurses inside the reporter instead of reporting.
stack_guard.o: stack_guard.c
	$(CC) $(filter-out -fstack-protector-strong,$(CFLAGS)) -fno-stack-protector -c $< -o $@
