// crashhandler.c - Crash Handler Service for MayteraOS
#include "crashhandler.h"
#include "window.h"
#include "desktop.h"
#include "../types.h"
#include "../serial.h"
#include "../mm/heap.h"
#include "../string.h"
#include "../video/framebuffer.h"
#include "../video/font.h"
#include "../cpu/isr.h"
#include "../drivers/mouse.h"
#include "../net/e1000.h"

// External timer
extern volatile uint64_t timer_ticks;

// Crash log
static crash_info_t g_crash_log[CRASH_LOG_SIZE];
static int g_crash_count = 0;
static int g_crash_head = 0;  // Next write position
static bool g_initialized = false;

// Current crash being displayed
static crash_info_t *g_current_crash = NULL;
static bool g_dialog_active = false;

// Kernel debug log ring buffer (visible in crash dialog)
#define KLOG_RING_SIZE  2048
static char g_klog_ring[KLOG_RING_SIZE];
static int  g_klog_ring_pos = 0;   // next write position
static int  g_klog_ring_len = 0;   // total bytes written (capped at KLOG_RING_SIZE)

void klog_ring_append(const char *msg) {
    if (!msg) return;
    for (int i = 0; msg[i] && i < 256; i++) {
        g_klog_ring[g_klog_ring_pos] = msg[i];
        g_klog_ring_pos = (g_klog_ring_pos + 1) % KLOG_RING_SIZE;
        if (g_klog_ring_len < KLOG_RING_SIZE) g_klog_ring_len++;
    }
}

// Get the last N lines from the ring buffer
static int klog_get_last_lines(char *out, int out_size, int max_lines) {
    if (g_klog_ring_len == 0 || !out || out_size < 2) return 0;

    // Find start of ring data
    int start;
    int total;
    if (g_klog_ring_len < KLOG_RING_SIZE) {
        start = 0;
        total = g_klog_ring_len;
    } else {
        start = g_klog_ring_pos;  // oldest byte
        total = KLOG_RING_SIZE;
    }

    // Count newlines from the end to find where to start
    int lines_found = 0;
    int read_start = total;  // default: start from beginning
    for (int i = total - 1; i >= 0; i--) {
        int idx = (start + i) % KLOG_RING_SIZE;
        if (g_klog_ring[idx] == '\n') {
            lines_found++;
            if (lines_found >= max_lines + 1) {
                read_start = i + 1;
                break;
            }
        }
    }
    if (lines_found < max_lines + 1) read_start = 0;

    // Copy from read_start to end
    int out_pos = 0;
    for (int i = read_start; i < total && out_pos < out_size - 1; i++) {
        int idx = (start + i) % KLOG_RING_SIZE;
        out[out_pos++] = g_klog_ring[idx];
    }
    out[out_pos] = '\0';
    return out_pos;
}

// Dialog dimensions
#define CD_WIDTH        500
#define CD_HEIGHT       620
#define CD_PADDING      12
#define CD_CHAR_W       8
#define CD_CHAR_H       16
#define CD_BUTTON_W     80
#define CD_BUTTON_H     24

// Colors
#define CD_BG           0xFFF0F0F0
#define CD_HEADER_BG    0xFFCC0000
#define CD_HEADER_FG    0xFFFFFFFF
#define CD_TEXT_FG      0xFF000000
#define CD_MONO_BG      0xFF1E1E1E
#define CD_MONO_FG      0xFF00FF00
#define CD_BUTTON_BG    0xFFE0E0E0
#define CD_BUTTON_HOVER 0xFFD0D0D0

// Button state
static bool g_kill_hovered = false;
static bool g_continue_hovered = false;
static bool g_details_hovered = false;
static bool g_show_details = true;

// Crash type names
static const char *crash_type_names[] = {
    "Page Fault",
    "General Protection Fault",
    "Invalid Opcode",
    "Divide by Zero",
    "Double Fault",
    "Stack Fault",
    "Assertion Failed",
    "Null Pointer",
    "Unknown Error"
};

