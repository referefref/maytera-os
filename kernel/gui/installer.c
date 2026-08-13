// installer.c - MayteraOS Installer GUI Application
// Provides a full graphical installer for installing MayteraOS to disk
// Version 1.8.0 - Full installation wizard with all features

#include "installer.h"
#include "window.h"
#include "widget.h"
#include "desktop.h"
#include "font.h"
#include "icons.h"
#include "../types.h"
#include "../string.h"
#include "../fs/ext2.h"
#include "../fs/blockdev.h"
#include "../drivers/usb_msc.h"
#include "../drivers/ahci.h"
#include "../serial.h"
#include "../mm/heap.h"
#include "../video/framebuffer.h"
#include "../drivers/ata.h"
#include "../fs/fat.h"

// The booted/source FAT filesystem (defined in main.c). The installer reads its
// partition extent (start LBA + sector count) to clone it onto the target.
extern fat_fs_t g_fat_fs;
extern void proc_sleep(uint32_t ms);

// Global installer state
static installer_t *g_installer = NULL;
static bool g_installer_running = false;

// Step titles
static const char *step_titles[] = {
    "Welcome to MayteraOS",
    "Select Installation Disk",
    "Partition Configuration",
    "Create User Account",
    "Timezone & Locale",
    "Installing MayteraOS",
    "Installation Complete"
};

// Timezone data
static const char *timezones[] = {
    "UTC", "America/New_York", "America/Chicago", "America/Denver",
    "America/Los_Angeles", "America/Anchorage", "Pacific/Honolulu",
    "Europe/London", "Europe/Paris", "Europe/Berlin", "Europe/Moscow",
    "Asia/Tokyo", "Asia/Shanghai", "Asia/Singapore", "Asia/Dubai",
    "Australia/Sydney", "Australia/Perth", "Pacific/Auckland"
};
#define NUM_TIMEZONES 18

// Keyboard layouts - names only for display
static const char *keyboard_layout_names[] = {
    "US English", "UK English", "German", "French", "Spanish",
    "Italian", "Portuguese", "Russian", "Japanese", "Korean", "Chinese"
};
#define NUM_KEYBOARD_LAYOUTS 11

// Language data
static const char *languages[] = {
    "English", "Spanish", "French", "German", "Portuguese",
    "Russian", "Japanese", "Chinese", "Korean"
};
#define NUM_LANGUAGES 9

// ============================================================================
// Helper Drawing Functions
// ============================================================================

static void inst_fill_rect(int32_t x, int32_t y, int32_t w, int32_t h, uint32_t color) {
    fb_fill_rect(x, y, w, h, color);
}

static void inst_draw_rect(int32_t x, int32_t y, int32_t w, int32_t h, uint32_t color) {
    fb_draw_rect(x, y, w, h, color);
}

static void inst_draw_text(int32_t x, int32_t y, const char *text, uint32_t color) {
    font_draw_string(x, y, text, FONT_SYSTEM, color);
}

static void inst_draw_text_bold(int32_t x, int32_t y, const char *text, uint32_t color) {
    font_draw_string(x, y, text, FONT_TITLE, color);
}

static void inst_draw_text_centered(int32_t x, int32_t y, int32_t w, const char *text, uint32_t color) {
    int text_len = 0;
    const char *p = text;
    while (*p++) text_len++;
    int text_w = text_len * 8;
    int32_t tx = x + (w - text_w) / 2;
    inst_draw_text(tx, y, text, color);
}

static void inst_draw_button(int32_t x, int32_t y, int32_t w, int32_t h,
                             const char *label, uint32_t bg_color, bool hovered) {
    uint32_t actual_bg = hovered ? (bg_color + 0x202020) : bg_color;
    inst_fill_rect(x, y, w, h, actual_bg);
    inst_draw_rect(x, y, w, h, 0x00404040);
    inst_draw_text_centered(x, y + (h - 12) / 2, w, label, 0x00FFFFFF);
}

static void inst_draw_progress_bar(int32_t x, int32_t y, int32_t w, int32_t h, int percent) {
    inst_fill_rect(x, y, w, h, INSTALLER_PROGRESS_BG);
    inst_draw_rect(x, y, w, h, 0x00808080);

    if (percent > 0) {
        int32_t fill_w = (w - 4) * percent / 100;
        if (fill_w > 0) {
            inst_fill_rect(x + 2, y + 2, fill_w, h - 4, INSTALLER_PROGRESS_FG);
        }
    }

    char percent_str[8];
    int i = 0;
    if (percent >= 100) {
        percent_str[i++] = '1';
        percent_str[i++] = '0';
        percent_str[i++] = '0';
    } else if (percent >= 10) {
        percent_str[i++] = '0' + (percent / 10);
        percent_str[i++] = '0' + (percent % 10);
    } else {
        percent_str[i++] = '0' + percent;
    }
    percent_str[i++] = '%';
    percent_str[i] = '\0';
    inst_draw_text_centered(x, y + (h - 12) / 2, w, percent_str, 0x00000000);
}

static void inst_draw_input(int32_t x, int32_t y, int32_t w, int32_t h,
                            const char *text, bool focused, bool password) {
    inst_fill_rect(x, y, w, h, 0x00FFFFFF);
    inst_draw_rect(x, y, w, h, focused ? INSTALLER_ACCENT_COLOR : 0x00808080);
    if (focused) {
        inst_draw_rect(x - 1, y - 1, w + 2, h + 2, INSTALLER_ACCENT_COLOR);
    }

    if (password && text[0]) {
        char masked[128];
        int i = 0;
        while (text[i] && i < 127) {
            masked[i] = '*';
            i++;
        }
        masked[i] = '\0';
        inst_draw_text(x + 8, y + (h - 12) / 2, masked, 0x00000000);
    } else {
        inst_draw_text(x + 8, y + (h - 12) / 2, text, 0x00000000);
    }

    if (focused) {
        int text_len = 0;
        const char *p = text;
        while (*p++) text_len++;
        int cursor_x = x + 8 + text_len * 8;
        inst_fill_rect(cursor_x, y + 4, 2, h - 8, 0x00000000);
    }
}

static void inst_draw_checkbox(int32_t x, int32_t y, bool checked, bool hovered) {
    inst_fill_rect(x, y, 18, 18, hovered ? 0x00E0E0E0 : 0x00FFFFFF);
    inst_draw_rect(x, y, 18, 18, 0x00606060);

    if (checked) {
        inst_fill_rect(x + 4, y + 9, 3, 5, INSTALLER_ACCENT_COLOR);
        inst_fill_rect(x + 7, y + 11, 2, 3, INSTALLER_ACCENT_COLOR);
        inst_fill_rect(x + 9, y + 8, 2, 6, INSTALLER_ACCENT_COLOR);
        inst_fill_rect(x + 11, y + 5, 2, 8, INSTALLER_ACCENT_COLOR);
        inst_fill_rect(x + 13, y + 3, 2, 8, INSTALLER_ACCENT_COLOR);
    }
}

static void inst_draw_dropdown(int32_t x, int32_t y, int32_t w, int32_t h,
                               const char *text, bool focused) {
    inst_fill_rect(x, y, w, h, 0x00FFFFFF);
    inst_draw_rect(x, y, w, h, focused ? INSTALLER_ACCENT_COLOR : 0x00808080);
    inst_draw_text(x + 8, y + (h - 12) / 2, text, 0x00000000);

    int32_t ax = x + w - 20;
    int32_t ay = y + h / 2 - 3;
    for (int i = 0; i < 6; i++) {
        inst_fill_rect(ax + i, ay + i, 12 - i * 2, 1, 0x00606060);
    }
}

static void format_size(uint64_t bytes, char *buf, int buf_size) {
    const char *units[] = {"B", "KB", "MB", "GB", "TB"};
    int unit = 0;
    uint64_t value = bytes;
    uint64_t remainder = 0;

    while (value >= 1024 && unit < 4) {
        remainder = (value % 1024) * 10 / 1024;
        value /= 1024;
        unit++;
    }

    char num[32];
    int i = 0;
    uint64_t v = value;
    if (v == 0) {
        num[i++] = '0';
    } else {
        while (v > 0) {
            num[i++] = '0' + (v % 10);
            v /= 10;
        }
    }
    num[i] = '\0';

    int j = 0;
    while (j < i / 2) {
        char t = num[j];
        num[j] = num[i - 1 - j];
        num[i - 1 - j] = t;
        j++;
    }

    int k = 0;
    j = 0;
    while (num[j] && k < buf_size - 8) {
        buf[k++] = num[j++];
    }
    if (remainder > 0 && k < buf_size - 6) {
        buf[k++] = '.';
        buf[k++] = '0' + (char)remainder;
    }
    buf[k++] = ' ';
    j = 0;
    while (units[unit][j] && k < buf_size - 1) {
        buf[k++] = units[unit][j++];
    }
    buf[k] = '\0';
}

// ============================================================================
// Step Drawing Functions
// ============================================================================

