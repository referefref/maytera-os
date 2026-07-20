// audioconfig - Audio Configuration for MayteraOS (user-space version)
// Sound settings with equalizer and device selection
#include "../../libc/maytera.h"
#include "../../libc/gui.h"
#include "../../libc/theme.h"

#define WIN_W 450
#define WIN_H 400
#define MARGIN 16
#define SECTION_H 28
#define SLIDER_H 20
#define SLIDER_W 200

// EQ bands
#define EQ_BANDS 8
static const char *eq_labels[EQ_BANDS] = {
    "60Hz", "170Hz", "310Hz", "600Hz", "1kHz", "3kHz", "6kHz", "12kHz"
};

// Audio state
static int win = -1;
static int master_volume = 80;
static int eq_values[EQ_BANDS] = {50, 50, 50, 50, 50, 50, 50, 50};  // 0-100
static int muted = 0;
static int current_device = 0;
static int dragging_slider = -1;  // -1=none, 0=master, 1-8=eq bands

// Device list
static const char *devices[] = {
    "Intel HDA (Built-in)",
    "USB Audio Device",
    "HDMI Audio Output",
    "Bluetooth Headphones"
};
#define DEVICE_COUNT 4

// Draw section header
static void draw_section(int y, const char *title) {
    win_draw_rect(win, 0, y, WIN_W, SECTION_H, THEME_BG_SECONDARY);
    win_draw_text(win, MARGIN, y + 6, title, THEME_TEXT_PRIMARY);
}

// Draw horizontal slider
static void draw_slider(int x, int y, int width, int value, int hover) {
    // Track
    win_draw_rect(win, x, y + (SLIDER_H - THEME_SLIDER_TRACK_H) / 2,
                  width, THEME_SLIDER_TRACK_H, THEME_SLIDER_TRACK);
    
    // Fill
    int fill_w = (value * width) / 100;
    win_draw_rect(win, x, y + (SLIDER_H - THEME_SLIDER_TRACK_H) / 2,
                  fill_w, THEME_SLIDER_TRACK_H, THEME_SLIDER_FILL);
    
    // Thumb
    int thumb_x = x + fill_w - THEME_SLIDER_THUMB_SIZE / 2;
    int thumb_y = y + (SLIDER_H - THEME_SLIDER_THUMB_SIZE) / 2;
    uint32_t thumb_color = hover ? THEME_SLIDER_THUMB_HOVER : THEME_SLIDER_THUMB;
    win_draw_rect(win, thumb_x, thumb_y, THEME_SLIDER_THUMB_SIZE, THEME_SLIDER_THUMB_SIZE, thumb_color);
}

// Draw vertical EQ slider
static void draw_eq_slider(int x, int y, int value, int hover) {
    int slider_h = 100;
    int slider_w = 24;
    
    // Track
    win_draw_rect(win, x + (slider_w - 4) / 2, y, 4, slider_h, THEME_SLIDER_TRACK);
    
    // Fill from center
    int center_y = y + slider_h / 2;
    int fill_h = ((value - 50) * (slider_h / 2)) / 50;
    if (fill_h > 0) {
        win_draw_rect(win, x + (slider_w - 4) / 2, center_y - fill_h, 4, fill_h, THEME_SLIDER_FILL);
    } else if (fill_h < 0) {
        win_draw_rect(win, x + (slider_w - 4) / 2, center_y, 4, -fill_h, THEME_ERROR);
    }
    
    // Thumb
    int thumb_y = y + slider_h - (value * slider_h) / 100 - 6;
    uint32_t thumb_color = hover ? THEME_SLIDER_THUMB_HOVER : THEME_SLIDER_THUMB;
    win_draw_rect(win, x, thumb_y, slider_w, 12, thumb_color);
    
    // Center line
    win_draw_rect(win, x, center_y - 1, slider_w, 2, THEME_WINDOW_BORDER);
}