const char *crashhandler_type_name(crash_type_t type) {
    if (type >= 0 && type < CRASH_UNKNOWN) {
        return crash_type_names[type];
    }
    return crash_type_names[CRASH_UNKNOWN];
}

void crashhandler_init(void) {
    memset(g_crash_log, 0, sizeof(g_crash_log));
    g_crash_count = 0;
    g_crash_head = 0;
    g_initialized = true;
    kprintf("[CrashHandler] Initialized\n");
}

// Capture memory snapshot around an address
static void capture_memory(uint8_t *dest, uint64_t addr, size_t size) {
    extern int vmm_is_mapped(uint64_t virt_addr);

    // Safety check - don't access invalid memory
    if (addr < 0x1000 || addr > 0xFFFFFFFF) {
        memset(dest, 0, size);
        return;
    }

    // Copy memory with safety bounds - check each page is mapped
    // page_ok is declared OUTSIDE the loop so it persists across iterations.
    // Without this, crossing into an unmapped page on a non-boundary byte
    // would reset page_ok to true and crash the kernel.
    bool page_ok = false;
    for (size_t i = 0; i < size; i++) {
        uint64_t src_addr = addr + i;
        // Only re-check on page boundaries (every 4KB) or first byte
        if ((src_addr & 0xFFF) == 0 || i == 0) {
            page_ok = (src_addr >= 0x1000 && src_addr < 0x100000000 && vmm_is_mapped(src_addr));
        }

        if (page_ok && src_addr >= 0x1000 && src_addr < 0x100000000) {
            dest[i] = *(volatile uint8_t *)src_addr;
        } else {
            dest[i] = 0;
        }
    }
}

void crashhandler_report(crash_type_t type, crash_regs_t *regs, int app_id) {
    // FIRST: Enter crash context to prevent network driver MMIO access
    // This must happen before ANY other operations to avoid recursive page faults
    e1000_enter_crash_context();

    if (!g_initialized) {
        crashhandler_init();
    }
    
    // Get next slot in circular buffer
    crash_info_t *crash = &g_crash_log[g_crash_head];
    
    // Fill in crash info
    crash->type = type;
    crash->type_name = crashhandler_type_name(type);
    crash->timestamp = timer_ticks;
    crash->valid = true;
    
    // Copy registers
    if (regs) {
        memcpy(&crash->regs, regs, sizeof(crash_regs_t));
    } else {
        memset(&crash->regs, 0, sizeof(crash_regs_t));
    }
    
    // Generate description
    switch (type) {
        case CRASH_PAGE_FAULT:
            crash->description = "Memory access violation";
            break;
        case CRASH_GENERAL_PROTECTION:
            crash->description = "Invalid memory operation";
            break;
        case CRASH_INVALID_OPCODE:
            crash->description = "Illegal CPU instruction";
            break;
        case CRASH_DIVIDE_BY_ZERO:
            crash->description = "Division by zero";
            break;
        case CRASH_DOUBLE_FAULT:
            crash->description = "Critical system error";
            break;
        case CRASH_STACK_FAULT:
            crash->description = "Stack overflow or corruption";
            break;
        case CRASH_ASSERTION_FAILED:
            crash->description = "Assertion check failed";
            break;
        case CRASH_NULL_POINTER:
            crash->description = "Null pointer dereference";
            break;
        default:
            crash->description = "Unknown error occurred";
            break;
    }
    
    // Capture memory snapshots
    if (regs) {
        crash->memory.stack_base = regs->rsp - CRASH_STACK_SNAPSHOT_SIZE / 2;
        capture_memory(crash->memory.stack, crash->memory.stack_base, CRASH_STACK_SNAPSHOT_SIZE);
        
        crash->memory.code_base = regs->rip - CRASH_CODE_SNAPSHOT_SIZE / 2;
        capture_memory(crash->memory.code, crash->memory.code_base, CRASH_CODE_SNAPSHOT_SIZE);
    }
    
    // App info
    crash->app_id = app_id;
    crash->app_window = NULL;
    crash->app_name = "Unknown";
    
    if (app_id >= 0) {
        app_registration_t *app = wm_get_app_by_id(app_id);
        if (app && app->window) {
            crash->app_window = app->window;
            crash->app_name = app->window->title;
        }
    }
    
    // Update log pointers
    g_crash_head = (g_crash_head + 1) % CRASH_LOG_SIZE;
    if (g_crash_count < CRASH_LOG_SIZE) {
        g_crash_count++;
    }
    
    // Set current crash
    g_current_crash = crash;
    
    // Log to serial
    crashhandler_dump_serial(crash);
}