void installer_draw_welcome(installer_t *inst) {
    if (!inst || !inst->window) return;

    int32_t wx, wy, ww, wh;
    window_get_content_bounds(inst->window, &wx, &wy, &ww, &wh);

    inst_fill_rect(wx, wy, ww, 70, INSTALLER_HEADER_COLOR);
    inst_draw_text_centered(wx, wy + 15, ww, "Welcome to MayteraOS", 0x00FFFFFF);
    inst_draw_text_centered(wx, wy + 40, ww, "Version 1.8.0", 0x00C0C0FF);

    int32_t logo_x = wx + ww / 2 - 50;
    int32_t logo_y = wy + 90;
    inst_fill_rect(logo_x, logo_y, 100, 80, INSTALLER_ACCENT_COLOR);
    inst_draw_rect(logo_x, logo_y, 100, 80, 0x00204060);
    inst_draw_text_centered(logo_x, logo_y + 30, 100, "MayteraOS", 0x00FFFFFF);

    int32_t content_y = logo_y + 100;
    inst_draw_text_centered(wx, content_y, ww, "Welcome to the MayteraOS Installation Wizard", INSTALLER_TEXT_COLOR);
    content_y += 35;

    inst_draw_text(wx + 40, content_y, "This wizard will guide you through installing MayteraOS", INSTALLER_TEXT_COLOR);
    content_y += 25;
    inst_draw_text(wx + 40, content_y, "on your computer. The installation process includes:", INSTALLER_TEXT_COLOR);
    content_y += 35;

    inst_draw_text(wx + 60, content_y, "1. Select an installation disk", INSTALLER_TEXT_COLOR);
    content_y += 25;
    inst_draw_text(wx + 60, content_y, "2. Configure disk partitions (EFI + Root)", INSTALLER_TEXT_COLOR);
    content_y += 25;
    inst_draw_text(wx + 60, content_y, "3. Create your user account", INSTALLER_TEXT_COLOR);
    content_y += 25;
    inst_draw_text(wx + 60, content_y, "4. Set timezone and keyboard layout", INSTALLER_TEXT_COLOR);
    content_y += 25;
    inst_draw_text(wx + 60, content_y, "5. Copy system files and install bootloader", INSTALLER_TEXT_COLOR);
    content_y += 40;

    inst_fill_rect(wx + 30, content_y - 5, ww - 60, 50, 0x00FFF3CD);
    inst_draw_rect(wx + 30, content_y - 5, ww - 60, 50, 0x00FFC107);
    inst_draw_text(wx + 50, content_y + 5, "WARNING: Installation will erase data on the selected disk.", 0x00856404);
    inst_draw_text(wx + 50, content_y + 25, "Please backup important files before proceeding.", 0x00856404);

    content_y = wy + wh - 100;
    inst_draw_text(wx + 40, content_y, "Language:", INSTALLER_TEXT_COLOR);
    inst_draw_dropdown(wx + 150, content_y - 5, 200, 30, languages[inst->locale.language_index], false);

    inst_draw_button(wx + ww - 120, wy + wh - 50, 100, 35, "Next >", INSTALLER_BTN_PRIMARY, inst->hover_button == 1);
}

void installer_draw_disk_select(installer_t *inst) {
    if (!inst || !inst->window) return;

    int32_t wx, wy, ww, wh;
    window_get_content_bounds(inst->window, &wx, &wy, &ww, &wh);

    inst_fill_rect(wx, wy, ww, 60, INSTALLER_HEADER_COLOR);
    inst_draw_text_centered(wx, wy + 20, ww, "Select Installation Disk", 0x00FFFFFF);

    int32_t content_y = wy + 75;
    inst_draw_text(wx + 20, content_y, "Choose the disk where MayteraOS will be installed:", INSTALLER_TEXT_COLOR);
    content_y += 25;

    int32_t list_y = content_y + 10;
    for (uint32_t i = 0; i < inst->disk_count && i < 8; i++) {
        bool selected = ((int)i == inst->selected_disk);
        bool hovered = ((int)i == inst->hover_disk);

        uint32_t bg_color = selected ? INSTALLER_DISK_SELECTED :
                           (hovered ? 0x00F0F0F0 : INSTALLER_DISK_BG);
        inst_fill_rect(wx + 20, list_y, ww - 40, 55, bg_color);
        inst_draw_rect(wx + 20, list_y, ww - 40, 55, selected ? INSTALLER_ACCENT_COLOR : 0x00C0C0C0);

        inst_fill_rect(wx + 30, list_y + 12, 30, 30, 0x00707070);
        inst_draw_text(wx + 35, list_y + 22, "HDD", 0x00FFFFFF);

        inst_draw_text_bold(wx + 75, list_y + 8, inst->disks[i].name, INSTALLER_TEXT_COLOR);
        inst_draw_text(wx + 75, list_y + 25, inst->disks[i].model, 0x00606060);

        char size_str[32];
        format_size(inst->disks[i].size_bytes, size_str, sizeof(size_str));
        inst_draw_text(wx + ww - 150, list_y + 15, size_str, INSTALLER_TEXT_COLOR);

        if (inst->disks[i].removable) {
            inst_draw_text(wx + ww - 150, list_y + 32, "(Removable)", 0x00808080);
        }

        list_y += 60;
    }

    if (inst->disk_count == 0) {
        inst_draw_text_centered(wx, list_y + 30, ww, "No disks detected!", 0x00CC0000);
        inst_draw_text_centered(wx, list_y + 55, ww, "Please check your hardware connection.", 0x00808080);
    }

    if (inst->selected_disk >= 0) {
        int32_t warn_y = wy + wh - 110;
        inst_fill_rect(wx + 20, warn_y, ww - 40, 45, 0x00FFEBEE);
        inst_draw_rect(wx + 20, warn_y, ww - 40, 45, 0x00E74C3C);
        inst_draw_text(wx + 35, warn_y + 8, "All data on this disk will be erased!", 0x00C62828);
        inst_draw_text(wx + 35, warn_y + 25, "Make sure you have backed up any important files.", 0x00C62828);
    }

    inst_draw_button(wx + 20, wy + wh - 50, 100, 35, "< Back", INSTALLER_BTN_SECONDARY, inst->hover_button == 0);
    inst_draw_button(wx + ww - 240, wy + wh - 50, 100, 35, "Refresh", INSTALLER_BTN_SECONDARY, inst->hover_button == 2);
    inst_draw_button(wx + ww - 120, wy + wh - 50, 100, 35, "Next >", 
                     inst->selected_disk >= 0 ? INSTALLER_BTN_PRIMARY : 0x00A0A0A0,
                     inst->hover_button == 1);
}

void installer_draw_partition(installer_t *inst) {
    if (!inst || !inst->window) return;

    int32_t wx, wy, ww, wh;
    window_get_content_bounds(inst->window, &wx, &wy, &ww, &wh);

    inst_fill_rect(wx, wy, ww, 60, INSTALLER_HEADER_COLOR);
    inst_draw_text_centered(wx, wy + 20, ww, "Partition Configuration", 0x00FFFFFF);

    int32_t content_y = wy + 75;
    if (inst->selected_disk >= 0 && inst->selected_disk < (int)inst->disk_count) {
        char disk_info[128];
        char size_str[32];
        format_size(inst->disks[inst->selected_disk].size_bytes, size_str, sizeof(size_str));

        const char *name = inst->disks[inst->selected_disk].name;
        const char *model = inst->disks[inst->selected_disk].model;
        int i = 0, j;
        disk_info[i++] = 'D'; disk_info[i++] = 'i'; disk_info[i++] = 's'; disk_info[i++] = 'k';
        disk_info[i++] = ':'; disk_info[i++] = ' ';
        j = 0; while (name[j] && i < 100) disk_info[i++] = name[j++];
        disk_info[i++] = ' '; disk_info[i++] = '-'; disk_info[i++] = ' ';
        j = 0; while (model[j] && i < 100) disk_info[i++] = model[j++];
        disk_info[i++] = ' '; disk_info[i++] = '(';
        j = 0; while (size_str[j] && i < 120) disk_info[i++] = size_str[j++];
        disk_info[i++] = ')';
        disk_info[i] = '\0';

        inst_draw_text(wx + 20, content_y, disk_info, INSTALLER_TEXT_COLOR);
    }

    content_y += 40;
    inst_draw_text_bold(wx + 20, content_y, "Partition Scheme:", INSTALLER_TEXT_COLOR);
    content_y += 30;

    bool auto_selected = inst->partition_mode == 0;
    inst_draw_checkbox(wx + 40, content_y, auto_selected, false);
    inst_draw_text(wx + 65, content_y + 3, "Automatic Partitioning (Recommended)", INSTALLER_TEXT_COLOR);
    content_y += 25;
    inst_draw_text(wx + 65, content_y, "Creates EFI System Partition (512 MB) + Root Partition", 0x00606060);
    content_y += 35;

    bool manual_selected = inst->partition_mode == 1;
    inst_draw_checkbox(wx + 40, content_y, manual_selected, false);
    inst_draw_text(wx + 65, content_y + 3, "Manual Partitioning", INSTALLER_TEXT_COLOR);
    content_y += 25;
    inst_draw_text(wx + 65, content_y, "Advanced users can customize partition layout", 0x00606060);
    content_y += 40;

    inst_draw_text_bold(wx + 20, content_y, "Partition Layout Preview:", INSTALLER_TEXT_COLOR);
    content_y += 25;

    int32_t bar_x = wx + 40;
    int32_t bar_w = ww - 80;
    int32_t bar_h = 40;

    uint64_t total_size = inst->selected_disk >= 0 ?
                          inst->disks[inst->selected_disk].size_bytes : 0;
    int32_t efi_w = (bar_w * 512 * 1024 * 1024) / (total_size > 0 ? total_size : 1);
    if (efi_w < 30) efi_w = 30;
    if (efi_w > bar_w / 3) efi_w = bar_w / 3;

    inst_fill_rect(bar_x, content_y, efi_w, bar_h, 0x003498DB);
    inst_draw_rect(bar_x, content_y, efi_w, bar_h, 0x00206090);
    inst_draw_text(bar_x + 5, content_y + 14, "EFI", 0x00FFFFFF);

    inst_fill_rect(bar_x + efi_w, content_y, bar_w - efi_w, bar_h, 0x0027AE60);
    inst_draw_rect(bar_x + efi_w, content_y, bar_w - efi_w, bar_h, 0x00186030);
    inst_draw_text(bar_x + efi_w + 10, content_y + 14, "MayteraOS Root", 0x00FFFFFF);

    content_y += bar_h + 15;

    inst_draw_text(bar_x, content_y, "EFI System Partition: 512 MB (FAT32)", 0x003498DB);
    content_y += 20;

    if (total_size > 0) {
        char root_size[64];
        format_size(total_size - 512 * 1024 * 1024, root_size, sizeof(root_size));
        char detail[128];
        int i = 0, j;
        const char *prefix = "Root Partition: ";
        j = 0; while (prefix[j]) detail[i++] = prefix[j++];
        j = 0; while (root_size[j]) detail[i++] = root_size[j++];
        const char *suffix = " (ext4)";
        j = 0; while (suffix[j]) detail[i++] = suffix[j++];
        detail[i] = '\0';
        inst_draw_text(bar_x, content_y, detail, 0x0027AE60);
    }

    inst_draw_button(wx + 20, wy + wh - 50, 100, 35, "< Back", INSTALLER_BTN_SECONDARY, inst->hover_button == 0);
    inst_draw_button(wx + ww - 120, wy + wh - 50, 100, 35, "Next >", INSTALLER_BTN_PRIMARY, inst->hover_button == 1);
}

