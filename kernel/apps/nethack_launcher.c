// nethack_launcher.c - Launch userland NETHACK.ELF
#include "serial.h"
#include "fs/fat.h"
#include "proc/process.h"
#include "mm/heap.h"
#include "string.h"

extern fat_fs_t g_fat_fs;

void nethack_launch(void) {
    kprintf("[NetHack] Loading NETHACK.ELF from disk...\n");

    uint32_t size = 0;
    uint8_t *elf_data = fat_read_file(&g_fat_fs, "/APPS/NETHACK.ELF", &size);

    if (!elf_data || size == 0) {
        kprintf("[NetHack] Failed to read NETHACK.ELF from disk\n");
        return;
    }

    kprintf("[NetHack] Loaded %u bytes, creating process...\n", size);

    int pid = proc_create_user("NetHack", elf_data, size, NULL, NULL);

    if (pid < 0) {
        kprintf("[NetHack] Failed to create NetHack process (error %d)\n", pid);
        kfree(elf_data);
        return;
    }

    kprintf("[NetHack] NetHack process created with PID %d\n", pid);
    kfree(elf_data);
}