void crashhandler_dump_serial(crash_info_t *crash) {
    if (!crash || !crash->valid) return;
    
    kprintf("\n");
    kprintf("========================================\n");
    kprintf("  CRASH REPORT: %s\n", crash->type_name);
    kprintf("========================================\n");
    kprintf("Description: %s\n", crash->description);
    kprintf("Application: %s (ID: %d)\n", crash->app_name, crash->app_id);
    kprintf("Timestamp: %llu ticks\n", crash->timestamp);
    kprintf("\n");
    kprintf("Registers:\n");
    kprintf("  RIP: 0x%016llX  RSP: 0x%016llX\n", crash->regs.rip, crash->regs.rsp);
    kprintf("  RAX: 0x%016llX  RBX: 0x%016llX\n", crash->regs.rax, crash->regs.rbx);
    kprintf("  RCX: 0x%016llX  RDX: 0x%016llX\n", crash->regs.rcx, crash->regs.rdx);
    kprintf("  RSI: 0x%016llX  RDI: 0x%016llX\n", crash->regs.rsi, crash->regs.rdi);
    kprintf("  RBP: 0x%016llX  R8:  0x%016llX\n", crash->regs.rbp, crash->regs.r8);
    kprintf("  R9:  0x%016llX  R10: 0x%016llX\n", crash->regs.r9, crash->regs.r10);
    kprintf("  CR2: 0x%016llX  ERR: 0x%016llX\n", crash->regs.cr2, crash->regs.error_code);
    kprintf("  RFLAGS: 0x%016llX\n", crash->regs.rflags);
    kprintf("========================================\n\n");
}

// Draw helpers
static void cd_draw_string(int32_t x, int32_t y, const char *str, uint32_t fg) {
    while (*str) {
        if (*str >= ' ' && *str < 127) {
            const uint8_t *glyph = font_get_glyph(*str);
            if (glyph) {
                for (int r = 0; r < FONT_HEIGHT; r++) {
                    uint8_t bits = glyph[r];
                    for (int col = 0; col < FONT_WIDTH; col++) {
                        if (bits & (0x80 >> col)) {
                            fb_put_pixel(x + col, y + r, fg & 0xFFFFFF);
                        }
                    }
                }
            }
        }
        x += CD_CHAR_W;
        str++;
    }
}

static void cd_draw_button(int32_t x, int32_t y, const char *label, bool hovered) {
    uint32_t bg = hovered ? CD_BUTTON_HOVER : CD_BUTTON_BG;
    fb_fill_rect(x, y, CD_BUTTON_W, CD_BUTTON_H, bg & 0xFFFFFF);
    fb_fill_rect(x, y, CD_BUTTON_W, 1, 0xA0A0A0);
    fb_fill_rect(x, y + CD_BUTTON_H - 1, CD_BUTTON_W, 1, 0x606060);
    fb_fill_rect(x, y, 1, CD_BUTTON_H, 0xA0A0A0);
    fb_fill_rect(x + CD_BUTTON_W - 1, y, 1, CD_BUTTON_H, 0x606060);
    
    int label_x = x + (CD_BUTTON_W - strlen(label) * CD_CHAR_W) / 2;
    int label_y = y + (CD_BUTTON_H - CD_CHAR_H) / 2;
    cd_draw_string(label_x, label_y, label, CD_TEXT_FG);
}