void installer_draw_user_setup(installer_t *inst) {
    if (!inst || !inst->window) return;

    int32_t wx, wy, ww, wh;
    window_get_content_bounds(inst->window, &wx, &wy, &ww, &wh);

    inst_fill_rect(wx, wy, ww, 60, INSTALLER_HEADER_COLOR);
    inst_draw_text_centered(wx, wy + 20, ww, "Create User Account", 0x00FFFFFF);

    int32_t form_x = wx + 40;
    int32_t form_y = wy + 85;
    int32_t label_w = 150;
    int32_t input_w = 300;
    int32_t row_h = 55;

    inst_draw_text(form_x, form_y + 8, "Full Name:", INSTALLER_TEXT_COLOR);
    inst_draw_input(form_x + label_w, form_y, input_w, 30,
                    inst->user.fullname, inst->focused_input == 0, false);
    form_y += row_h;

    inst_draw_text(form_x, form_y + 8, "Username:", INSTALLER_TEXT_COLOR);
    inst_draw_input(form_x + label_w, form_y, input_w, 30,
                    inst->user.username, inst->focused_input == 1, false);
    inst_draw_text(form_x + label_w + input_w + 10, form_y + 8, "(lowercase, no spaces)", 0x00808080);
    form_y += row_h;

    inst_draw_text(form_x, form_y + 8, "Password:", INSTALLER_TEXT_COLOR);
    inst_draw_input(form_x + label_w, form_y, input_w, 30,
                    inst->user.password, inst->focused_input == 2, true);
    form_y += row_h;

    inst_draw_text(form_x, form_y + 8, "Confirm Password:", INSTALLER_TEXT_COLOR);
    inst_draw_input(form_x + label_w, form_y, input_w, 30,
                    inst->user.password_confirm, inst->focused_input == 3, true);

    if (inst->user.password[0] && inst->user.password_confirm[0]) {
        bool match = true;
        const char *p1 = inst->user.password;
        const char *p2 = inst->user.password_confirm;
        while (*p1 || *p2) {
            if (*p1 != *p2) { match = false; break; }
            if (*p1) p1++;
            if (*p2) p2++;
        }
        if (match) {
            inst_draw_text(form_x + label_w + input_w + 10, form_y + 8, "Passwords match", 0x0027AE60);
        } else {
            inst_draw_text(form_x + label_w + input_w + 10, form_y + 8, "Passwords do not match!", 0x00E74C3C);
        }
    }
    form_y += row_h;

    inst_draw_text(form_x, form_y + 8, "Computer Name:", INSTALLER_TEXT_COLOR);
    inst_draw_input(form_x + label_w, form_y, input_w, 30,
                    inst->user.hostname, inst->focused_input == 4, false);
    form_y += row_h + 10;

    inst_draw_checkbox(form_x + label_w, form_y, inst->user.auto_login, inst->hover_checkbox);
    inst_draw_text(form_x + label_w + 25, form_y + 3, "Log in automatically", INSTALLER_TEXT_COLOR);

    inst_draw_button(wx + 20, wy + wh - 50, 100, 35, "< Back", INSTALLER_BTN_SECONDARY, inst->hover_button == 0);
    inst_draw_button(wx + ww - 120, wy + wh - 50, 100, 35, "Next >", INSTALLER_BTN_PRIMARY, inst->hover_button == 1);
}

void installer_draw_timezone(installer_t *inst) {
    if (!inst || !inst->window) return;

    int32_t wx, wy, ww, wh;
    window_get_content_bounds(inst->window, &wx, &wy, &ww, &wh);

    inst_fill_rect(wx, wy, ww, 60, INSTALLER_HEADER_COLOR);
    inst_draw_text_centered(wx, wy + 20, ww, "Timezone & Locale Settings", 0x00FFFFFF);

    int32_t form_x = wx + 40;
    int32_t form_y = wy + 85;
    int32_t dropdown_w = 300;
    int32_t row_h = 60;

    inst_draw_text_bold(form_x, form_y, "Timezone:", INSTALLER_TEXT_COLOR);
    form_y += 25;
    inst_draw_dropdown(form_x, form_y, dropdown_w, 30,
                       timezones[inst->locale.timezone_index],
                       inst->focused_dropdown == 0);
    form_y += row_h;

    inst_draw_text_bold(form_x, form_y, "Keyboard Layout:", INSTALLER_TEXT_COLOR);
    form_y += 25;
    inst_draw_dropdown(form_x, form_y, dropdown_w, 30,
                       keyboard_layout_names[inst->locale.keyboard_index],
                       inst->focused_dropdown == 1);
    form_y += row_h;

    inst_draw_text_bold(form_x, form_y, "Language:", INSTALLER_TEXT_COLOR);
    form_y += 25;
    inst_draw_dropdown(form_x, form_y, dropdown_w, 30,
                       languages[inst->locale.language_index],
                       inst->focused_dropdown == 2);
    form_y += row_h + 20;

    inst_fill_rect(form_x, form_y, ww - 80, 80, 0x00F8F8F8);
    inst_draw_rect(form_x, form_y, ww - 80, 80, 0x00C0C0C0);

    inst_draw_text_bold(form_x + 15, form_y + 10, "Configuration Summary:", INSTALLER_TEXT_COLOR);

    char summary[128];
    int i = 0, j;
    const char *tz_label = "  Timezone: ";
    j = 0; while (tz_label[j]) summary[i++] = tz_label[j++];
    j = 0; while (timezones[inst->locale.timezone_index][j]) summary[i++] = timezones[inst->locale.timezone_index][j++];
    summary[i] = '\0';
    inst_draw_text(form_x + 15, form_y + 30, summary, 0x00404040);

    i = 0;
    const char *kb_label = "  Keyboard: ";
    j = 0; while (kb_label[j]) summary[i++] = kb_label[j++];
    j = 0; while (keyboard_layout_names[inst->locale.keyboard_index][j]) summary[i++] = keyboard_layout_names[inst->locale.keyboard_index][j++];
    summary[i] = '\0';
    inst_draw_text(form_x + 15, form_y + 50, summary, 0x00404040);

    inst_draw_button(wx + 20, wy + wh - 50, 100, 35, "< Back", INSTALLER_BTN_SECONDARY, inst->hover_button == 0);
    inst_draw_button(wx + ww - 120, wy + wh - 50, 100, 35, "Install", INSTALLER_BTN_PRIMARY, inst->hover_button == 1);
}

void installer_draw_install(installer_t *inst) {
    if (!inst || !inst->window) return;

    int32_t wx, wy, ww, wh;
    window_get_content_bounds(inst->window, &wx, &wy, &ww, &wh);

    inst_fill_rect(wx, wy, ww, 60, INSTALLER_HEADER_COLOR);
    inst_draw_text_centered(wx, wy + 20, ww, "Installing MayteraOS", 0x00FFFFFF);

    int32_t content_y = wy + 90;

    inst_draw_text(wx + 40, content_y, "Overall Progress:", INSTALLER_TEXT_COLOR);
    content_y += 25;
    inst_draw_progress_bar(wx + 40, content_y, ww - 80, 30, inst->progress.percent);
    content_y += 50;

    inst_draw_text(wx + 40, content_y, "Current Operation:", INSTALLER_TEXT_COLOR);
    content_y += 25;
    inst_fill_rect(wx + 40, content_y, ww - 80, 30, 0x00F5F5F5);
    inst_draw_rect(wx + 40, content_y, ww - 80, 30, 0x00C0C0C0);
    inst_draw_text(wx + 50, content_y + 8, inst->progress.current_file, 0x00404040);
    content_y += 50;

    inst_draw_text_bold(wx + 40, content_y, "Installation Stages:", INSTALLER_TEXT_COLOR);
    content_y += 30;

    const char *stages[] = {
        "Format partitions",
        "Copy system files",
        "Install bootloader",
        "Configure system",
        "Create user account",
        "Generate caches"
    };

    for (int i = 0; i < 6; i++) {
        int stage_progress = (inst->progress.percent - i * 15) * 100 / 15;
        if (stage_progress < 0) stage_progress = 0;
        if (stage_progress > 100) stage_progress = 100;

        uint32_t color;
        const char *status;
        if (stage_progress == 100) {
            color = 0x0027AE60;
            status = "[Done]";
        } else if (stage_progress > 0) {
            color = 0x003498DB;
            status = "[In Progress]";
        } else {
            color = 0x00808080;
            status = "[Pending]";
        }

        inst_draw_text(wx + 60, content_y, stages[i], color);
        inst_draw_text(wx + ww - 150, content_y, status, color);
        content_y += 22;
    }

    if (inst->progress.error) {
        content_y = wy + wh - 100;
        inst_fill_rect(wx + 30, content_y, ww - 60, 40, 0x00FFEBEE);
        inst_draw_rect(wx + 30, content_y, ww - 60, 40, 0x00E74C3C);
        inst_draw_text(wx + 45, content_y + 12, inst->progress.error_msg, 0x00C62828);
    }
}

