# Makefile for mfa - MayteraOS TOTP / MFA authenticator (#271)

CC = gcc
LD = ld

LIBC_DIR = ../../libc

CFLAGS = -ffreestanding -fstack-protector-strong -mstack-protector-guard=global -fPIE -mno-red-zone \
         -nostdlib -nostdinc -fno-builtin -Wall -Wextra -O2 -g \
         -I$(LIBC_DIR) -isystem /usr/lib/gcc/x86_64-linux-gnu/12/include

LDFLAGS = -pie --no-dynamic-linker -z notext -nostdlib -z max-page-size=0x1000 -T ../../user-pie.ld

TARGET = mfa
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

# THE HOST-SIDE RFC 6238 UNIT TEST IS GONE FROM HERE, AND IT DID NOT DIE.
# It tested totp.c, which this app no longer has. The same vectors, and eleven
# more, now run against the KERNEL's implementation in two places that both
# execute: kernel/crypto/secretuse_kat.sh on the host (RFC 6238 Appendix B, SHA1
# AND SHA256 arms, all six published times each, plus four proven RED builds) and
# totp_selftest_rs() on EVERY BOOT. Deleting a test along with the code it covered
# is correct; deleting it and not saying where the coverage went is how a tree ends
# up believing it tests something it does not.

clean:
	rm -f $(OBJS) $(TARGET)

.PHONY: all clean
