// mouse.c - PS/2 Mouse driver implementation
#include "mouse.h"
#include "../serial.h"
#include "../string.h"
#include "../cpu/idt.h"
#include "../cpu/pic.h"
#include "../video/framebuffer.h"

// PS/2 Controller ports
#define PS2_DATA_PORT    0x60
#define PS2_STATUS_PORT  0x64
#define PS2_CMD_PORT     0x64

// PS/2 Commands
#define PS2_CMD_READ_CONFIG   0x20
#define PS2_CMD_WRITE_CONFIG  0x60
#define PS2_CMD_DISABLE_PORT2 0xA7
#define PS2_CMD_ENABLE_PORT2  0xA8
#define PS2_CMD_TEST_PORT2    0xA9
#define PS2_CMD_WRITE_PORT2   0xD4

// Mouse commands
#define MOUSE_CMD_RESET       0xFF
#define MOUSE_CMD_RESEND      0xFE
#define MOUSE_CMD_SET_DEFAULT 0xF6
#define MOUSE_CMD_DISABLE     0xF5
#define MOUSE_CMD_ENABLE      0xF4
#define MOUSE_CMD_SET_RATE    0xF3
#define MOUSE_CMD_GET_ID      0xF2
#define MOUSE_CMD_SET_REMOTE  0xF0
#define MOUSE_CMD_SET_WRAP    0xEE
#define MOUSE_CMD_RESET_WRAP  0xEC
#define MOUSE_CMD_READ_DATA   0xEB
#define MOUSE_CMD_SET_STREAM  0xEA
#define MOUSE_CMD_STATUS_REQ  0xE9
#define MOUSE_CMD_SET_RES     0xE8
#define MOUSE_CMD_SET_SCALE21 0xE7
#define MOUSE_CMD_SET_SCALE11 0xE6

// Mouse responses
#define MOUSE_ACK  0xFA
#define MOUSE_NACK 0xFE

// Mouse sensitivity: 1=slow, 5=1:1, 10=fast. Default raised to 7 so the pointer
// feels quick and responsive out of the box (desktop UX pass); the compositor
// overrides this from the persisted UIPROFIL mouse_sens value on boot.
static int g_mouse_sensitivity = 7;

void mouse_set_sensitivity(int s) {
    if (s < 1) s = 1;
    if (s > 10) s = 10;
    g_mouse_sensitivity = s;
}

int mouse_get_sensitivity(void) {
    return g_mouse_sensitivity;
}

// Mouse state
static mouse_state_t g_mouse;
extern volatile uint64_t timer_ticks;
// While timer_ticks < this, ignore real PS/2 packets so a synthetic
// click injected via mouse_inject_button() is not overwritten mid-click.
volatile uint64_t g_mouse_synth_until = 0;
static int32_t mouse_min_x = 0;
static int32_t mouse_min_y = 0;
static int32_t mouse_max_x = 1920;
static int32_t mouse_max_y = 1080;

// Packet assembly
static uint8_t mouse_packet[4];
static int mouse_packet_idx = 0;
static int mouse_has_wheel = 0;

// Global variables for external access (fb_syscall.c)
int32_t mouse_x = 0;
int32_t mouse_y = 0;
uint8_t mouse_buttons = 0;

// Wait for PS/2 controller to be ready for input
static void ps2_wait_write(void) {
    int timeout = 100000;
    while (timeout-- && (inb(PS2_STATUS_PORT) & 0x02)) {
        __asm__ volatile("pause");
    }
}

// Wait for PS/2 controller to have data
static int ps2_wait_read(void) {
    int timeout = 100000;
    while (timeout-- && !(inb(PS2_STATUS_PORT) & 0x01)) {
        __asm__ volatile("pause");
    }
    return timeout > 0;
}

// Send command to PS/2 controller
static void ps2_send_cmd(uint8_t cmd) {
    ps2_wait_write();
    outb(PS2_CMD_PORT, cmd);
}

// Send data to PS/2 data port
static void ps2_send_data(uint8_t data) {
    ps2_wait_write();
    outb(PS2_DATA_PORT, data);
}

// Read from PS/2 data port
static uint8_t ps2_read_data(void) {
    if (ps2_wait_read()) {
        return inb(PS2_DATA_PORT);
    }
    return 0xFF;
}