void installer_draw_complete(installer_t *inst) {
    if (!inst || !inst->window) return;

    int32_t wx, wy, ww, wh;
    window_get_content_bounds(inst->window, &wx, &wy, &ww, &wh);

    inst_fill_rect(wx, wy, ww, 70, 0x0027AE60);
    inst_draw_text_centered(wx, wy + 25, ww, "Installation Complete!", 0x00FFFFFF);

    int32_t icon_x = wx + ww / 2 - 40;
    int32_t icon_y = wy + 100;
    inst_fill_rect(icon_x, icon_y, 80, 80, 0x0027AE60);

    inst_fill_rect(icon_x + 15, icon_y + 45, 15, 5, 0x00FFFFFF);
    inst_fill_rect(icon_x + 25, icon_y + 35, 5, 20, 0x00FFFFFF);
    inst_fill_rect(icon_x + 30, icon_y + 25, 5, 25, 0x00FFFFFF);
    inst_fill_rect(icon_x + 35, icon_y + 20, 5, 25, 0x00FFFFFF);
    inst_fill_rect(icon_x + 40, icon_y + 25, 5, 20, 0x00FFFFFF);
    inst_fill_rect(icon_x + 45, icon_y + 30, 5, 15, 0x00FFFFFF);
    inst_fill_rect(icon_x + 50, icon_y + 35, 5, 10, 0x00FFFFFF);

    int32_t content_y = icon_y + 100;
    inst_draw_text_centered(wx, content_y, ww, "MayteraOS has been successfully installed on your computer.", INSTALLER_TEXT_COLOR);
    content_y += 30;
    inst_draw_text_centered(wx, content_y, ww, "Please remove the installation media and restart your computer.", INSTALLER_TEXT_COLOR);
    content_y += 50;

    inst_fill_rect(wx + 60, content_y, ww - 120, 120, 0x00F8F8F8);
    inst_draw_rect(wx + 60, content_y, ww - 120, 120, 0x00C0C0C0);

    inst_draw_text_bold(wx + 80, content_y + 15, "Installation Summary:", INSTALLER_TEXT_COLOR);

    char summary[64];
    int i, j;

    i = 0;
    const char *user_label = "User: ";
    j = 0; while (user_label[j]) summary[i++] = user_label[j++];
    j = 0; while (inst->user.username[j]) summary[i++] = inst->user.username[j++];
    summary[i] = '\0';
    inst_draw_text(wx + 80, content_y + 40, summary, 0x00404040);

    i = 0;
    const char *host_label = "Computer: ";
    j = 0; while (host_label[j]) summary[i++] = host_label[j++];
    j = 0; while (inst->user.hostname[j]) summary[i++] = inst->user.hostname[j++];
    summary[i] = '\0';
    inst_draw_text(wx + 80, content_y + 60, summary, 0x00404040);

    i = 0;
    const char *tz_label = "Timezone: ";
    j = 0; while (tz_label[j]) summary[i++] = tz_label[j++];
    j = 0; while (timezones[inst->locale.timezone_index][j]) summary[i++] = timezones[inst->locale.timezone_index][j++];
    summary[i] = '\0';
    inst_draw_text(wx + 80, content_y + 80, summary, 0x00404040);

    inst_draw_button(wx + ww / 2 - 80, wy + wh - 60, 160, 40, "Restart Now", INSTALLER_BTN_PRIMARY, inst->hover_button == 1);
}

// ============================================================================
// Disk Detection
// ============================================================================

// Human-readable fallback label for a target with no IDENTIFY model string
// (AHCI ports and USB disks). Kept deliberately tiny: no snprintf in this
// translation unit, and an unlabelled disk in a DESTRUCTIVE picker is worse
// than an ugly one.
static void inst_bus_label(const inst_target_t *t, char *out, uint32_t cap) {
    if (!out || cap < 8) return;
    const char *bus = (t->kind == INST_KIND_ATA)  ? "ATA disk "
                    : (t->kind == INST_KIND_AHCI) ? "AHCI port "
                    : (t->kind == INST_KIND_USB)  ? "USB disk "
                                                  : "disk ";
    uint32_t i = 0;
    while (bus[i] && i + 5 < cap) { out[i] = bus[i]; i++; }
    uint8_t v = t->index;
    if (v >= 100 && i + 1 < cap) out[i++] = (char)('0' + (v / 100));
    if (v >= 10  && i + 1 < cap) out[i++] = (char)('0' + ((v / 10) % 10));
    if (i + 1 < cap) out[i++] = (char)('0' + (v % 10));
    out[i] = '\0';
}

void installer_detect_disks(installer_t *inst) {
    if (!inst) return;

    kprintf("[INSTALLER] Detecting disks...\n");
    inst->disk_count = 0;
    inst->selected_disk = -1;

    // #306: enumerate EVERY bus we can write to, not just the two ATA channels.
    // The old loop was the reason the iMac showed "no disks": its internal
    // drive is AHCI, and ata_get_drive() cannot see it at all.
    inst_target_t targets[INST_MAX_TARGETS];
    int n = inst_enumerate_targets(targets, INST_MAX_TARGETS);
    if (n < 0) n = 0;

    // A disk smaller than the source ESP alone can never hold the install, so
    // it is worth hiding. This is a FLOOR, not the authoritative capacity
    // check: installer_do_install_target() computes the real ESP + ext2-root
    // requirement and returns -6 if the disk does not fit. Two checks with two
    // different jobs, deliberately not merged.
    uint64_t min_sectors = (uint64_t)g_fat_fs.part_sectors + 66;

    for (int ti = 0; ti < n && inst->disk_count < INSTALLER_MAX_DISKS; ti++) {
        inst_target_t *t = &targets[ti];

        if (!inst_target_installable(t, min_sectors)) {
            kprintf("[INSTALLER] skipping kind=%u idx=%u sectors=%llu (%s)\n",
                    t->kind, t->index, (unsigned long long)t->sectors,
                    t->is_boot ? "boot disk" : "below minimum size");
            continue;
        }

        installer_disk_t *disk = &inst->disks[inst->disk_count];

        disk->name[0] = 's';
        disk->name[1] = 'd';
        disk->name[2] = (char)('a' + (char)inst->disk_count);
        disk->name[3] = '\0';

        // Prefer the real IDENTIFY model string where we have one; otherwise
        // name the bus and index so two disks are never indistinguishable.
        const char *model = NULL;
        if (t->kind == INST_KIND_ATA) {
            ata_drive_t *drive = ata_get_drive((uint8_t)((t->index >> 1) & 1),
                                               (uint8_t)(t->index & 1));
            if (drive && drive->exists && drive->model[0])
                model = drive->model;
        }
        if (model) {
            int i = 0;
            while (model[i] && i < 63) { disk->model[i] = model[i]; i++; }
            disk->model[i] = '\0';
        } else {
            inst_bus_label(t, disk->model, sizeof(disk->model));
        }

        disk->size_bytes  = t->sectors * 512ULL;
        disk->sector_size = 512;
        disk->removable   = (t->kind == INST_KIND_USB);
        disk->selected    = false;
        disk->kind        = t->kind;
        disk->index       = t->index;
        disk->drive_id    = (t->kind == INST_KIND_ATA) ? (int)t->index : -1;

        kprintf("[INSTALLER] Found disk: %s - %s (%llu bytes, kind=%u idx=%u)\n",
                disk->name, disk->model, disk->size_bytes, t->kind, t->index);
        inst->disk_count++;
    }

    kprintf("[INSTALLER] Detected %u disk(s)\n", inst->disk_count);
}

// ============================================================================
// Installation Functions
// ============================================================================

void installer_next_step(installer_t *inst) {
    if (!inst) return;

    if (inst->current_step < INSTALL_STEP_COUNT - 1) {
        inst->current_step++;

        switch (inst->current_step) {
            case INSTALL_STEP_DISK_SELECT:
                installer_detect_disks(inst);
                break;

            case INSTALL_STEP_PARTITION:
                inst->partition_mode = 0;
                break;

            case INSTALL_STEP_INSTALL:
                installer_start_install(inst);
                break;

            default:
                break;
        }

        window_invalidate(inst->window);
        kprintf("[INSTALLER] Moved to step %d: %s\n",
                inst->current_step, step_titles[inst->current_step]);
    }
}

void installer_prev_step(installer_t *inst) {
    if (!inst) return;

    if (inst->current_step > INSTALL_STEP_WELCOME) {
        inst->current_step--;
        window_invalidate(inst->window);
        kprintf("[INSTALLER] Moved back to step %d: %s\n",
                inst->current_step, step_titles[inst->current_step]);
    }
}

// ============================================================================
// Install engine
//
// Strategy: reproduce the live image's TWO-PARTITION layout on the target and
// raw-clone both partitions sector-for-sector.
//
// The comment that used to sit here described a single GPT disk with one EF00
// FAT32 ESP that was "BOTH the EFI System Partition and the MayteraOS root (no
// /ROOTEXT2, so FAT is root)". That was true when it was written and has been
// FALSE since #365/b788. Every golden since then is 256MiB FAT ESP + ext2 root,
// and the invariant gate asserts exactly that ("ESP has no /APPS directory -
// userland is off the ESP"). Cloning only the ESP therefore produced a disk
// carrying the kernel and BOOTX64.EFI but NO /APPS, /CONFIG, wallpapers or
// fonts - and, because the cloned ESP still carries the ROOTEXT2 marker, one
// that actively told the kernel to mount an ext2 root that had never been
// written. It booted the kernel into nothing.
//
// Nothing caught it because the only trigger is a /CONFIG/AUTOINST.CFG marker
// that nothing sets, so the layout change sailed straight past this code.
//
// Cloning rather than formatting is still the right call: this kernel has no
// mkfs for FAT32 or ext2, so a filesystem cannot be created from scratch. Both
// source partitions are already proven-good images.
// ============================================================================

