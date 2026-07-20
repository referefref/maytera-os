// stubs.c - Stub functions

#include <stdint.h>

#define NULL ((void*)0)
// External declarations
extern void kprintf(const char *fmt, ...);
extern uint8_t *fat_read_file(void *fs, const char *path, uint32_t *size);
extern void *g_fat_fs;

// Correct signature matching process.h
extern int proc_create_user(const char *name, void *elf_data, uint64_t elf_size,
                           char **argv, char **envp);

// ELF validation
extern int elf_validate(const void *data, uint64_t size);

// DOOM launcher - loads and executes userland DOOM.ELF

// SMB stubs (smb_list_shares/smb_free_shares now real in net/smb.c)
int smb_rename(void *ctx, const char *old, const char *new) {
    (void)ctx; (void)old; (void)new;
    return -1;
}

// Settings widget stubs
void sw_draw_checkbox(int x, int y, int w, int h, const char *label, int checked, int enabled) {
    (void)x; (void)y; (void)w; (void)h; (void)label; (void)checked; (void)enabled;
}

// Graphics stubs
void fb_draw_hline(int x, int y, int width, uint32_t color) {
    extern void fb_fill_rect(int x, int y, int w, int h, uint32_t color);
    fb_fill_rect(x, y, width, 1, color);
}

// DNS: route to the real resolver in net/dns.c (was a -1 stub).
int dns_lookup(const char *hostname, uint32_t *ip) {
    extern int dns_resolve(const char *hostname, uint32_t *ip_out);
    return dns_resolve(hostname, ip);
}

void sw_draw_radio(int x, int y, int w, int h, const char *label, int selected, int enabled) {
    (void)x; (void)y; (void)w; (void)h; (void)label; (void)selected; (void)enabled;
}

uint64_t timer_get_ticks(void) {
    // Return the REAL monotonic tick counter (was a fake per-call ++counter,
    // which made every timeout/duration using it meaningless).
    extern volatile uint64_t timer_ticks;
    return timer_ticks;
}

// TLS stubs removed - real implementation in net/tls/

// --- Word6/OLE2 merge (ole2c -> main kernel) compat globals ---
volatile unsigned long long g_last_input_tick = 0;  // ole2c win16 idle tick (write-only here)
volatile int g_x86_dbgring = 0;                     // main dosexec debug-ring toggle (off)
