// browser_launcher.c - Launch browser (user-space or kernel fallback)
#include "../serial.h"
#include "../fs/fat.h"
#include "../exec/elf.h"
#include "../proc/process.h"
#include "../mm/heap.h"
#include "../string.h"

extern fat_fs_t g_fat_fs;
extern void browser_launch_kernel(void);

void browser_launch(void) {
    if (g_fat_fs.mounted) {
        kprintf("[Browser] Loading /apps/browser (user-space)\n");
        uint32_t size = 0;
        void *data = fat_read_file(&g_fat_fs, "/apps/browser", &size);
        if (data && size > 0) {
            int pid = proc_create_user("Browser", data, size, NULL, NULL);
            if (pid > 0) {
                kprintf("[Browser] User-space started (PID %d)\n", pid);
                kfree(data);
                return;
            }
            kfree(data);
        }
    }
    kprintf("[Browser] Falling back to kernel browser\n");
    browser_launch_kernel();
}