// EFI System Partition type GUID (C12A7328-F81F-11D2-BA4B-00A0C93EC93B) in the
// mixed-endian on-disk GPT byte order.
// Linux filesystem data (0FC63DAF-8483-4772-8E79-3D69D8477DE4) in the same
// mixed-endian on-disk GPT byte order as the ESP GUID below: first three fields
// little-endian, last two big-endian. This is p2's type code, 8300.
static const uint8_t INST_LINUX_TYPE_GUID[16] = {
    0xAF, 0x3D, 0xC6, 0x0F, 0x83, 0x84, 0x72, 0x47,
    0x8E, 0x79, 0x3D, 0x69, 0xD8, 0x47, 0x7D, 0xE4
};

static const uint8_t INST_ESP_TYPE_GUID[16] = {
    0x28, 0x73, 0x2A, 0xC1, 0x1F, 0xF8, 0xD2, 0x11,
    0xBA, 0x4B, 0x00, 0xA0, 0xC9, 0x3E, 0xC9, 0x3B
};

static uint32_t inst_crc32(const uint8_t *data, uint32_t len) {
    uint32_t crc = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++) {
            uint32_t mask = (crc & 1u) ? 0xEDB88320u : 0u;  // standard CRC-32 reflected poly
            crc = (crc >> 1) ^ mask;
        }
    }
    return crc ^ 0xFFFFFFFFu;
}

static void inst_put_le16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
}
static void inst_put_le32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static void inst_put_le64(uint8_t *p, uint64_t v) {
    for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (8 * i));
}