static void cd_draw_hex(int32_t x, int32_t y, uint64_t value, uint32_t fg) {
    char hex[19] = "0x";
    const char *digits = "0123456789ABCDEF";
    for (int i = 15; i >= 0; i--) {
        hex[17 - i] = digits[(value >> (i * 4)) & 0xF];
    }
    hex[18] = '\0';
    cd_draw_string(x, y, hex, fg);
}

static void cd_draw_dialog(crash_info_t *crash) {
    if (!crash) return;
    
    int screen_w = fb_get_width();
    int screen_h = fb_get_height();
    int dialog_h = g_show_details ? CD_HEIGHT + 120 : CD_HEIGHT;
    int dx = (screen_w - CD_WIDTH) / 2;
    int dy = (screen_h - dialog_h) / 2;
    
    // Shadow
    fb_fill_rect(dx + 4, dy + 4, CD_WIDTH, dialog_h, 0x404040);
    
    // Background
    fb_fill_rect(dx, dy, CD_WIDTH, dialog_h, CD_BG & 0xFFFFFF);
    
    // Red header bar
    fb_fill_rect(dx, dy, CD_WIDTH, 30, CD_HEADER_BG & 0xFFFFFF);
    cd_draw_string(dx + CD_PADDING, dy + 7, "Application Crashed", CD_HEADER_FG);
    
    int y = dy + 40;
    
    // Crash icon (simple X in a circle)
    int icon_x = dx + CD_PADDING;
    int icon_y = y;
    fb_fill_rect(icon_x, icon_y, 32, 32, 0xCC0000);
    cd_draw_string(icon_x + 12, icon_y + 8, "X", 0xFFFFFF);
    
    // Crash info
    int text_x = dx + CD_PADDING + 44;
    cd_draw_string(text_x, y, crash->type_name, CD_TEXT_FG);
    y += CD_CHAR_H + 4;
    cd_draw_string(text_x, y, crash->description, 0x606060);
    y += CD_CHAR_H + 8;
    
    // App name
    char app_info[64];
    strcpy(app_info, "Application: ");
    strcat(app_info, crash->app_name);
    cd_draw_string(dx + CD_PADDING, y, app_info, CD_TEXT_FG);
    y += CD_CHAR_H + 16;
    
    // Key registers
    cd_draw_string(dx + CD_PADDING, y, "Fault Address:", CD_TEXT_FG);
    cd_draw_hex(dx + CD_PADDING + 15 * CD_CHAR_W, y, crash->regs.cr2, 0x0000CC);
    y += CD_CHAR_H;
    
    cd_draw_string(dx + CD_PADDING, y, "Instruction:  ", CD_TEXT_FG);
    cd_draw_hex(dx + CD_PADDING + 15 * CD_CHAR_W, y, crash->regs.rip, 0x0000CC);
    y += CD_CHAR_H;
    
    cd_draw_string(dx + CD_PADDING, y, "Stack Pointer:", CD_TEXT_FG);
    cd_draw_hex(dx + CD_PADDING + 15 * CD_CHAR_W, y, crash->regs.rsp, 0x0000CC);
    y += CD_CHAR_H + 8;

    // Show return addresses from stack (populated by exception handler)
    if (crash->stack_entry_count > 0) {
        cd_draw_string(dx + CD_PADDING, y, "Stack trace:", CD_TEXT_FG);
        y += CD_CHAR_H + 2;
        for (int i = 0; i < crash->stack_entry_count && i < 8; i++) {
            char lbl[16];
            strcpy(lbl, "[RSP+");
            int off = i * 8;
            if (off >= 10) { lbl[5] = (char)(48 + off / 10); lbl[6] = (char)(48 + off % 10); lbl[7] = 93; lbl[8] = 0; }
            else { lbl[5] = (char)(48 + off); lbl[6] = 93; lbl[7] = 0; }
            cd_draw_string(dx + CD_PADDING, y, lbl, 0x606060);
            cd_draw_hex(dx + CD_PADDING + 10 * CD_CHAR_W, y, crash->stack_entries[i], 0x0000CC);
            y += CD_CHAR_H;
        }
        y += 8;
    } else {
        y += 8;
    }
    
    // Details section (expandable)
    if (g_show_details) {
        // Register dump in monospace box
        fb_fill_rect(dx + CD_PADDING, y, CD_WIDTH - 2 * CD_PADDING, 100, CD_MONO_BG & 0xFFFFFF);
        
        int ry = y + 4;
        char reg_line[80];
        
        strcpy(reg_line, "RAX=");
        cd_draw_hex(dx + CD_PADDING + 4, ry, crash->regs.rax, CD_MONO_FG);
        cd_draw_string(dx + CD_PADDING + 4 + 20 * CD_CHAR_W, ry, "RBX=", CD_MONO_FG);
        cd_draw_hex(dx + CD_PADDING + 4 + 24 * CD_CHAR_W, ry, crash->regs.rbx, CD_MONO_FG);
        ry += CD_CHAR_H;
        
        cd_draw_string(dx + CD_PADDING + 4, ry, "RCX=", CD_MONO_FG);
        cd_draw_hex(dx + CD_PADDING + 4 + 4 * CD_CHAR_W, ry, crash->regs.rcx, CD_MONO_FG);
        cd_draw_string(dx + CD_PADDING + 4 + 24 * CD_CHAR_W, ry, "RDX=", CD_MONO_FG);
        cd_draw_hex(dx + CD_PADDING + 4 + 28 * CD_CHAR_W, ry, crash->regs.rdx, CD_MONO_FG);
        ry += CD_CHAR_H;
        
        cd_draw_string(dx + CD_PADDING + 4, ry, "RSI=", CD_MONO_FG);
        cd_draw_hex(dx + CD_PADDING + 4 + 4 * CD_CHAR_W, ry, crash->regs.rsi, CD_MONO_FG);
        cd_draw_string(dx + CD_PADDING + 4 + 24 * CD_CHAR_W, ry, "RDI=", CD_MONO_FG);
        cd_draw_hex(dx + CD_PADDING + 4 + 28 * CD_CHAR_W, ry, crash->regs.rdi, CD_MONO_FG);
        ry += CD_CHAR_H;
        
        cd_draw_string(dx + CD_PADDING + 4, ry, "RBP=", CD_MONO_FG);
        cd_draw_hex(dx + CD_PADDING + 4 + 4 * CD_CHAR_W, ry, crash->regs.rbp, CD_MONO_FG);
        cd_draw_string(dx + CD_PADDING + 4 + 24 * CD_CHAR_W, ry, "ERR=", CD_MONO_FG);
        cd_draw_hex(dx + CD_PADDING + 4 + 28 * CD_CHAR_W, ry, crash->regs.error_code, CD_MONO_FG);
        ry += CD_CHAR_H;
        
        cd_draw_string(dx + CD_PADDING + 4, ry, "RFLAGS=", CD_MONO_FG);
        cd_draw_hex(dx + CD_PADDING + 4 + 7 * CD_CHAR_W, ry, crash->regs.rflags, CD_MONO_FG);
        
        y += 108;
    }
    
    // Debug log section (last klog messages from SYS_KLOG)
    {
        static char klog_buf[512];
        int klog_len = klog_get_last_lines(klog_buf, sizeof(klog_buf), 6);
        if (klog_len > 0) {
            cd_draw_string(dx + CD_PADDING, y, "Debug log (last msgs):", CD_TEXT_FG);
            y += CD_CHAR_H + 2;
            fb_fill_rect(dx + CD_PADDING, y, CD_WIDTH - 2 * CD_PADDING,
                         6 * CD_CHAR_H + 8, CD_MONO_BG & 0xFFFFFF);
            int ly = y + 4;
            int line_start = 0;
            for (int ki = 0; ki <= klog_len; ki++) {
                bool is_end = (ki == klog_len);
                bool is_nl  = (!is_end && klog_buf[ki] == 10);  // newline char
                if (is_end || is_nl) {
                    if (ki > line_start) {
                        int max_chars = (CD_WIDTH - 2 * CD_PADDING - 8) / CD_CHAR_W;
                        int end = ki;
                        if (end - line_start > max_chars) end = line_start + max_chars;
                        char save = klog_buf[end];
                        klog_buf[end] = 0;  // NUL terminate for draw
                        cd_draw_string(dx + CD_PADDING + 4, ly,
                                       &klog_buf[line_start], CD_MONO_FG);
                        klog_buf[end] = save;
                        ly += CD_CHAR_H;
                    }
                    line_start = ki + 1;
                }
            }
            y += 6 * CD_CHAR_H + 12;
        }
    }

    // Buttons
    int btn_y = dy + dialog_h - CD_PADDING - CD_BUTTON_H;
    int btn_x = dx + CD_WIDTH - CD_PADDING - CD_BUTTON_W;
    
    cd_draw_button(btn_x, btn_y, "Close App", g_kill_hovered);
}