// Send command to mouse (via port 2)
static uint8_t mouse_send_cmd(uint8_t cmd) {
    ps2_send_cmd(PS2_CMD_WRITE_PORT2);
    ps2_send_data(cmd);
    return ps2_read_data();
}

// Process mouse packet
static void mouse_process_packet(void) {
    if (g_mouse_synth_until && timer_ticks < g_mouse_synth_until) return;
    uint8_t flags = mouse_packet[0];

    // Store previous button state
    g_mouse.prev_buttons = g_mouse.buttons;

    // Extract button state
    g_mouse.buttons = flags & 0x07;

    // Extract movement
    int32_t dx = mouse_packet[1];
    int32_t dy = mouse_packet[2];

    // Handle sign extension
    if (flags & 0x10) dx |= 0xFFFFFF00;
    if (flags & 0x20) dy |= 0xFFFFFF00;

    // Y is inverted in PS/2
    dy = -dy;

    // Accumulate delta (cleared by mouse_get_state_and_clear)
    g_mouse.dx += dx;
    g_mouse.dy += dy;

    // Extract scroll wheel data (4th byte if wheel mouse)
    if (mouse_has_wheel) {
        // 4th byte is signed scroll value
        int8_t scroll = (int8_t)mouse_packet[3];
        g_mouse.scroll = scroll;
    } else {
        g_mouse.scroll = 0;
    }

    // Apply sensitivity scaling (5 = 1:1, values above/below speed up/slow down)
    dx = (dx * g_mouse_sensitivity) / 5;
    dy = (dy * g_mouse_sensitivity) / 5;

    // Update position
    g_mouse.x += dx;
    g_mouse.y += dy;

    // Clamp to bounds
    if (g_mouse.x < mouse_min_x) g_mouse.x = mouse_min_x;
    if (g_mouse.x > mouse_max_x) g_mouse.x = mouse_max_x;
    if (g_mouse.y < mouse_min_y) g_mouse.y = mouse_min_y;
    if (g_mouse.y > mouse_max_y) g_mouse.y = mouse_max_y;

    // Update global variables for external access
    mouse_x = g_mouse.x;
    mouse_y = g_mouse.y;
    mouse_buttons = g_mouse.buttons;
}

// Mouse interrupt handler
static void mouse_handler(interrupt_frame_t *frame) {
    (void)frame;

    uint8_t status = inb(PS2_STATUS_PORT);

    // Check if data is from mouse (bit 5 set)
    if (!(status & 0x20)) {
        pic_send_eoi(12);
        return;
    }

    uint8_t data = inb(PS2_DATA_PORT);

    // First byte should have bit 3 set (always 1)
    if (mouse_packet_idx == 0 && !(data & 0x08)) {
        // Resync - discard byte
        pic_send_eoi(12);
        return;
    }

    mouse_packet[mouse_packet_idx++] = data;

    int packet_size = mouse_has_wheel ? 4 : 3;

    if (mouse_packet_idx >= packet_size) {
        mouse_process_packet();
        mouse_packet_idx = 0;
    }

    pic_send_eoi(12);
}

// Try to enable scroll wheel (IntelliMouse)
static int mouse_enable_wheel(void) {
    // Magic sequence to enable wheel
    mouse_send_cmd(MOUSE_CMD_SET_RATE);
    mouse_send_cmd(200);
    mouse_send_cmd(MOUSE_CMD_SET_RATE);
    mouse_send_cmd(100);
    mouse_send_cmd(MOUSE_CMD_SET_RATE);
    mouse_send_cmd(80);

    // Get device ID
    mouse_send_cmd(MOUSE_CMD_GET_ID);
    uint8_t id = ps2_read_data();

    return (id == 3);  // ID 3 = wheel mouse
}