static uint64_t inst_rdtsc(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

// Fill 16 bytes with a pseudo-unique GUID derived from the CPU timestamp.
static void inst_make_guid(uint8_t *out, uint64_t salt) {
    uint64_t r = inst_rdtsc() ^ salt;
    r |= 1;
    for (int i = 0; i < 16; i++) {
        r ^= r << 13; r ^= r >> 7; r ^= r << 17;
        out[i] = (uint8_t)r;
    }
}

// ---------------------------------------------------------------------------
// #306: thin accessors for rustkern/instdisk.rs. These exist rather than
// mirroring ata_drive_t / usb_msc_device_t in Rust, because a second copy of a
// C struct layout is a second thing to drift (the ext2_get_is_dir reasoning).
// ---------------------------------------------------------------------------
int inst_ata_exists(uint8_t channel, uint8_t drive) {
    ata_drive_t *d = ata_get_drive(channel, drive);
    return (d && d->exists && d->type == ATA_TYPE_ATA) ? 1 : 0;
}
uint64_t inst_ata_sectors(uint8_t channel, uint8_t drive) {
    ata_drive_t *d = ata_get_drive(channel, drive);
    return (d && d->exists) ? (uint64_t)d->sectors : 0;
}
// #306/#32: device IDENTITY for dedup. The ATA driver and the AHCI driver can
// both claim the SAME PCI function - on q35 the ATA driver logs "Found PCI
// storage controller: 8086:2922 at 00:1f.2 ... Secondary in compatibility
// mode" for the very controller AHCI initializes - so one physical disk gets
// enumerated twice. Capacity alone cannot separate a duplicate from two
// genuinely identical drives; the IDENTIFY serial can.
//
// Both copy at most 20 chars + NUL into a caller-supplied 21-byte buffer, and
// write an empty string rather than leaving the buffer untouched, so the Rust
// side never reads uninitialised bytes.
void inst_ata_serial(uint8_t channel, uint8_t drive, char *out) {
    if (!out) return;
    out[0] = '\0';
    ata_drive_t *d = ata_get_drive(channel, drive);
    if (!d || !d->exists) return;
    int i = 0;
    while (i < 20 && d->serial[i]) { out[i] = d->serial[i]; i++; }
    out[i] = '\0';
}

void inst_ahci_serial(int port, char *out) {
    if (!out) return;
    out[0] = '\0';
    const char *sn = ahci_get_serial(port);
    if (!sn) return;
    int i = 0;
    while (i < 20 && sn[i]) { out[i] = sn[i]; i++; }
    out[i] = '\0';
}

uint64_t inst_usb_sectors(int index) {
    extern usb_msc_device_t *usb_msc_get_device(int index);
    usb_msc_device_t *d = usb_msc_get_device(index);
    if (!d || !d->ready) return 0;
    return (uint64_t)d->num_blocks;
}
// Which device did we boot from? The installer must never offer it: cloning a
// disk onto itself corrupts the source it is still reading.
int inst_boot_kind(void) {
    extern int blk_root_is_usb(void);
    return blk_root_is_usb() ? 2 /*INST_KIND_USB*/ : 0 /*INST_KIND_ATA*/;
}
int inst_boot_index(void) {
    extern int blk_root_usb_index(void);
    if (blk_root_is_usb()) return blk_root_usb_index();
    return g_fat_fs.drive;   /* already a combined 0-3 index */
}

// ---------------------------------------------------------------------------
// Sector I/O dispatched by TARGET KIND. Previously ATA-only, which is why
// widening enumeration alone would have changed nothing: an AHCI disk could be
// listed and still not written.
// ---------------------------------------------------------------------------
static int inst_kind_read(uint8_t kind, uint8_t index, uint32_t lba, uint8_t cnt, void *buf) {
    if (kind == 1) {            /* AHCI */
        return ahci_read((int)index, lba, cnt, buf) == 0 ? cnt : -1;
    }
    if (kind == 2) {            /* USB mass storage */
        extern usb_msc_device_t *usb_msc_get_device(int index);
        usb_msc_device_t *d = usb_msc_get_device((int)index);
        if (!d) return -1;
        return usb_msc_read(d, 0, lba, buf, cnt) == 0 ? cnt : -1;
    }
    uint8_t ch = (uint8_t)(index >> 1), u = (uint8_t)(index & 1);
    if (ata_dma_available(ch, u)) return ata_read_sectors_dma(ch, u, lba, cnt, buf);
    return ata_read_sectors(ch, u, lba, cnt, buf);
}
static int inst_kind_write(uint8_t kind, uint8_t index, uint32_t lba, uint8_t cnt, const void *buf) {
    if (kind == 1) {            /* AHCI */
        return ahci_write((int)index, lba, cnt, buf) == 0 ? cnt : -1;
    }
    if (kind == 2) {            /* USB mass storage */
        extern usb_msc_device_t *usb_msc_get_device(int index);
        usb_msc_device_t *d = usb_msc_get_device((int)index);
        if (!d) return -1;
        return usb_msc_write(d, 0, lba, buf, cnt) == 0 ? cnt : -1;
    }
    uint8_t ch = (uint8_t)(index >> 1), u = (uint8_t)(index & 1);
    if (ata_dma_available(ch, u)) return ata_write_sectors_dma(ch, u, lba, cnt, buf);
    return ata_write_sectors(ch, u, lba, cnt, buf);
}

static void inst_build_gpt_header(uint8_t *sec, uint64_t disk_sectors,
                                  uint64_t my_lba, uint64_t alt_lba,
                                  uint64_t entry_lba, uint32_t parr_crc,
                                  const uint8_t *disk_guid) {
    memset(sec, 0, 512);
    sec[0]='E'; sec[1]='F'; sec[2]='I'; sec[3]=' ';
    sec[4]='P'; sec[5]='A'; sec[6]='R'; sec[7]='T';
    inst_put_le32(sec + 8, 0x00010000);          // revision 1.0
    inst_put_le32(sec + 12, 92);                 // header size
    inst_put_le32(sec + 16, 0);                  // header CRC (computed below)
    inst_put_le32(sec + 20, 0);                  // reserved
    inst_put_le64(sec + 24, my_lba);             // MyLBA
    inst_put_le64(sec + 32, alt_lba);            // AlternateLBA
    inst_put_le64(sec + 40, 34);                 // FirstUsableLBA
    inst_put_le64(sec + 48, disk_sectors - 34);  // LastUsableLBA
    memcpy(sec + 56, disk_guid, 16);             // DiskGUID
    inst_put_le64(sec + 72, entry_lba);          // PartitionEntryLBA
    inst_put_le32(sec + 80, 128);                // NumberOfPartitionEntries
    inst_put_le32(sec + 84, 128);                // SizeOfPartitionEntry
    inst_put_le32(sec + 88, parr_crc);           // PartitionEntryArrayCRC32
    inst_put_le32(sec + 16, inst_crc32(sec, 92));// HeaderCRC32
}

// inst_disk_read/inst_disk_write (ATA-only) removed: every call site now
// goes through inst_kind_read/inst_kind_write, which dispatch on target kind.


// Legacy ATA-only entry point, kept so the #306 boot selftest and any other
// drive-id caller still work. It builds a descriptor and defers.
int installer_do_install(int target_drive_id, installer_progress_fn cb, void *ctx) {
    if (target_drive_id < 0 || target_drive_id > 3) {
        if (cb) cb(ctx, 0, "ERROR: invalid target drive id");
        kprintf("[INSTALLER] invalid target drive id %d\n", target_drive_id);
        return -2;
    }
    ata_drive_t *td = ata_get_drive((uint8_t)((target_drive_id >> 1) & 1),
                                    (uint8_t)(target_drive_id & 1));
    if (!td || !td->exists || td->type != ATA_TYPE_ATA) {
        if (cb) cb(ctx, 0, "ERROR: target disk not present");
        kprintf("[INSTALLER] target ATA drive %d not present\n", target_drive_id);
        return -4;
    }
    inst_target_t t;
    t.kind    = INST_KIND_ATA;
    t.index   = (uint8_t)target_drive_id;
    t.is_boot = 0;
    t._pad    = 0;
    t.sectors = td->sectors;
    return installer_do_install_target(&t, cb, ctx);
}

int installer_do_install_target(const inst_target_t *t, installer_progress_fn cb, void *ctx) {
    uint8_t *sec = NULL, *parr = NULL, *buf = NULL;

    #define INST_PROG(p, m) do { \
        if (cb) cb(ctx, (p), (m)); \
        kprintf("[INSTALLER] %3d%% %s\n", (p), (m)); \
    } while (0)
    #define INST_FAIL(code, m) do { \
        INST_PROG(0, m); \
        if (sec)  { kfree(sec); } \
        if (parr) { kfree(parr); } \
        if (buf)  { kfree(buf); } \
        return (code); \
    } while (0)

    if (!g_fat_fs.mounted)
        INST_FAIL(-1, "ERROR: no source filesystem mounted");

    int src_drive = g_fat_fs.drive;
    if (!t)
        INST_FAIL(-2, "ERROR: no install target given");

    uint8_t src_ch = (uint8_t)((src_drive >> 1) & 1), src_u = (uint8_t)(src_drive & 1);

    // Address BOTH sides by KIND rather than by ATA channel/drive. The source
    // is a USB stick on every live boot, and the target is AHCI on every iMac;
    // the old ata_* calls could reach neither.
    uint8_t src_kind = (uint8_t)inst_boot_kind();
    uint8_t src_idx  = (uint8_t)inst_boot_index();
    uint8_t tgt_kind = t->kind;
    uint8_t tgt_idx  = t->index;

    // Refuse the disk we booted from, by DESCRIPTOR rather than by ATA id.
    // Two independent guards: the enumerator flags the boot disk, and we
    // re-derive it here, because a caller can hand us a hand-built target
    // that never went through inst_enumerate_targets().
    if (t->is_boot)
        INST_FAIL(-3, "ERROR: refusing to install onto the source/boot disk");
    if (tgt_kind == src_kind && tgt_idx == src_idx)
        INST_FAIL(-3, "ERROR: refusing to install onto the source/boot disk");

    uint64_t disk_sectors = t->sectors;
    if (disk_sectors == 0)
        INST_FAIL(-4, "ERROR: target disk reports zero capacity");
    uint32_t src_start = g_fat_fs.part_start_lba;
    uint32_t part_sectors = g_fat_fs.part_sectors;
    if (part_sectors == 0) part_sectors = g_fat_fs.total_sectors;
    if (part_sectors == 0)
        INST_FAIL(-5, "ERROR: source partition size unknown");

    // ---- ext2 ROOT geometry on the SOURCE disk (#365 two-partition layout) --
    // ext2_find_partition() parses the source GPT for the Linux partition;
    // ext2_end_bytes() is part_start*512 + blocks_count*block_size, i.e. the
    // absolute end of the mounted volume. Their difference is its length.
    uint32_t root_start = 0, root_sectors = 0;
    int have_root = 0;
    if (ext2_is_mounted() && ext2_find_partition(src_ch, src_u, &root_start) == 0) {
        uint64_t end_b = ext2_end_bytes();
        if (end_b > (uint64_t)root_start * 512ULL) {
            uint64_t len = (end_b / 512ULL) - (uint64_t)root_start;
            if (len > 0 && len < 0xFFFFFFFFULL) {
                root_sectors = (uint32_t)len;
                have_root = 1;
            }
        }
    }
    if (!have_root)
        INST_FAIL(-15, "ERROR: could not locate the ext2 root partition to clone");

    uint32_t tgt_start = 2048;                       // p1 (ESP)
    uint32_t tgt_root_start = tgt_start + part_sectors;
    // 4KiB-align p2 so the ext2 volume starts on an 8-sector boundary.
    tgt_root_start = (tgt_root_start + 7u) & ~7u;

    // Budget BOTH partitions plus the 33-sector backup GPT at the end of the
    // disk. The old check counted only the ESP, so a disk large enough for p1
    // and too small for p2 passed and then failed mid-clone.
    uint64_t need = (uint64_t)tgt_root_start + root_sectors + 33 + 1;
    if (disk_sectors < need)
        INST_FAIL(-6, "ERROR: target disk is too small for the ESP + root partitions");

    kprintf("[INSTALLER] src kind=%u idx=%u (ch%d u%d) lba=%u sectors=%u -> tgt kind=%u idx=%u disk_sectors=%llu\n",
            src_kind, src_idx, src_ch, src_u, src_start, part_sectors,
            tgt_kind, tgt_idx, (unsigned long long)disk_sectors);

    sec  = (uint8_t *)kmalloc(512);
    parr = (uint8_t *)kmalloc(128 * 128);   // 128 entries * 128 bytes = 16 KB
    buf  = (uint8_t *)kmalloc(64 * 512);    // 32 KB clone chunk
    if (!sec || !parr || !buf)
        INST_FAIL(-7, "ERROR: out of memory");

    INST_PROG(2, "Writing partition table (GPT)...");

    // ---- Partition entry array: entry 0 = ESP, entry 1 = ext2 root --------
    // Two entries, matching what the invariant gate requires of every golden:
    // exactly 2 GPT partitions, p1=EF00(ESP), p2=8300(root).
    memset(parr, 0, 128 * 128);

    memcpy(parr + 0, INST_ESP_TYPE_GUID, 16);
    inst_make_guid(parr + 16, 0x1111);
    inst_put_le64(parr + 32, tgt_start);
    inst_put_le64(parr + 40, (uint64_t)tgt_start + part_sectors - 1);
    inst_put_le64(parr + 48, 0);
    { const char *nm = "MAYTERA_ESP";
      for (int i = 0; nm[i]; i++) inst_put_le16(parr + 56 + i * 2, (uint16_t)nm[i]); }

    uint8_t *e1 = parr + 128;
    memcpy(e1 + 0, INST_LINUX_TYPE_GUID, 16);
    inst_make_guid(e1 + 16, 0x3333);
    inst_put_le64(e1 + 32, tgt_root_start);
    inst_put_le64(e1 + 40, (uint64_t)tgt_root_start + root_sectors - 1);
    inst_put_le64(e1 + 48, 0);
    { const char *nm = "MAYTERA_ROOT";
      for (int i = 0; nm[i]; i++) inst_put_le16(e1 + 56 + i * 2, (uint16_t)nm[i]); }
    uint32_t parr_crc = inst_crc32(parr, 128 * 128);

    uint8_t disk_guid[16];
    inst_make_guid(disk_guid, 0x2222);

    uint64_t backup_arr_lba = disk_sectors - 33;
    uint64_t backup_hdr_lba = disk_sectors - 1;

    // ---- Primary GPT header (LBA 1) + array (LBA 2..33) ----
    inst_build_gpt_header(sec, disk_sectors, 1, backup_hdr_lba, 2, parr_crc, disk_guid);
    if (inst_kind_write(tgt_kind, tgt_idx, 1, 1, sec) != 1)
        INST_FAIL(-8, "ERROR: failed to write primary GPT header");
    for (uint32_t i = 0; i < 32; i++)
        if (inst_kind_write(tgt_kind, tgt_idx, 2 + i, 1, parr + i * 512) != 1)
            INST_FAIL(-9, "ERROR: failed to write GPT partition array");

    // ---- Backup array + header at end of disk ----
    for (uint32_t i = 0; i < 32; i++)
        if (inst_kind_write(tgt_kind, tgt_idx, (uint32_t)backup_arr_lba + i, 1, parr + i * 512) != 1)
            INST_FAIL(-10, "ERROR: failed to write backup GPT array");
    inst_build_gpt_header(sec, disk_sectors, backup_hdr_lba, 1, backup_arr_lba, parr_crc, disk_guid);
    if (inst_kind_write(tgt_kind, tgt_idx, (uint32_t)backup_hdr_lba, 1, sec) != 1)
        INST_FAIL(-11, "ERROR: failed to write backup GPT header");

    // ---- Protective MBR (LBA 0) ----
    memset(sec, 0, 512);
    sec[446] = 0x00;                                  // not bootable
    sec[447] = 0x00; sec[448] = 0x02; sec[449] = 0x00;// start CHS
    sec[450] = 0xEE;                                  // GPT protective type
    sec[451] = 0xFF; sec[452] = 0xFF; sec[453] = 0xFF;// end CHS
    inst_put_le32(sec + 454, 1);                      // start LBA
    { uint64_t sz = disk_sectors - 1; if (sz > 0xFFFFFFFFu) sz = 0xFFFFFFFFu;
      inst_put_le32(sec + 458, (uint32_t)sz); }
    sec[510] = 0x55; sec[511] = 0xAA;
    if (inst_kind_write(tgt_kind, tgt_idx, 0, 1, sec) != 1)
        INST_FAIL(-12, "ERROR: failed to write protective MBR");

    // ---- Raw-clone BOTH partitions ----------------------------------------
    // Progress is split across the two by their real sector counts, so the bar
    // reflects work done rather than partitions finished.
    uint64_t total_sectors = (uint64_t)part_sectors + (uint64_t)root_sectors;
    uint64_t copied_all = 0;

    for (int part = 0; part < 2; part++) {
        uint32_t remaining = (part == 0) ? part_sectors : root_sectors;
        uint32_t soff      = (part == 0) ? src_start    : root_start;
        uint32_t toff      = (part == 0) ? tgt_start    : tgt_root_start;
        uint32_t since_yield = 0;

        INST_PROG(10 + (int)(copied_all * 85 / total_sectors),
                  part == 0 ? "Copying boot partition..."
                            : "Copying system files...");

        while (remaining > 0) {
            uint8_t cnt = remaining > 64 ? 64 : (uint8_t)remaining;
            if (inst_kind_read(src_kind, src_idx, soff, cnt, buf) != cnt)
                INST_FAIL(-13, "ERROR: read from source disk failed");
            if (inst_kind_write(tgt_kind, tgt_idx, toff, cnt, buf) != cnt)
                INST_FAIL(-14, "ERROR: write to target disk failed");
            soff += cnt; toff += cnt; remaining -= cnt;
            copied_all += cnt; since_yield += cnt;
            if (since_yield >= 4096) {  // ~2 MB between progress updates
                since_yield = 0;
                int pct = 10 + (int)(copied_all * 85 / total_sectors);
                INST_PROG(pct, part == 0 ? "Copying boot partition..."
                                         : "Copying system files...");
                // Cooperative yield inside a BULK COPY, not a poll: there is no
                // condition being waited on, so a wait-queue is not the right
                // primitive here. This bounds how long the copy monopolises the
                // CPU between progress updates.
                proc_sleep(1);
            }
        }
    }

    INST_PROG(96, "Flushing disk caches...");
    ata_flush_all();

    INST_PROG(100, "Installation complete. You may reboot from the new disk.");
    if (sec) kfree(sec);
    if (parr) kfree(parr);
    if (buf) kfree(buf);
    return 0;

    #undef INST_PROG
    #undef INST_FAIL
}