void crashhandler_show_dialog(void) {
    if (!g_current_crash) return;
    
    g_dialog_active = true;
    g_show_details = true;
    
    kprintf("[CrashHandler] Showing crash dialog\n");
    
    // Disable preemption so the compositor (and other processes) stop
    // running.  Without this, the compositor races for the framebuffer
    // and the crash dialog becomes unresponsive.
    extern bool sched_set_preemption(bool enable);
    bool old_preempt = sched_set_preemption(false);
    
    bool prev_left = false;
    // Enable interrupts and ensure mouse IRQ is unmasked
    // Mouse is IRQ 12 (slave PIC bit 4)
    outb(0x21, inb(0x21) & ~0x04);  // Unmask IRQ 2 (cascade)
    outb(0xA1, inb(0xA1) & ~0x10);  // Unmask IRQ 12 (mouse)
    __asm__ volatile("sti");
    
    while (g_dialog_active) {
        int32_t mx, my;
        mouse_get_position(&mx, &my);
        bool left = mouse_button_pressed(0);
        bool left_click = left && !prev_left;
        prev_left = left;
        
        // Calculate button positions
        int screen_w = fb_get_width();
        int screen_h = fb_get_height();
        int dialog_h = g_show_details ? CD_HEIGHT + 120 : CD_HEIGHT;
        int dx = (screen_w - CD_WIDTH) / 2;
        int dy = (screen_h - dialog_h) / 2;
        int btn_y = dy + dialog_h - CD_PADDING - CD_BUTTON_H;
        
        int kill_x = dx + CD_WIDTH - CD_PADDING - CD_BUTTON_W;
        int continue_x = kill_x - CD_BUTTON_W - 8;
        int details_x = continue_x - CD_BUTTON_W - 8;
        
        // Update hover state
        g_kill_hovered = (mx >= kill_x && mx < kill_x + CD_BUTTON_W &&
                          my >= btn_y && my < btn_y + CD_BUTTON_H);
        g_continue_hovered = (mx >= continue_x && mx < continue_x + CD_BUTTON_W &&
                              my >= btn_y && my < btn_y + CD_BUTTON_H);
        g_details_hovered = (mx >= details_x && mx < details_x + CD_BUTTON_W &&
                             my >= btn_y && my < btn_y + CD_BUTTON_H);
        
        // Handle clicks
        if (left_click) {
            if (g_kill_hovered) {
                kprintf("[CrashHandler] Kill App clicked\n");
                crashhandler_kill_app(g_current_crash);
                g_dialog_active = false;
                g_dialog_active = false;
            }
        }
        
        // Handle ESC key
        if (keyboard_has_char()) {
            int key = keyboard_get_char();
            if (key == 27) {  // Escape
                g_dialog_active = false;
            }
        }
        
        // Draw
        extern bool wm_is_exclusive_mode(void);
        if (wm_is_exclusive_mode()) {
            // Compositor is active: fill screen with dark background
            // instead of drawing the kernel desktop (which should never show).
            fb_fill_rect(0, 0, fb_get_width(), fb_get_height(), 0x1A1A2E);
        } else {
            extern void desktop_draw(void);
            desktop_draw();
            wm_draw_all();
        }
        cd_draw_dialog(g_current_crash);

        extern void desktop_draw_cursor(int32_t x, int32_t y);
        desktop_draw_cursor(mx, my);

        // #418 ROOT CAUSE FIX: fb_swap_buffers() copies fb_back -> fb_front,
        // and fb_front is the raw physical framebuffer at its identity-mapped
        // address (see video/framebuffer.c). We are here from INSIDE an
        // exception handler, so whatever CR3 the FAULTING process had loaded
        // is still active - and every user process (compositor, widgets drawn
        // in-process, and standalone services like haservice/netinfo/svchb)
        // links at the SAME low-half base (userland/user.ld, 0x80000000-ish
        // "VMM user space region"), which on real hardware is essentially
        // never the same physical page as the real GOP framebuffer BAR. This
        // is the exact bug class gui/fb_syscall.c's sys_fb_flip() was fixed
        // for (#307/#368): without a kernel-CR3 switch, the crash dialog's own
        // present either corrupts whatever IS mapped at that VA in the
        // faulting process, or faults immediately while already inside
        // exception context - the textbook path to a double/triple fault. In
        // other words the CODE THAT REPORTS A CRASH was itself unguarded
        // against the same crash. Mirror sys_fb_flip()'s save/switch/restore
        // exactly: interrupts are already handled by the caller (idt.c calls
        // us from inside the exception handler with the CPU's normal IF
        // state; the dialog loop itself explicitly STIs above so the mouse
        // IRQ can run), and preemption is already disabled
        // (sched_set_preemption(false) above), so no context switch can
        // observe or clobber the temporary CR3 between switch and restore.
        {
            extern uint64_t g_kernel_cr3;
            extern void vmm_switch_pml4(uint64_t);
            uint64_t saved_cr3 = read_cr3();
            if (g_kernel_cr3) vmm_switch_pml4(g_kernel_cr3);
            fb_swap_buffers();
            vmm_switch_pml4(saved_cr3);
        }

        // Small delay so the dialog loop does not spin at 100% CPU.
        // Timer ticks still fire (they run the mouse handler) but
        // preemption is off so no context switches happen.
        for (int i = 0; i < 500; i++) {
            __asm__ volatile("pause");
        }
    }

    // Re-enable preemption before returning to exception handler
    sched_set_preemption(old_preempt);
}

