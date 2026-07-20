# Makefile for mfa - MayteraOS TOTP / MFA authenticator (#271)

CC = gcc
LD = ld

LIBC_DIR = ../../libc

CFLAGS = -ffreestanding -fno-stack-protector -fno-pic -mno-red-zone -mcmodel=large \
         -nostdlib -nostdinc -fno-builtin -Wall -Wextra -O2 -g \
         -I$(LIBC_DIR) -isystem /usr/lib/gcc/x86_64-linux-gnu/12/include

LDFLAGS = -nostdlib -z max-page-size=0x1000 -T ../../user.ld

TARGET = mfa
OBJS = main.o totp.o

CRT0 = $(LIBC_DIR)/crt0.o
LIBC = $(LIBC_DIR)/libc.a

all: $(TARGET)

$(TARGET): $(OBJS) $(CRT0) $(LIBC)
	$(LD) $(LDFLAGS) -o $@ $(CRT0) $(OBJS) $(LIBC)
	@echo "Built $(TARGET)"
	@ls -l $(TARGET)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

# Host-side unit test of the TOTP core against RFC 6238 vectors.
hosttest: totp.c totp_test.c
	gcc -O2 -Wall -Wextra -DMFA_HOST_TEST -o totp_test totp_test.c totp.c
	./totp_test

clean:
	rm -f $(OBJS) $(TARGET) totp_test

.PHONY: all clean hosttest