// GUI progress callback: mirrors engine progress into the wizard's progress UI.
static void installer_gui_progress(void *ctx, int percent, const char *msg) {
    installer_t *inst = (installer_t *)ctx;
    if (!inst) return;
    inst->progress.percent = percent;
    int i = 0;
    while (msg[i] && i < 127) { inst->progress.current_file[i] = msg[i]; i++; }
    inst->progress.current_file[i] = '\0';
    if (percent == 0) {  // error
        i = 0;
        while (msg[i] && i < 255) { inst->progress.error_msg[i] = msg[i]; i++; }
        inst->progress.error_msg[i] = '\0';
        inst->progress.error = true;
    }
    window_invalidate(inst->window);
}

void installer_start_install(installer_t *inst) {
    if (!inst) return;

    kprintf("[INSTALLER] Starting installation...\n");
    inst->installing = true;
    inst->progress.percent = 0;
    inst->progress.complete = false;
    inst->progress.error = false;

    // #306: hand the engine the DESCRIPTOR the user actually picked. This used
    // to pass an ATA drive_id, which is -1 for every AHCI and USB disk, so
    // selecting one of those silently installed nowhere.
    inst_target_t tgt;
    bool have_target = false;
    if (inst->selected_disk >= 0 && inst->selected_disk < (int)inst->disk_count) {
        installer_disk_t *d = &inst->disks[inst->selected_disk];
        tgt.kind    = d->kind;
        tgt.index   = d->index;
        tgt.is_boot = 0;
        tgt._pad    = 0;
        tgt.sectors = d->size_bytes / 512ULL;
        have_target = true;
    }

    window_invalidate(inst->window);

    int rc = have_target
           ? installer_do_install_target(&tgt, installer_gui_progress, inst)
           : -2;
    if (!have_target)
        kprintf("[INSTALLER] install requested with no disk selected\n");

    inst->installing = false;
    if (rc == 0) {
        inst->progress.percent = 100;
        inst->progress.complete = true;
        inst->current_step = INSTALL_STEP_COMPLETE;
        kprintf("[INSTALLER] Installation complete!\n");
    } else {
        inst->progress.error = true;
        kprintf("[INSTALLER] Installation FAILED rc=%d\n", rc);
    }
    window_invalidate(inst->window);
}

// ============================================================================
// task #306: deferred headless auto-install worker. Gated on /CONFIG/AUTOINST.CFG.
// When that marker file exists on the booted FAT root, this installs MayteraOS
// onto the first non-boot ATA disk and logs the result to serial. This lets the
// full partition+clone flow be driven headlessly (serial is always reliable,
// unlike the compositor-starved RC service).
// ============================================================================
static void installer_autoinstall_worker(void *arg) {
    (void)arg;
    proc_sleep(6000);  // let the desktop settle and the ATA bus go idle

    uint32_t sz = 0;
    void *cfg = fat_read_file(&g_fat_fs, "/CONFIG/AUTOINST.CFG", &sz);
    if (!cfg) return;  // no marker -> silent no-op
    kfree(cfg);

    kprintf("\n========== OS INSTALL SELFTEST (task #306) ==========\n");

    // #306: this worker used to scan the two ATA channels with ata_get_drive()
    // and report "need a second ATA disk". That made the automated install
    // path blind to AHCI and USB in exactly the way the GUI path was, so on the
    // iMac - whose internal disk is AHCI - it could never find a target. Use
    // the same enumerator the picker uses.
    inst_target_t targets[INST_MAX_TARGETS];
    int n = inst_enumerate_targets(targets, INST_MAX_TARGETS);
    kprintf("[OSINSTALL] enumerated %d target(s)\n", n);

    // Same floor the picker applies; the engine still owns the authoritative
    // ESP + root capacity check and returns -6 if the disk does not fit.
    uint64_t min_sectors = (uint64_t)g_fat_fs.part_sectors + 66;
    int pick = -1;
    for (int i = 0; i < n; i++) {
        int ok = inst_target_installable(&targets[i], min_sectors);
        kprintf("[OSINSTALL]   kind=%u idx=%u sectors=%llu boot=%u installable=%d\n",
                targets[i].kind, targets[i].index,
                (unsigned long long)targets[i].sectors, targets[i].is_boot, ok);
        if (pick < 0 && ok) pick = i;
    }

    if (pick < 0) {
        kprintf("[OSINSTALL] no installable target disk found\n");
        kprintf("========== OS INSTALL SELFTEST END ==========\n");
        return;
    }

    kprintf("[OSINSTALL] installing to kind=%u idx=%u\n",
            targets[pick].kind, targets[pick].index);
    int r = installer_do_install_target(&targets[pick], NULL, NULL);
    if (r == 0) kprintf("[OSINSTALL] RESULT: SUCCESS - kind=%u idx=%u is now bootable\n",
                        targets[pick].kind, targets[pick].index);
    else        kprintf("[OSINSTALL] RESULT: FAILED rc=%d\n", r);
    kprintf("========== OS INSTALL SELFTEST END ==========\n");
}

void installer_start_deferred_autoinstall(void) {
    extern int proc_create_ex(const char *name, void (*entry)(void *), void *arg,
                              int priority, uint32_t stack_size);
    proc_create_ex("osinstall", installer_autoinstall_worker, 0, 1, 256 * 1024);
}

bool installer_copy_files(installer_t *inst) {
    if (!inst) return false;
    kprintf("[INSTALLER] Copying system files...\n");
    return true;
}

bool installer_install_bootloader(installer_t *inst) {
    if (!inst) return false;
    kprintf("[INSTALLER] Installing bootloader...\n");
    return true;
}

bool installer_configure_system(installer_t *inst) {
    if (!inst) return false;
    kprintf("[INSTALLER] Configuring system...\n");
    return true;
}

bool installer_create_user(installer_t *inst) {
    if (!inst) return false;
    kprintf("[INSTALLER] Creating user: %s\n", inst->user.username);
    return true;
}

// ============================================================================
// Event Handlers
// ============================================================================

void installer_handle_mouse_move(installer_t *inst, int32_t x, int32_t y) {
    if (!inst || !inst->window) return;

    int32_t wx, wy, ww, wh;
    window_get_content_bounds(inst->window, &wx, &wy, &ww, &wh);

    int old_hover_disk = inst->hover_disk;
    int old_hover_button = inst->hover_button;

    inst->hover_disk = -1;
    inst->hover_button = -1;
    inst->hover_checkbox = false;

    if (x >= wx + 20 && x < wx + 120 && y >= wy + wh - 50 && y < wy + wh - 15) {
        if (inst->current_step > INSTALL_STEP_WELCOME &&
            inst->current_step != INSTALL_STEP_INSTALL) {
            inst->hover_button = 0;
        }
    }
    if (x >= wx + ww - 120 && x < wx + ww - 20 && y >= wy + wh - 50 && y < wy + wh - 15) {
        inst->hover_button = 1;
    }
    if (inst->current_step == INSTALL_STEP_COMPLETE) {
        if (x >= wx + ww / 2 - 80 && x < wx + ww / 2 + 80 &&
            y >= wy + wh - 60 && y < wy + wh - 20) {
            inst->hover_button = 1;
        }
    }

    if (inst->current_step == INSTALL_STEP_DISK_SELECT) {
        if (x >= wx + ww - 240 && x < wx + ww - 140 && y >= wy + wh - 50 && y < wy + wh - 15) {
            inst->hover_button = 2;
        }

        int32_t list_y = wy + 110;
        for (uint32_t i = 0; i < inst->disk_count && i < 8; i++) {
            if (x >= wx + 20 && x < wx + ww - 20 && y >= list_y && y < list_y + 55) {
                inst->hover_disk = i;
            }
            list_y += 60;
        }
    }

    if (inst->current_step == INSTALL_STEP_USER_SETUP) {
        int32_t form_x = wx + 40;
        int32_t form_y = wy + 85;
        int32_t label_w = 150;
        int32_t row_h = 55;
        int32_t check_y = form_y + 5 * row_h + 10;

        if (x >= form_x + label_w && x < form_x + label_w + 20 &&
            y >= check_y && y < check_y + 20) {
            inst->hover_checkbox = true;
        }
    }

    if (old_hover_disk != inst->hover_disk || old_hover_button != inst->hover_button) {
        window_invalidate(inst->window);
    }
}