// Draw output device section
static void draw_device_section(int y) {
    draw_section(y, "Output Device");
    
    int item_y = y + SECTION_H + 8;
    for (int i = 0; i < DEVICE_COUNT; i++) {
        // Radio button
        int rb_x = MARGIN;
        int rb_y = item_y + i * 24;
        
        // Circle outline
        gui_draw_rect_outline(win, rb_x, rb_y, 16, 16, THEME_CHECKBOX_BORDER);
        
        // Fill if selected
        if (i == current_device) {
            win_draw_rect(win, rb_x + 4, rb_y + 4, 8, 8, THEME_ACCENT);
        }
        
        // Label
        win_draw_text(win, rb_x + 24, rb_y, devices[i], THEME_TEXT_PRIMARY);
    }
}

// Draw master volume section
static void draw_volume_section(int y) {
    draw_section(y, "Master Volume");
    
    int slider_y = y + SECTION_H + 12;
    
    // Mute button
    uint32_t mute_color = muted ? THEME_ERROR : THEME_BUTTON_BG;
    win_draw_rect(win, MARGIN, slider_y, 40, 24, mute_color);
    win_draw_text(win, MARGIN + 4, slider_y + 4, muted ? "ON" : "Mute", THEME_BUTTON_TEXT);
    
    // Volume slider
    int slider_x = MARGIN + 56;
    draw_slider(slider_x, slider_y, SLIDER_W, muted ? 0 : master_volume, dragging_slider == 0);
    
    // Volume percentage
    char vol_str[8];
    vol_str[0] = '0' + (master_volume / 100);
    vol_str[1] = '0' + ((master_volume / 10) % 10);
    vol_str[2] = '0' + (master_volume % 10);
    vol_str[3] = '%';
    vol_str[4] = '\0';
    win_draw_text(win, slider_x + SLIDER_W + 12, slider_y + 2, vol_str, THEME_TEXT_PRIMARY);
}

// Draw equalizer section
static void draw_eq_section(int y) {
    draw_section(y, "Equalizer");
    
    int eq_y = y + SECTION_H + 12;
    int band_spacing = (WIN_W - 2 * MARGIN) / EQ_BANDS;
    
    for (int i = 0; i < EQ_BANDS; i++) {
        int bx = MARGIN + i * band_spacing + (band_spacing - 24) / 2;
        
        // Slider
        draw_eq_slider(bx, eq_y, eq_values[i], dragging_slider == i + 1);
        
        // Label
        int label_w = gui_string_width(eq_labels[i]);
        win_draw_text(win, bx + 12 - label_w / 2, eq_y + 108, eq_labels[i], THEME_TEXT_SECONDARY);
        
        // Value
        char val_str[8];
        int db = eq_values[i] - 50;  // -50 to +50 range -> dB
        if (db >= 0) {
            val_str[0] = '+';
            val_str[1] = '0' + (db / 10);
            val_str[2] = '0' + (db % 10);
        } else {
            val_str[0] = '-';
            val_str[1] = '0' + ((-db) / 10);
            val_str[2] = '0' + ((-db) % 10);
        }
        val_str[3] = '\0';
        win_draw_text(win, bx + 4, eq_y + 124, val_str, THEME_TEXT_SECONDARY);
    }
    
    // Reset EQ button
    int btn_x = WIN_W - MARGIN - 80;
    win_draw_rect(win, btn_x, eq_y + 105, 72, 28, THEME_BUTTON_BG);
    win_draw_text(win, btn_x + 8, eq_y + 111, "Reset EQ", THEME_BUTTON_TEXT);
}

