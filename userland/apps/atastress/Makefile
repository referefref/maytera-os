CC = gcc
LD = ld
LIBC_DIR = ../../libc
BUILD_DIR = build
CFLAGS = -ffreestanding -fno-stack-protector -fno-pic -mno-red-zone -mcmodel=large \
         -nostdlib -nostdinc -fno-builtin -Wall -Wextra -O2 -g \
         -I$(LIBC_DIR) -isystem /usr/lib/gcc/x86_64-linux-gnu/12/include
LDFLAGS = -nostdlib -z max-page-size=0x1000 -T ../../user.ld
CRT0 = $(LIBC_DIR)/crt0.o
LIBC = $(LIBC_DIR)/libc.a

# Builds 4 independent, separately-linked binaries (atastress0..atastress3),
# each with its own worker id baked in via -DWID=N, so they can be launched
# as 4 independent OS processes (SERVICES.CFG) instead of via fork() - see
# the #444 v3 comment in main.c for why.
all: $(BUILD_DIR) atastress0 atastress1 atastress2 atastress3

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

atastress0: $(BUILD_DIR)/main0.o $(CRT0) $(LIBC)
	$(LD) $(LDFLAGS) -o $@ $(CRT0) $(BUILD_DIR)/main0.o $(LIBC)
atastress1: $(BUILD_DIR)/main1.o $(CRT0) $(LIBC)
	$(LD) $(LDFLAGS) -o $@ $(CRT0) $(BUILD_DIR)/main1.o $(LIBC)
atastress2: $(BUILD_DIR)/main2.o $(CRT0) $(LIBC)
	$(LD) $(LDFLAGS) -o $@ $(CRT0) $(BUILD_DIR)/main2.o $(LIBC)
atastress3: $(BUILD_DIR)/main3.o $(CRT0) $(LIBC)
	$(LD) $(LDFLAGS) -o $@ $(CRT0) $(BUILD_DIR)/main3.o $(LIBC)

$(BUILD_DIR)/main0.o: main.c
	$(CC) $(CFLAGS) -DWID=0 -c $< -o $@
$(BUILD_DIR)/main1.o: main.c
	$(CC) $(CFLAGS) -DWID=1 -c $< -o $@
$(BUILD_DIR)/main2.o: main.c
	$(CC) $(CFLAGS) -DWID=2 -c $< -o $@
$(BUILD_DIR)/main3.o: main.c
	$(CC) $(CFLAGS) -DWID=3 -c $< -o $@

clean:
	rm -rf $(BUILD_DIR) atastress0 atastress1 atastress2 atastress3
.PHONY: all clean