void installer_handle_mouse_down(installer_t *inst, int32_t x, int32_t y, uint32_t button) {
    if (!inst || !inst->window || button != 1) return;

    int32_t wx, wy, ww, wh;
    window_get_content_bounds(inst->window, &wx, &wy, &ww, &wh);

    if (inst->hover_button == 0 && inst->current_step > INSTALL_STEP_WELCOME &&
        inst->current_step != INSTALL_STEP_INSTALL) {
        installer_prev_step(inst);
        return;
    }

    if (inst->hover_button == 1) {
        bool can_proceed = true;

        switch (inst->current_step) {
            case INSTALL_STEP_DISK_SELECT:
                can_proceed = (inst->selected_disk >= 0);
                break;

            case INSTALL_STEP_USER_SETUP:
                can_proceed = (inst->user.username[0] != '\0');
                if (can_proceed && inst->user.password[0]) {
                    const char *p1 = inst->user.password;
                    const char *p2 = inst->user.password_confirm;
                    while (*p1 || *p2) {
                        if (*p1 != *p2) { can_proceed = false; break; }
                        if (*p1) p1++;
                        if (*p2) p2++;
                    }
                }
                break;

            case INSTALL_STEP_COMPLETE:
                kprintf("[INSTALLER] Restarting system...\n");
                break;

            default:
                break;
        }

        if (can_proceed) {
            installer_next_step(inst);
        }
        return;
    }

    if (inst->current_step == INSTALL_STEP_DISK_SELECT && inst->hover_disk >= 0) {
        inst->selected_disk = inst->hover_disk;
        window_invalidate(inst->window);
        return;
    }

    if (inst->current_step == INSTALL_STEP_DISK_SELECT && inst->hover_button == 2) {
        installer_detect_disks(inst);
        window_invalidate(inst->window);
        return;
    }

    if (inst->current_step == INSTALL_STEP_USER_SETUP) {
        int32_t form_x = wx + 40;
        int32_t form_y = wy + 85;
        int32_t label_w = 150;
        int32_t input_w = 300;
        int32_t row_h = 55;

        for (int i = 0; i < 5; i++) {
            int32_t input_y = form_y + i * row_h;
            if (x >= form_x + label_w && x < form_x + label_w + input_w &&
                y >= input_y && y < input_y + 30) {
                inst->focused_input = i;
                window_invalidate(inst->window);
                return;
            }
        }

        int32_t check_y = form_y + 5 * row_h + 10;
        if (x >= form_x + label_w && x < form_x + label_w + 20 &&
            y >= check_y && y < check_y + 20) {
            inst->user.auto_login = !inst->user.auto_login;
            window_invalidate(inst->window);
            return;
        }
    }

    if (inst->current_step == INSTALL_STEP_TIMEZONE) {
        int32_t form_x = wx + 40;
        int32_t form_y = wy + 110;
        int32_t dropdown_w = 300;
        int32_t row_h = 60;

        if (x >= form_x && x < form_x + dropdown_w &&
            y >= form_y && y < form_y + 30) {
            inst->locale.timezone_index = (inst->locale.timezone_index + 1) % NUM_TIMEZONES;
            window_invalidate(inst->window);
            return;
        }
        form_y += row_h;

        if (x >= form_x && x < form_x + dropdown_w &&
            y >= form_y && y < form_y + 30) {
            inst->locale.keyboard_index = (inst->locale.keyboard_index + 1) % NUM_KEYBOARD_LAYOUTS;
            window_invalidate(inst->window);
            return;
        }
        form_y += row_h;

        if (x >= form_x && x < form_x + dropdown_w &&
            y >= form_y && y < form_y + 30) {
            inst->locale.language_index = (inst->locale.language_index + 1) % NUM_LANGUAGES;
            window_invalidate(inst->window);
            return;
        }
    }

    if (inst->current_step == INSTALL_STEP_PARTITION) {
        int32_t content_y = wy + 145;

        if (x >= wx + 40 && x < wx + 60 &&
            y >= content_y && y < content_y + 20) {
            inst->partition_mode = 0;
            window_invalidate(inst->window);
            return;
        }

        content_y += 60;
        if (x >= wx + 40 && x < wx + 60 &&
            y >= content_y && y < content_y + 20) {
            inst->partition_mode = 1;
            window_invalidate(inst->window);
            return;
        }
    }
}

void installer_handle_key(installer_t *inst, uint32_t keycode, char key_char) {
    if (!inst) return;

    if (inst->current_step != INSTALL_STEP_USER_SETUP) return;

    char *target = NULL;
    int max_len = 0;

    switch (inst->focused_input) {
        case 0: target = inst->user.fullname; max_len = INSTALLER_FULLNAME_MAX; break;
        case 1: target = inst->user.username; max_len = INSTALLER_USERNAME_MAX; break;
        case 2: target = inst->user.password; max_len = INSTALLER_PASSWORD_MAX; break;
        case 3: target = inst->user.password_confirm; max_len = INSTALLER_PASSWORD_MAX; break;
        case 4: target = inst->user.hostname; max_len = INSTALLER_HOSTNAME_MAX; break;
        default: return;
    }

    int len = 0;
    while (target[len]) len++;

    if (keycode == 0x0E) {
        if (len > 0) {
            target[len - 1] = '\0';
            window_invalidate(inst->window);
        }
    } else if (keycode == 0x0F || keycode == 0x1C) {
        inst->focused_input = (inst->focused_input + 1) % 5;
        window_invalidate(inst->window);
    } else if (key_char >= 32 && key_char < 127 && len < max_len - 1) {
        if (inst->focused_input == 1 || inst->focused_input == 4) {
            if (key_char >= 'A' && key_char <= 'Z') {
                key_char = key_char - 'A' + 'a';
            }
            if (!((key_char >= 'a' && key_char <= 'z') ||
                  (key_char >= '0' && key_char <= '9') ||
                  key_char == '-' || key_char == '_')) {
                return;
            }
        }
        target[len] = key_char;
        target[len + 1] = '\0';
        window_invalidate(inst->window);
    }
}

// ============================================================================
// Window Event Handler
// ============================================================================

// Draw callback (fired each frame by wm_draw_apps).
static void installer_on_draw(void *app_data) {
    installer_t *inst = (installer_t *)app_data;
    if (!inst || !inst->window) return;

    switch (inst->current_step) {
        case INSTALL_STEP_WELCOME:     installer_draw_welcome(inst);     break;
        case INSTALL_STEP_DISK_SELECT: installer_draw_disk_select(inst); break;
        case INSTALL_STEP_PARTITION:   installer_draw_partition(inst);   break;
        case INSTALL_STEP_USER_SETUP:  installer_draw_user_setup(inst);  break;
        case INSTALL_STEP_TIMEZONE:    installer_draw_timezone(inst);    break;
        case INSTALL_STEP_INSTALL:     installer_draw_install(inst);     break;
        case INSTALL_STEP_COMPLETE:    installer_draw_complete(inst);    break;
        default: break;
    }
}

// Event callback (mouse/keyboard/close).
static void installer_on_event(void *app_data, gui_event_t *event) {
    installer_t *inst = (installer_t *)app_data;
    if (!inst || !event) return;

    switch (event->type) {
        case EVENT_MOUSE_MOVE:
            installer_handle_mouse_move(inst, event->mouse_x, event->mouse_y);
            break;
        case EVENT_MOUSE_DOWN:
            installer_handle_mouse_down(inst, event->mouse_x, event->mouse_y,
                                        (event->mouse_buttons & MOUSE_BUTTON_LEFT) ? 1 : 0);
            break;
        case EVENT_KEY_DOWN:
            installer_handle_key(inst, event->keycode, event->key_char);
            break;
        case EVENT_WINDOW_CLOSE:
            if (!inst->installing) installer_close();
            break;
        default:
            break;
    }
}

static void installer_on_destroy(void *app_data) {
    (void)app_data;
    // Window teardown is driven from installer_close(); nothing extra to free here.
}

// ============================================================================
// Public API
// ============================================================================

void installer_init(void) {
    if (g_installer) return;

    kprintf("[INSTALLER] Initializing MayteraOS Installer v1.8.0...\n");

    g_installer = (installer_t *)kmalloc(sizeof(installer_t));
    if (!g_installer) {
        kprintf("[INSTALLER] Failed to allocate installer state\n");
        return;
    }

    for (int i = 0; i < (int)sizeof(installer_t); i++) {
        ((uint8_t *)g_installer)[i] = 0;
    }

    g_installer->current_step = INSTALL_STEP_WELCOME;
    g_installer->selected_disk = -1;
    g_installer->selected_partition = -1;
    g_installer->hover_disk = -1;
    g_installer->hover_button = -1;
    g_installer->focused_input = 0;
    g_installer->partition_mode = 0;
    g_installer->app_id = -1;

    const char *default_host = "maytera-pc";
    int i = 0;
    while (default_host[i]) {
        g_installer->user.hostname[i] = default_host[i];
        i++;
    }
    g_installer->user.hostname[i] = '\0';

    g_installer->locale.timezone_index = 1;
    g_installer->locale.keyboard_index = 0;
    g_installer->locale.language_index = 0;

    g_installer->window = window_create(
        "MayteraOS Installer",
        100, 50,
        INSTALLER_WINDOW_WIDTH,
        INSTALLER_WINDOW_HEIGHT
    );

    if (!g_installer->window) {
        kprintf("[INSTALLER] Failed to create installer window\n");
        kfree(g_installer);
        g_installer = NULL;
        return;
    }

    g_installer->app_id = wm_register_app(
        g_installer->window,
        g_installer,
        installer_on_event,
        installer_on_draw,
        installer_on_destroy
    );
    if (g_installer->app_id < 0) {
        kprintf("[INSTALLER] Failed to register installer with window manager\n");
        window_destroy(g_installer->window);
        kfree(g_installer);
        g_installer = NULL;
        return;
    }
    window_set_focus(g_installer->window);

    g_installer_running = true;
    kprintf("[INSTALLER] Installer initialized successfully\n");
}

void installer_run(void) {
    installer_init();
}

void installer_close(void) {
    if (!g_installer) return;

    kprintf("[INSTALLER] Closing installer...\n");

    if (g_installer->app_id >= 0) {
        wm_unregister_app(g_installer->app_id);
        g_installer->app_id = -1;
    }
    if (g_installer->window) {
        window_destroy(g_installer->window);
    }

    kfree(g_installer);
    g_installer = NULL;
    g_installer_running = false;
}

bool installer_is_running(void) {
    return g_installer_running;
}