// Initialize PS/2 mouse
int mouse_init(void) {
    kprintf("[MOUSE] Initializing PS/2 mouse...\n");

    // Initialize state
    memset(&g_mouse, 0, sizeof(g_mouse));
    g_mouse.x = mouse_max_x / 2;
    g_mouse.y = mouse_max_y / 2;

    // Set bounds from framebuffer
    uint32_t fb_w = fb_get_width();
    uint32_t fb_h = fb_get_height();
    if (fb_w > 0 && fb_h > 0) {
        mouse_max_x = fb_w - 1;
        mouse_max_y = fb_h - 1;
        g_mouse.x = fb_w / 2;
        g_mouse.y = fb_h / 2;
    }

    // Enable second PS/2 port
    ps2_send_cmd(PS2_CMD_ENABLE_PORT2);

    // Read config
    ps2_send_cmd(PS2_CMD_READ_CONFIG);
    uint8_t config = ps2_read_data();

    // Enable IRQ12 (mouse interrupt)
    config |= 0x02;  // Enable second PS/2 port interrupt
    config &= ~0x20; // Enable second PS/2 port clock

    ps2_send_cmd(PS2_CMD_WRITE_CONFIG);
    ps2_send_data(config);

    // Reset mouse
    uint8_t response = mouse_send_cmd(MOUSE_CMD_RESET);
    if (response != MOUSE_ACK) {
        kprintf("[MOUSE] Reset failed (response: 0x%02x)\n", response);
        return -1;
    }

    // Wait for self-test
    ps2_read_data();  // Should be 0xAA (test passed)
    ps2_read_data();  // Should be 0x00 (device ID)

    // Try to enable scroll wheel
    mouse_has_wheel = mouse_enable_wheel();
    if (mouse_has_wheel) {
        kprintf("[MOUSE] Scroll wheel enabled\n");
    }

    // Set sample rate to the PS/2 maximum (200 samples/sec) for smooth, low
    // latency cursor motion. This runs AFTER the IntelliMouse wheel-enable magic
    // sequence (which leaves the rate at 80), so it is the effective final rate.
    // (desktop UX pass - mouse feel)
    mouse_send_cmd(MOUSE_CMD_SET_RATE);
    mouse_send_cmd(200);  // 200 samples/sec (max)

    // Set resolution
    mouse_send_cmd(MOUSE_CMD_SET_RES);
    mouse_send_cmd(3);  // 8 counts/mm

    // Set scaling 1:1
    mouse_send_cmd(MOUSE_CMD_SET_SCALE11);

    // Enable data reporting
    response = mouse_send_cmd(MOUSE_CMD_ENABLE);
    if (response != MOUSE_ACK) {
        kprintf("[MOUSE] Enable failed\n");
        return -1;
    }

    // Register interrupt handler (IRQ 12 = INT 44)
    idt_register_handler(44, mouse_handler);

    // Enable IRQ 12 in PIC
    pic_enable_irq(12);

    kprintf("[MOUSE] Initialized at (%d, %d), bounds 0,0 to %d,%d\n",
            g_mouse.x, g_mouse.y, mouse_max_x, mouse_max_y);

    return 0;
}

// Get current mouse state
void mouse_get_state(mouse_state_t *state) {
    if (state) {
        *state = g_mouse;
    }
}

// Get current mouse state and clear deltas (for exclusive consumers like DOOM)
void mouse_get_state_and_clear(mouse_state_t *state) {
    __asm__ volatile("cli");
    if (state) {
        *state = g_mouse;
    }
    g_mouse.dx = 0;
    g_mouse.dy = 0;
    g_mouse.scroll = 0;
    __asm__ volatile("sti");
}

// Test/automation hook: synthesize an absolute cursor position + button state.
// The compositor polls mouse_get_state() every frame and replays it through the
// real input path (sys_inject_mouse), so this produces a genuine click. Used by
// the RemoteCtrl `click x y` command for headless click testing.
void mouse_inject_button(int32_t x, int32_t y, int down) {
    __asm__ volatile("cli");
    g_mouse.prev_buttons = g_mouse.buttons;
    g_mouse.x = x; g_mouse.y = y;
    g_mouse.buttons = down ? 1u : 0u;
    mouse_x = x; mouse_y = y; mouse_buttons = g_mouse.buttons;
    g_mouse_synth_until = timer_ticks + 60;   /* ~240ms at 250Hz */
    __asm__ volatile("sti");
}