// Full redraw
static void draw_all(void) {
    // Background
    win_draw_rect(win, 0, 0, WIN_W, WIN_H, THEME_BG_PRIMARY);
    
    // Sections
    draw_device_section(0);
    draw_volume_section(124);
    draw_eq_section(200);
    
    win_invalidate(win);
}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    
    // Create window
    win = win_create("Audio Settings", 120, 80, WIN_W, WIN_H);
    if (win < 0) {
        printf("Failed to create window\n");
        return 1;
    }
    
    printf("Audio Settings window created (handle=%d)\n", win);
    
    // Initial draw
    draw_all();
    
    // Event loop
    gui_event_t event;
    int running = 1;
    int win_x = 120, win_y = 80;
    
    while (running) {
        int event_type = win_get_event(win, &event, 100);
        if (event_type == 0) continue;
        
        switch (event.type) {
            case EVENT_REDRAW:
                draw_all();
                break;
                
            case EVENT_WINDOW_CLOSE:
                running = 0;
                break;
                
            case EVENT_KEY_DOWN:
                if (event.key_char == 27) {
                    running = 0;
                } else if (event.key_char == 'm' || event.key_char == 'M') {
                    muted = !muted;
                    draw_all();
                }
                break;
                
            case EVENT_MOUSE_DOWN:
                if (event.mouse_buttons & MOUSE_BUTTON_LEFT) {
                    int lx = event.mouse_x;
                    int ly = event.mouse_y;
                    
                    // Check device selection (y: 0-124)
                    if (ly >= SECTION_H + 8 && ly < 124) {
                        int item = (ly - SECTION_H - 8) / 24;
                        if (item >= 0 && item < DEVICE_COUNT) {
                            current_device = item;
                            draw_all();
                        }
                    }
                    
                    // Check mute button (y: 124 + 28 + 12 = 164 to 188)
                    if (ly >= 164 && ly < 188 && lx >= MARGIN && lx < MARGIN + 40) {
                        muted = !muted;
                        draw_all();
                    }
                    
                    // Check master volume slider
                    if (ly >= 164 && ly < 184 && lx >= MARGIN + 56 && lx < MARGIN + 56 + SLIDER_W) {
                        dragging_slider = 0;
                        master_volume = ((lx - MARGIN - 56) * 100) / SLIDER_W;
                        if (master_volume < 0) master_volume = 0;
                        if (master_volume > 100) master_volume = 100;
                        draw_all();
                    }
                    
                    // Check EQ sliders
                    int eq_y = 200 + SECTION_H + 12;
                    if (ly >= eq_y && ly < eq_y + 100) {
                        int band_spacing = (WIN_W - 2 * MARGIN) / EQ_BANDS;
                        for (int i = 0; i < EQ_BANDS; i++) {
                            int bx = MARGIN + i * band_spacing;
                            if (lx >= bx && lx < bx + band_spacing) {
                                dragging_slider = i + 1;
                                eq_values[i] = 100 - ((ly - eq_y) * 100) / 100;
                                if (eq_values[i] < 0) eq_values[i] = 0;
                                if (eq_values[i] > 100) eq_values[i] = 100;
                                draw_all();
                                break;
                            }
                        }
                    }
                    
                    // Check Reset EQ button
                    int btn_x = WIN_W - MARGIN - 80;
                    if (ly >= eq_y + 105 && ly < eq_y + 133 && lx >= btn_x && lx < btn_x + 72) {
                        for (int i = 0; i < EQ_BANDS; i++) {
                            eq_values[i] = 50;
                        }
                        draw_all();
                    }
                }
                break;
                
            case EVENT_MOUSE_MOVE:
                if (dragging_slider >= 0) {
                    int lx = event.mouse_x;
                    int ly = event.mouse_y;
                    
                    if (dragging_slider == 0) {
                        // Master volume
                        master_volume = ((lx - MARGIN - 56) * 100) / SLIDER_W;
                        if (master_volume < 0) master_volume = 0;
                        if (master_volume > 100) master_volume = 100;
                        draw_all();
                    } else {
                        // EQ slider
                        int eq_y = 200 + SECTION_H + 12;
                        eq_values[dragging_slider - 1] = 100 - ((ly - eq_y) * 100) / 100;
                        if (eq_values[dragging_slider - 1] < 0) eq_values[dragging_slider - 1] = 0;
                        if (eq_values[dragging_slider - 1] > 100) eq_values[dragging_slider - 1] = 100;
                        draw_all();
                    }
                }
                break;
                
            case EVENT_MOUSE_UP:
                dragging_slider = -1;
                break;
                
            default:
                break;
        }
    }
    
    win_destroy(win);
    printf("Audio Settings closed\n");
    
    return 0;
}
