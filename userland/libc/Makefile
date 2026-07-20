# Makefile for MayteraOS userland libc

CC = gcc
AR = ar

CFLAGS = -m64 -ffreestanding -fno-builtin -nostdinc -nostdlib \
         -fno-stack-protector -mno-red-zone -fno-pic -mcmodel=large \
         -Wall -Wextra -O2 \
         -isystem /usr/lib/gcc/x86_64-linux-gnu/12/include

# Core sources
SRCS = string.c stdio.c stdio_file.c stdlib.c gui.c gui_scroll.c gui_font.c compositor_client.c \
       errno.c signal.c fcntl.c unistd.c termios.c dirent.c libc_init.c \
       pwd.c grp.c assoc.c aiclient.c aicap.c notify.c math.c pthread.c \
       ctype.c scanf.c time.c assert.c posixextra.c wallpapers.c
OBJS = $(SRCS:.c=.o)

# sys/ sources
SYS_SRCS = sys/wait.c sys/ioctl.c sys/stat.c
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