// #307: inject a USB-HID boot mouse report (relative deltas + button bitmap +
// wheel). Feeds the SAME g_mouse state that the PS/2 packet path updates, so the
// compositor treats USB and PS/2 mice identically. HID Y is positive-down, the
// same as screen coordinates, so (unlike PS/2) it is NOT inverted here.
void mouse_inject_hid(int dx, int dy, uint8_t buttons, int wheel) {
    __asm__ volatile("cli");
    g_mouse.prev_buttons = g_mouse.buttons;
    g_mouse.buttons = buttons & 0x07;

    g_mouse.dx += dx;
    g_mouse.dy += dy;
    g_mouse.scroll = (int8_t)wheel;

    int32_t sx = (dx * g_mouse_sensitivity) / 5;
    int32_t sy = (dy * g_mouse_sensitivity) / 5;
    g_mouse.x += sx;
    g_mouse.y += sy;

    if (g_mouse.x < mouse_min_x) g_mouse.x = mouse_min_x;
    if (g_mouse.x > mouse_max_x) g_mouse.x = mouse_max_x;
    if (g_mouse.y < mouse_min_y) g_mouse.y = mouse_min_y;
    if (g_mouse.y > mouse_max_y) g_mouse.y = mouse_max_y;

    mouse_x = g_mouse.x;
    mouse_y = g_mouse.y;
    mouse_buttons = g_mouse.buttons;
    __asm__ volatile("sti");
}

// Get mouse position
void mouse_get_position(int32_t *x, int32_t *y) {
    if (x) *x = g_mouse.x;
    if (y) *y = g_mouse.y;
}

// Set mouse position
void mouse_set_position(int32_t x, int32_t y) {
    g_mouse.x = x;
    g_mouse.y = y;

    // Clamp
    if (g_mouse.x < mouse_min_x) g_mouse.x = mouse_min_x;
    if (g_mouse.x > mouse_max_x) g_mouse.x = mouse_max_x;
    if (g_mouse.y < mouse_min_y) g_mouse.y = mouse_min_y;
    if (g_mouse.y > mouse_max_y) g_mouse.y = mouse_max_y;

    // Update global variables
    mouse_x = g_mouse.x;
    mouse_y = g_mouse.y;
}

// Set mouse bounds
void mouse_set_bounds(int32_t min_x, int32_t min_y, int32_t max_x, int32_t max_y) {
    mouse_min_x = min_x;
    mouse_min_y = min_y;
    mouse_max_x = max_x;
    mouse_max_y = max_y;

    // Re-clamp current position
    if (g_mouse.x < mouse_min_x) g_mouse.x = mouse_min_x;
    if (g_mouse.x > mouse_max_x) g_mouse.x = mouse_max_x;
    if (g_mouse.y < mouse_min_y) g_mouse.y = mouse_min_y;
    if (g_mouse.y > mouse_max_y) g_mouse.y = mouse_max_y;

    // Update global variables
    mouse_x = g_mouse.x;
    mouse_y = g_mouse.y;
}

// Check if button is pressed
int mouse_button_pressed(uint8_t button) {
    return (g_mouse.buttons & button) != 0;
}

// Get current button state
uint8_t mouse_get_buttons(void) {
    return g_mouse.buttons;
}

// Check if button was just clicked
int mouse_button_clicked(uint8_t button) {
    return (g_mouse.buttons & button) && !(g_mouse.prev_buttons & button);
}

// Check if button was just released
int mouse_button_released(uint8_t button) {
    return !(g_mouse.buttons & button) && (g_mouse.prev_buttons & button);
}

// Get scroll wheel delta and clear it
int8_t mouse_get_scroll(void) {
    int8_t scroll = g_mouse.scroll;
    g_mouse.scroll = 0;
    return scroll;
}

// Check if scroll wheel is supported
int mouse_has_scroll_wheel(void) {
    return mouse_has_wheel;
}

// Poll mouse (for systems without interrupts working)
void mouse_poll(void) {
    uint8_t status = inb(PS2_STATUS_PORT);

    while (status & 0x01) {
        if (status & 0x20) {
            // Data from mouse
            uint8_t data = inb(PS2_DATA_PORT);

            if (mouse_packet_idx == 0 && !(data & 0x08)) {
                // Resync
                status = inb(PS2_STATUS_PORT);
                continue;
            }

            mouse_packet[mouse_packet_idx++] = data;

            int packet_size = mouse_has_wheel ? 4 : 3;
            if (mouse_packet_idx >= packet_size) {
                mouse_process_packet();
                mouse_packet_idx = 0;
            }
        } else {
            // Data from keyboard - read and discard
            inb(PS2_DATA_PORT);
        }

        status = inb(PS2_STATUS_PORT);
    }
}

// Check if mouse data available
int mouse_has_data(void) {
    uint8_t status = inb(PS2_STATUS_PORT);
    return (status & 0x21) == 0x21;  // Data available and from mouse
}