bool crashhandler_kill_app(crash_info_t *crash) {
    if (!crash) return false;
    if (crash->app_id < 0) {
        // app_id unknown, but still mark the dialog as done
        // so proc_exit can run
        g_dialog_active = false;
        return true;
    }
    
    kprintf("[CrashHandler] Killing app %d (%s)\n", crash->app_id, crash->app_name);
    
    // Unregister from window manager
    wm_unregister_app(crash->app_id);
    
    // Hide and destroy window
    if (crash->app_window) {
        window_hide(crash->app_window);
        window_destroy(crash->app_window);
        crash->app_window = NULL;
    }
    
    // Invalidate screen
    wm_invalidate_all();
    
    return true;
}

bool crashhandler_try_recover(crash_info_t *crash) {
    // For now, just attempt to continue - no real recovery
    if (!crash) return false;
    kprintf("[CrashHandler] Attempting recovery (no-op)\n");
    return true;
}

crash_info_t *crashhandler_get_last(void) {
    if (g_crash_count == 0) return NULL;
    int idx = (g_crash_head - 1 + CRASH_LOG_SIZE) % CRASH_LOG_SIZE;
    return &g_crash_log[idx];
}

crash_info_t *crashhandler_get(int index) {
    if (index < 0 || index >= g_crash_count) return NULL;
    int idx = (g_crash_head - 1 - index + CRASH_LOG_SIZE) % CRASH_LOG_SIZE;
    return &g_crash_log[idx];
}

int crashhandler_get_count(void) {
    return g_crash_count;
}

void crashhandler_clear_log(void) {
    g_crash_count = 0;
    g_crash_head = 0;
    g_current_crash = NULL;
    memset(g_crash_log, 0, sizeof(g_crash_log));
}
