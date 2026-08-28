// isr.c - Interrupt Service Routines implementation
#include "isr.h"
#include "mono.h"
#include "pic.h"
#include "apic.h"        // #62: lapic_eoi() for the redundant tick source
#include "../fs/bootlog.h"  // #62: the arming result must reach the stick
#include "../serial.h"
#include "../proc/process.h"
#include "../drivers/keymod.h"   // KEY_MOD_*: one definition, shared with drivers/keyboard.h
#include "../sync/spinlock.h"  // shared lock primitive: the DOS tap now has >1 writer
#include "../drivers/sysvol.h"   // #162: the ONE system volume/mute state

// Timer tick count
volatile uint64_t timer_ticks = 0;

// Interrupt flag (Ctrl+C)
volatile int interrupt_requested = 0;

// Keyboard buffer
#define KEYBOARD_BUFFER_SIZE 256
// #243: SIXTEEN bits wide, not eight. The cooked-code namespace ran out of
// byte: 0x00-0x7F is ASCII, 0x80-0x8F is arrows+F-keys, 0x90-0x9D is releases
// and modifiers, 0xA0-0xFE is `printable | 0x80` releases. There was no free
// byte left for Home/End/PgUp/PgDn/Insert/Delete, which is WHY they were
// forwarded as their raw set-1 make codes (0x47/0x4F/0x49/0x51/0x52/0x53) and
// WHY those are indistinguishable from the letters G/O/I/Q/R/S. Widening the
// element type is what makes the 0x100+ range below possible; nothing else
// could have de-collided them.
static uint16_t keyboard_buffer[KEYBOARD_BUFFER_SIZE];
static volatile uint16_t kb_read_idx = 0;
static volatile uint16_t kb_write_idx = 0;

// #243: THE one cooked-key ring push. Every producer in this file goes through
// here. Before #243 there were twenty-three copies of these five lines, each
// with its own `char c` local; that is how a value like KEY_HOME (0x100) or a
// release code (0xC7, negative in a signed char) could have been silently
// truncated or sign-extended at one site and not another. Takes uint16_t so a
// caller cannot lose the high byte on the way in.
static void kb_push(uint16_t code) {
    uint16_t next_write = (kb_write_idx + 1) % KEYBOARD_BUFFER_SIZE;
    if (next_write != kb_read_idx) {   // full: drop, as before
        keyboard_buffer[kb_write_idx] = code;
        kb_write_idx = next_write;
    }
}

// #334: total cooked keys drained by keyboard_get_char(), so the serial test-
// input channel can confirm an injected key was actually CONSUMED, not just
// queued. Monotonic; harmless to wrap.
volatile uint64_t g_kbd_consumed = 0;

// #334: raw scancode BYTES seen by the IRQ1 handler at port 0x60. If this does
// not advance under qm sendkey, QEMU never delivered the scancode (the drop is
// QEMU-side, not in our ring).
volatile uint64_t g_kbd_irq_scancodes = 0;

// ---- Raw scancode tap (#202 DOS games) -----------------------------------
// When a DOS task (dos/dosexec.c) is running it sets g_dos_scancode_tap=1.
// The keyboard ISR then mirrors every RAW scancode byte (press, release, and
// 0xE0 extended prefix) into this ring so the DOS layer can replay it through
// the guest's INT 9 handler / port 0x60 exactly like real hardware. This is
// what id Software's Galaxy engine (Commander Keen 4-6) needs: it installs its
// own INT 9 ISR that reads port 0x60 and maintains a Keyboard[] scancode array.
#define DOS_SC_RING 256
volatile int      g_dos_scancode_tap = 0;
static volatile uint8_t  dos_sc_ring[DOS_SC_RING];
static volatile uint16_t dos_sc_rd = 0, dos_sc_wr = 0;
// #763: the ring used to have exactly ONE producer (the IRQ1 ISR), so an
// unlocked SPSC push was sound. It now has several (see dos_scancode_tap
// below), and a producer running in a thread can be preempted mid-push by the
// IRQ1 ISR producing on the same ring. Guard the WRITE side only: the reader
// (dos_scancode_get, called from the DOS interpreter thread) is still the sole
// consumer, so reader-vs-writer stays exactly the SPSC pairing it always was.
// The critical section is four instructions and never sleeps.
static spinlock_t dos_sc_lock = SPINLOCK_INIT_NAMED("dos_sc_ring");
static void dos_sc_push(uint8_t b) {
    uint64_t fl = spinlock_acquire_irqsave(&dos_sc_lock);
    uint16_t nx = (uint16_t)((dos_sc_wr + 1) % DOS_SC_RING);
    if (nx != dos_sc_rd) { dos_sc_ring[dos_sc_wr] = b; dos_sc_wr = nx; }
    spinlock_release_irqrestore(&dos_sc_lock, fl);
}

// #763: THE ONE PLACE A SET-1 SCANCODE BYTE ENTERS THE DOS TAP.
//
// It used to be a single call inside keyboard_handler(), the PS/2 IRQ1 ISR,
// and that was the whole bug: on any machine whose keyboard is USB (which is
// EVERY real iMac, and the #307 target) IRQ1 never fires, so nothing ever
// pushed and every DOS guest waited forever for a key. Not just guests with
// their own INT 9 handler either: dos_keyq_pump() fills the guest's BDA
// keyboard ring from this same tap, so INT 16h, the INT 21h console input
// group and a direct BDA read were ALL dead on a USB keyboard.
//
// The fix is not "call it from usb_hid.c too", which would have left the
// Bluetooth HID path (bt/hid.c) and the #334 test-input channel broken in
// exactly the same way and invited a fourth copy. It is called from
// keyboard_process_scancode(), which is ALREADY the single function every
// scancode source in the kernel funnels through:
//
//   cpu/isr.c        keyboard_handler()      real PS/2 IRQ1
//   cpu/isr.c        keyboard_poll_i8042()   polled i8042 (early boot, IF=0)
//   drivers/usb_hid.c emit_set1()            USB HID keyboards
//   bt/hid.c          emit_set1()            Bluetooth HID keyboards
//   drivers/testinput.c                      the #334 host->guest test channel
//
// so a DOS guest now sees keys from all five, and any SIXTH source added later
// is tapped by construction rather than by someone remembering.
//
// THE CONTROLLER STATUS-BYTE FILTER IS NOT DUPLICATED HERE ON PURPOSE. 0xFA
// (ACK), 0xFE (resend), 0xFC/0x00/0xFF (self-test / error / overrun) and 0xEE
// (echo) are i8042 CONTROLLER responses, not scancodes; they cannot occur on
// the USB or Bluetooth paths at all. keyboard_process_scancode() already
// drops them at its top, and this is called AFTER that early return, so the
// bytes reaching the tap are byte-identical to what the PS/2 path pushed
// before this change, with one filter in the kernel instead of two copies.
// ---- #162: the three media keys, taken out of the stream for everybody ----
//
// WHY THE INTERCEPT IS HERE AND NOT IN THE COMPOSITOR. #162 asks for volume
// up / down / mute to work FROM ANYWHERE, including under a fullscreen game,
// a DOS window and the lock screen. The compositor is not "anywhere": a
// running Win16 app is the SOLE keyboard consumer (SYS_GET_KEYBOARD returns
// -1 to the compositor while g_win16_owns_screen is set, see proc/syscall.c
// and the #200 note in the compositor's own input loop), so a hotkey checked
// in userland would be dead for every Win16 app on the machine. It would also
// be dead before the compositor starts.
//
// keyboard_process_scancode() is the one function EVERY scancode source in
// this kernel funnels through - the same property #763 relied on for the DOS
// tap and #156 relied on for the focus gate - so intercepting here covers
// PS/2 IRQ1, the polled i8042, USB HID, Bluetooth HID and the #334 test
// channel at once, and any sixth source added later by construction.
//
// The set-1 extended codes are the real ones a PS/2 keyboard sends and the
// ones QEMU's `sendkey volumeup/volumedown/audiomute` produces: E0 30, E0 2E,
// E0 20. drivers/usb_hid.c translates the Consumer-page HID usages to these
// same three codes (see hid_consumer_emit there), so there is ONE handler for
// every transport rather than one per driver.
static int media_action_for(uint8_t key) {
    switch (key) {
        case 0x30: return SYSVOL_ACT_UP;     // E0 30  Volume Up
        case 0x2E: return SYSVOL_ACT_DOWN;   // E0 2E  Volume Down
        case 0x20: return SYSVOL_ACT_MUTE;   // E0 20  Mute
        default:   return -1;
    }
}

// The DOS tap must not see the media keys either. A DOS guest that reads port
// 0x60 itself (Commander Keen's Galaxy engine does exactly this, which is why
// the tap exists at all) treats 0xE0 as a prefix it mostly ignores and then
// sees 0x30, which on the main block is the letter B. Pressing volume-up in a
// DOS game would type a phantom B. So the tap gets the byte pair only after
// it is known NOT to be a media key, which needs one byte of lookahead.
//
// This shares its one-keyboard-at-a-time assumption with the `extended_scancode`
// static that the cooked path a few lines below already keeps, and is no worse:
// two keyboards interleaving an E0 prefix would already have confused that one.
// It self-clears whenever the tap is disarmed, so a stale prefix cannot survive
// a #156 focus edge.
static volatile uint8_t media_tap_e0 = 0;
static void dos_scancode_tap(uint8_t b) {
    if (!g_dos_scancode_tap) { media_tap_e0 = 0; return; }
    if (media_tap_e0) {
        media_tap_e0 = 0;
        if (media_action_for((uint8_t)(b & 0x7F)) >= 0) return;   // swallow both
        dos_sc_push(0xE0);
        dos_sc_push(b);
        return;
    }
    if (b == 0xE0) { media_tap_e0 = 1; return; }
    dos_sc_push(b);
}

// Act on one media key. Called from the extended-make decode below, which may
// be running in HARD IRQ CONTEXT (PS/2 IRQ1). sysvol_key_rs() is atomics-only
// and sysvol_wake_apply() is a wake_up() on an irqsave lock; the codec write
// itself happens on the sysvol worker thread. Nothing here blocks.
static void media_key_press(int action) {
    if (action < 0) return;
    if (sysvol_key_rs(action)) sysvol_wake_apply();
}
// Drain one raw scancode byte. Returns -1 if empty.
int dos_scancode_get(void) {
    if (dos_sc_rd == dos_sc_wr) return -1;
    uint8_t b = dos_sc_ring[dos_sc_rd];
    dos_sc_rd = (uint16_t)((dos_sc_rd + 1) % DOS_SC_RING);
    return (int)b;
}
void dos_scancode_clear(void) { dos_sc_rd = dos_sc_wr = 0; }

// US keyboard scancode to ASCII mapping (set 1)
static const char scancode_to_ascii[128] = {
    0,    27,  '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b',
    '\t', 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n',
    0,    'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`',
    0,    '\\', 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/', 0,
    '*',  0,   ' ', 0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,    0,   0,   0,   0,   0,   '-', 0,   0,   0,   '+', 0,   0,
    0,    0,   0,   0,   0,   0,   0,   0,   0,
};

// Shifted characters
static const char scancode_to_ascii_shift[128] = {
    0,    27,  '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', '\b',
    '\t', 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n',
    0,    'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~',
    0,    '|', 'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?', 0,
    '*',  0,   ' ', 0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,    0,   0,   0,   0,   0,   '-', 0,   0,   0,   '+', 0,   0,
    0,    0,   0,   0,   0,   0,   0,   0,   0,
};

// ---------------------------------------------------------------------------
// The two shared keyboard primitives. drivers/keyboard.h has DECLARED both
// since the driver was written and NOTHING EVER DEFINED THEM, so the first
// caller to use one got a link error rather than a working function. That is
// why the tables above have no other consumer: everything that needed a
// scancode-to-character mapping had no shared one to call.
//
// They are defined HERE, in the same translation unit as the tables and the
// modifier state, so a caller can never see a keymap that disagrees with the
// one the ISR itself uses. Declared in drivers/keyboard.h; the forward
// declarations of the statics below keep them where they already are.
// ---------------------------------------------------------------------------
static volatile uint8_t shift_pressed;
static volatile uint8_t ctrl_pressed;
static volatile uint8_t alt_pressed;
static volatile uint8_t caps_lock;

char keyboard_scancode_to_char(uint8_t scancode, uint32_t modifiers) {
    if (scancode >= 128) return 0;
    char c = (modifiers & KEY_MOD_SHIFT) ? scancode_to_ascii_shift[scancode]
                                         : scancode_to_ascii[scancode];
    if ((modifiers & KEY_MOD_CAPS) && c >= 'a' && c <= 'z')
        c = (char)(c - 'a' + 'A');
    else if ((modifiers & KEY_MOD_CAPS) && (modifiers & KEY_MOD_SHIFT) &&
             c >= 'A' && c <= 'Z')
        c = (char)(c - 'A' + 'a');
    if (modifiers & KEY_MOD_CTRL) {
        if (c >= 'a' && c <= 'z')      c = (char)(c - 'a' + 1);
        else if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 1);
    }
    return c;
}

uint32_t keyboard_get_modifiers(void) {
    uint32_t m = 0;
    if (shift_pressed) m |= KEY_MOD_SHIFT;
    if (ctrl_pressed)  m |= KEY_MOD_CTRL;
    if (alt_pressed)   m |= KEY_MOD_ALT;
    if (caps_lock)     m |= KEY_MOD_CAPS;
    return m;
}

// Modifier key states
static volatile uint8_t shift_pressed = 0;
static volatile uint64_t shift_press_tick = 0;  // tick when shift was last pressed
static volatile uint8_t ctrl_pressed = 0;
static volatile uint8_t alt_pressed = 0;
static volatile uint8_t caps_lock = 0;
static volatile uint8_t extended_scancode = 0;  // For 0xE0 extended keys
// #148: PrintScreen MAKE is E0 2A E0 37 - two extended-prefixed bytes, and
// extended_scancode above is a one-byte lookahead that gets reset after
// EVERY extended byte it processes, so it cannot span the gap by itself.
// This flag is set when the first half (extended 0x2A) is seen and consumed
// when the second half (extended 0x37) completes the sequence; see the
// switch below. 0x2A is unambiguous here: real Left/Right Shift are NOT
// E0-prefixed, so an extended 0x2A can only be PrintScreen's first byte, and
// extended 0x37 has no other producer in the set-1 table at all.
static volatile uint8_t prtsc_pending = 0;

// Special key codes (above ASCII range)
#define KEY_UP      0x80
#define KEY_DOWN    0x81
#define KEY_LEFT    0x82
#define KEY_RIGHT   0x83
#define KEY_F5      0x84
#define KEY_F11     0x85
#define KEY_F12     0x86
#define KEY_F1      0x88
#define KEY_F2      0x89
#define KEY_F3      0x8B
#define KEY_F4      0x8C
#define KEY_F7      0x8D
#define KEY_F8      0x8E
#define KEY_F9      0x8F
#define KEY_F10     0x87
#ifndef KEY_F6
#define KEY_F6      0x8A
#endif
#ifndef KEY_SUPER
#define KEY_SUPER   0x9B
#endif

// #525 DIAGNOSTIC: measure how fast ticks are actually DELIVERED, in real time.
//
// This is the instrument that a heartbeat can never be. `uptime` is ticks/hz -
// circular, derived from the very counter under suspicion - and any sampling
// window longer than a burst averages the burst away, because tick REINJECTION
// CONSERVES the tick count: 1250 ticks crammed into 15ms still average an
// innocent 250Hz over 2 seconds. Only a per-tick, non-tick-clock measurement
// can see it. Cost: one rdtsc and a compare per tick, no polling loop.
//
// A tick arriving <1ms after the previous one (nominal spacing is 4ms at 250Hz)
// did not represent 4ms of elapsed time: it is a REINJECTED tick from a backlog
// KVM banked while the vCPU was starved. tickburst_max_run counts the longest
// consecutive run of such ticks, i.e. exactly how many ticks a `timer_ticks + N`
// deadline can be advanced by without N/hz seconds of wall clock passing.
uint64_t tickburst_max_run = 0;      // longest run of sub-1ms-spaced ticks
uint64_t tickburst_run_us  = 0;      // real microseconds that run occupied
uint64_t tickburst_min_gap = ~0ULL;  // smallest observed inter-tick gap (us)
static uint64_t tb_last_us = 0, tb_run = 0, tb_run_start_us = 0;

// Returns the monotonic microsecond reading it took, or 0 if the clock is not
// calibrated yet. #745 (#62): the caller needs that timestamp to record WHEN
// the native tick last fired, and taking a second rdtsc for it would double
// the per-tick cost of a function whose whole justification is that it costs
// one rdtsc.
static uint64_t tickburst_sample(void) {
    if (!mono_ready()) return 0;
    uint64_t now = mono_us();
    if (tb_last_us == 0) { tb_last_us = now; return now; }
    uint64_t gap = now - tb_last_us;
    tb_last_us = now;
    if (gap < tickburst_min_gap) tickburst_min_gap = gap;
    if (gap < 1000) {                 // reinjected: far tighter than 4ms nominal
        if (tb_run == 0) tb_run_start_us = now;
        tb_run++;
        if (tb_run > tickburst_max_run) {
            tickburst_max_run = tb_run;
            tickburst_run_us  = now - tb_run_start_us;
        }
    } else {
        tb_run = 0;
    }
    return now;
}

// ===========================================================================
// #745 (#62): TWO TICK SOURCES, SO THE CLOCK CANNOT HAVE A SINGLE POINT OF
// FAILURE.
//
// THE PROBLEM. Every timer in this OS - every sleep, every timeout, every
// scheduler preemption, and therefore every frame the compositor presents - is
// downstream of ONE interrupt: the 8254 PIT on IRQ0, vector 32, delivered
// through the 8259. On the owner's iMac14,4 the desktop only advances while
// something is running: stop moving the mouse and uptime stops counting; open
// an app and a real one-per-second tick appears. That is what a machine looks
// like when its periodic interrupt is not arriving and the scheduler is being
// dragged forward incidentally by other interrupts instead.
//
// THE SHAPE OF THE FIX. CLAUDE.md's preference order puts a REDUNDANT,
// ALWAYS-ARMED wake source first, "so no wake can ever be lost", with the PCM
// pump as the worked example: woken from BOTH the BCIS MSI ISR AND a 10 ms
// poll worker, so no wake is lost for more than 10 ms even where the MSI never
// arms. The same reasoning applies with more force here, because the thing at
// risk is time itself.
//
// WHY IT IS ARMED FROM BOOT AND NOT BY A WATCHDOG. The obvious design is to
// measure the tick and arm a backup when it looks dead. That design cannot
// work: any watchdog able to notice a dead tick would have to be scheduled,
// and scheduling is downstream of the tick. A watchdog for the clock cannot be
// clocked by the clock. So the Local APIC timer is armed unconditionally at
// boot and simply stays out of the way while the native source is healthy.
//
// COST ON A HEALTHY MACHINE. The redundant timer runs at TICK_REDUNDANT_HZ
// (well below the 250 Hz native rate, so it is a small fixed overhead), and on
// every one of those interrupts it does one comparison, finds the native
// source alive, and returns after an EOI. It never touches timer_ticks and
// never calls the scheduler. See tick_synth_decide_rs (rustkern/tickwatch.rs)
// for the decision and its self-tests.
// ===========================================================================

// Ticks contributed by the REDUNDANT source. cpu/tickwatch.c needs the NATIVE
// count, which is timer_ticks minus this: a health verdict computed from the
// total would be satisfied by the redundant source's own ticks and would
// report a healthy clock on a machine whose real timer is dead, hiding the
// very fault this instrument exists to find.
volatile uint64_t g_tick_src_lapic = 0;

// Monotonic microseconds at which the NATIVE source last fired. 0 = never (or
// the monotonic clock was not calibrated yet).
volatile uint64_t g_tick_last_native_us = 0;

// Rate for the redundant source. Deliberately far below the native 250 Hz:
// its job is to notice that the native source has stopped and then keep REAL
// time from the TSC, not to be a second full-rate clock. 50 Hz costs 50
// interrupts a second on a healthy machine and bounds the detection delay to
// ~20 ms.
#define TICK_REDUNDANT_HZ 50

// Hard cap on ticks synthesised in one interrupt. Without it, a long stall
// would be recovered as a single enormous jump in timer_ticks - which is
// precisely the KVM tick-reinjection burst that blame.md records as the reason
// nothing in this kernel may use ticks as a clock. We must not manufacture the
// same defect while fixing a different one.
#define TICK_SYNTH_MAX_CATCHUP 16

extern uint64_t tick_synth_decide_rs(uint64_t now_us, uint64_t last_native_us,
                                     uint64_t last_synth_us, uint64_t period_us,
                                     uint64_t max_catchup);

// Timer interrupt handler - the NATIVE source (8254 PIT on IRQ0, vector 32).
static void timer_handler(interrupt_frame_t *frame) {
    (void)frame;
    timer_ticks++;
    // Reuse the timestamp tickburst_sample() already took rather than paying a
    // second rdtsc per tick.
    uint64_t now = tickburst_sample();
    if (now) g_tick_last_native_us = now;

    // Send EOI first to allow nested interrupts
    pic_send_eoi(0);

    // Call scheduler tick (handles preemption)
    sched_tick();
}

// The REDUNDANT tick source (Local APIC timer, vector 0x41).
//
// Fires unconditionally at TICK_REDUNDANT_HZ from boot. While the native tick
// is alive this returns immediately after its EOI. When the native tick stops,
// it advances timer_ticks by however many ticks REAL TIME says have elapsed -
// not by one per interrupt - so tick-derived time stays approximately correct
// instead of running at 50/250 of true speed, and then runs the scheduler.
static void lapic_tick_handler(interrupt_frame_t *frame) {
    (void)frame;

    // A LAPIC-delivered interrupt MUST be acknowledged at the LAPIC, and first:
    // miss it and no further LAPIC interrupt is ever delivered, so the
    // redundant clock would fire exactly once and the machine would be no
    // better off. This is why the two sources have separate handlers rather
    // than a shared body with a shared EOI - a PIC-delivered interrupt needs
    // pic_send_eoi(0) and a LAPIC-delivered one needs lapic_eoi(), and a
    // handler that sent both would be lying about where its interrupt came
    // from.
    lapic_eoi();

    if (!mono_ready()) return;
    uint64_t now = mono_us();

    static uint64_t synth_last_us = 0;
    if (synth_last_us == 0) { synth_last_us = now; return; }

    uint32_t hz = g_timer_hz ? g_timer_hz : 250;
    uint64_t period_us = 1000000ull / hz;

    uint64_t n = tick_synth_decide_rs(now, g_tick_last_native_us, synth_last_us,
                                      period_us, TICK_SYNTH_MAX_CATCHUP);
    if (n == 0) {
        // Native source is alive and doing its job. Keep the synthesis
        // baseline current so that if it dies later we measure from now,
        // not from boot.
        synth_last_us = now;
        return;
    }

    synth_last_us += n * period_us;
    timer_ticks       += n;
    g_tick_src_lapic  += n;

    sched_tick();
}

// ===========================================================================
// #169: THE PER-CORE PREEMPTION TICK ON AN APPLICATION PROCESSOR.
//
// THE PROBLEM. This kernel has always run on the PIT, and IRQ0 is delivered to
// the BSP only. The only LAPIC-timer caller was the #62 redundant tick above,
// also BSP-only. So an AP took NO scheduler tick: preemption there was
// COOPERATIVE and a CPU-bound Ring-3 process held its core until it exited.
// MEASURED (#168 Job 1, 8 workers / 4 vCPU / gate ON, one boot): 48 work units
// on one worker against 40,192 on another. 837x between identical processes.
//
// THE DECISION THIS TICKET HAD TO MAKE, stated rather than drifted into: DOES
// THE PIT STAY, OR DOES EVERYTHING MOVE TO THE LAPIC? The PIT stays, and the AP
// gets a preemption-ONLY tick. Three reasons:
//
//  1. The BSP's LAPIC timer is ALREADY COMMITTED to a different job - #62's
//     redundant clock on vector 0x41. Moving the primary clock onto the same
//     hardware would collapse two independent tick sources into one and delete
//     the redundancy #62 exists to provide.
//  2. timer_ticks MUST KEEP EXACTLY ONE WRITER. Four cores each advancing it
//     makes the wall clock run at four times real speed and breaks every
//     `timer_ticks + N` deadline in the tree.
//  3. The regime that SHIPS is single-core, gate OFF. With the gate off no AP
//     is ever started, so nothing here is reached and the BSP path is
//     byte-identical to before this change.
//
// AND THIS IS NOT A THIRD ANSWER TO "WHAT DRIVES THE SCHEDULER". It writes down
// a split that was already implicit and unstated:
//
//     GLOBAL TIME and the once-per-tick global housekeeping (timer_ticks,
//     sched_ticks, cron/futex/child-exit hooks, the g_cpu_pct aggregate) have
//     EXACTLY ONE OWNER CORE, the BSP: PIT primary, LAPIC 0x41 failover.
//
//     PREEMPTION is PER-CORE, and always was. On the BSP it rides the global
//     tick; on an AP it needs its own source, because IRQ0 cannot be delivered
//     there. sched_tick() keeps the first job plus the BSP's share of the
//     second; sched_tick_ap() (proc/process.c) does the second job ONLY.
//
// Arming a LAPIC LVT is MMIO on per-core hardware reached from an interrupt
// stub, so this half stays in C and asm - the stated entanglement reason. The
// POLICY (what a tick should do to the process it interrupted) is Rust, in
// rustkern/aptick.rs.
// ===========================================================================
static void ap_preempt_tick_handler(interrupt_frame_t *frame) {
    (void)frame;
    // A LAPIC-delivered interrupt is acknowledged AT THE LAPIC, and FIRST. Miss
    // it and no further LAPIC interrupt is ever delivered to this core, so the
    // AP would take exactly one preemption tick and go cooperative again - a
    // failure that looks identical to not having made this change at all.
    // Sent BEFORE sched_tick_ap() because that call ends in sched_schedule(),
    // which does not return to this frame on the switching path.
    lapic_eoi();
    { extern void sched_tick_ap(void); sched_tick_ap(); }
}

// #169: arm THIS core's preemption tick. Called from ap_entry() (cpu/smp.c)
// once the AP's LAPIC, GDT/TSS and IDT are up, immediately before it enables
// interrupts.
//
// Returns 1 if the timer was actually armed, 0 if not. A caller must know: a
// core whose timer did not arm is a core that silently keeps the old
// cooperative behaviour, and "some cores preempt and some do not" is a harder
// bug than "no core preempts".
int tick_ap_arm(void) {
    extern uint32_t smp_this_cpu(void);
    uint32_t cpu = smp_this_cpu();

    // The BSP must never take this path: it already has the PIT for preemption
    // and its LAPIC timer is #62's redundant clock on vector 0x41. Arming 0x42
    // here would reprogram that one LVT and DESTROY the redundancy.
    if (cpu == 0) {
        kprintf("[APTICK] refused on cpu 0: the BSP preempts from the PIT and "
                "its LAPIC timer is the #62 redundant clock\n");
        return 0;
    }
    if (!lapic_is_enabled()) {
        kprintf("[APTICK] cpu %u: NOT armed, Local APIC not enabled. This core "
                "will not preempt; a CPU-bound process here runs to completion.\n",
                cpu);
        return 0;
    }
    // Refuse rather than let lapic_timer_init_vec() take its calibrate branch on
    // an AP - see lapic_timer_rate() in cpu/apic.c for why that is unsafe here.
    if (lapic_timer_rate() == 0) {
        kprintf("[APTICK] cpu %u: NOT armed, the LAPIC timer has no calibration "
                "(the BSP's calibrate must run first). This core will not "
                "preempt.\n", cpu);
        return 0;
    }

    // THE SAME RATE AS THE BSP TICK, deliberately. [SCHEDCORE]'s per-core busy
    // percentage divides a per-core busy-tick count by a window measured in BSP
    // ticks; a different AP rate would silently scale every AP's percentage.
    uint32_t hz = g_timer_hz ? g_timer_hz : 250;
    if (lapic_timer_init_vec(hz, 0x42)) {
        kprintf("[APTICK] cpu %u: preemption tick ARMED (LAPIC timer %u Hz, "
                "vector 0x42). This core preempts its own processes; it does "
                "NOT touch timer_ticks.\n", cpu, hz);
        bootlog_write("[APTICK] cpu %u ARMED (%uHz vec 0x42)", cpu, hz);
        return 1;
    }
    kprintf("[APTICK] *** cpu %u: preemption tick FAILED TO ARM. This core is "
            "COOPERATIVE ONLY: a CPU-bound Ring-3 process placed here will hold "
            "it until it exits (#169). ***\n", cpu);
    bootlog_write("[APTICK] cpu %u FAILED TO ARM", cpu);
    return 0;
}

// #745 (#62): arm the redundant tick source. Called once from kernel_main
// after smp_init() has brought the Local APIC up (reading or writing LAPIC
// registers before that would fault).
//
// A failure here is NOT fatal - it leaves the machine exactly as it was before
// this change, running on the PIT alone - but it must be LOUD, because a
// redundancy that silently did not arm is indistinguishable from one that is
// working, and this project has that exact failure on record more than once.
void tick_redundant_arm(void) {
#ifdef TICKWATCH_NO_REDUNDANT
    // #62 CONTROL ARM (`make TICKNOREDUNDANT=1`). Builds the kernel WITHOUT the
    // redundancy so that, paired with TICKFAULT=1, the machine can be watched
    // freezing. A test whose control arm is identical to the test arm produces
    // "no difference" for the one reason that is not a result, which is a
    // mistake this ticket's own previous pass made and recorded.
    kprintf("[TICKSRC] redundant tick DISABLED at compile time "
            "(TICKWATCH_NO_REDUNDANT). This is a TEST build.\n");
    bootlog_write("[TICKSRC] redundant tick DISABLED (test build)");
    return;
#else
    if (!lapic_is_enabled()) {
        kprintf("[TICKSRC] redundant tick NOT armed: Local APIC is not enabled. "
                "This machine has ONE tick source (the PIT); if IRQ0 is not "
                "delivered here, nothing will advance the clock.\n");
        bootlog_write("[TICKSRC] redundant tick NOT armed (no LAPIC)");
        return;
    }
    if (lapic_timer_init_vec(TICK_REDUNDANT_HZ, 0x41)) {
        kprintf("[TICKSRC] redundant tick source ARMED: Local APIC timer at %u Hz "
                "on vector 0x41, synthesising at most %u ticks per interrupt. "
                "The PIT remains the primary source; this only advances time "
                "when the PIT stops.\n",
                TICK_REDUNDANT_HZ, TICK_SYNTH_MAX_CATCHUP);
        bootlog_write("[TICKSRC] redundant tick ARMED (LAPIC %uHz vec 0x41)",
                      TICK_REDUNDANT_HZ);
    } else {
        kprintf("[TICKSRC] *** redundant tick FAILED TO ARM (LAPIC timer would "
                "not calibrate). The PIT is the only clock on this machine. ***\n");
        bootlog_write("[TICKSRC] redundant tick FAILED TO ARM");
    }
#endif
}

// Keyboard interrupt handler
// #307: scancode processing extracted from the IRQ1 handler so USB HID
// keyboards can push synthetic PS/2 set-1 scancodes through the EXACT same
// translation (shift/ctrl/caps state, key buffer encoding, arrow/F-key codes)
// as the real PS/2 keyboard. This runs in thread or IRQ context and must NOT
// touch the PIC/EOI (the caller owns that).
void keyboard_process_scancode(uint8_t scancode) {
    // Ignore keyboard controller response bytes (ACK, resend, self-test pass, etc.)
    // These arrive during keyboard initialization and are not real keypresses.
    if (scancode == 0xFA || scancode == 0xFE || scancode == 0xFC ||
        scancode == 0x00 || scancode == 0xFF ||
        scancode == 0xEE) {
        return;
    }

    // Raw scancode tap for DOS guests (#202, fixed for non-PS/2 keyboards by
    // #763). Mirror every byte VERBATIM, including 0xE0 extended prefixes and
    // release codes, before the cooked translation below: the DOS layer
    // replays these through the guest's INT 9 and fills its BDA key ring from
    // them. This is a MIRROR with no other consumer, so the guest does not
    // race the desktop for a keystroke.
    // #162: same ring, same bytes, minus the three media keys - see
    // dos_scancode_tap() above for why a DOS guest must not receive them.
    dos_scancode_tap(scancode);

    // Handle extended scancode prefix (0xE0)
    if (scancode == 0xE0) {
        extended_scancode = 1;
        return;
    }

    // Check for key release (bit 7 set)
    if (scancode & 0x80) {
        // Key release
        uint8_t key = scancode & 0x7F;
        if (extended_scancode) {
            // Extended key release - send KEY_UP for arrow keys
            extended_scancode = 0;
            uint16_t c = 0;
            switch (key) {
                case 0x48: c = KEY_UP + 0x10; break;    // Up release
                case 0x50: c = KEY_DOWN + 0x10; break;  // Down release
                case 0x4B: c = KEY_LEFT + 0x10; break;  // Left release
                case 0x4D: c = KEY_RIGHT + 0x10; break; // Right release
                // (Word6 divergence catalog #2, Alt-menu) Right Alt/AltGr
                // (HID 0xE6) and real PS/2 Right Alt both arrive as extended
                // (0xE0-prefixed) scancode 0x38, same byte as Left Alt's
                // non-extended release below. Route both to the same
                // KEY_ALT_UP so either Alt key drives WM_SYSKEYUP/menu-mnemonic
                // gating in exec/win16api.c.
                case 0x38: c = KEY_ALT_UP; break;       // Right Alt release
                // #221 phase 0, RIGHT CTRL. MEASURED on VM <vmid> (golden
                // 2040) with the #334 serial injector: E0 1D / E0 9D
                // produced NOTHING AT ALL. No cooked key reached any app,
                // AND ctrl_pressed was never set, so `Right Ctrl + c`
                // delivered a plain 'c' (0x63) instead of the control
                // character 0x03 that Left Ctrl produces. Right Ctrl was not
                // "partly supported"; it did not exist. The extended switch
                // simply had no case for it, and the non-extended 0x1D case
                // below can never see an E0-prefixed byte.
                //
                // Both Ctrl keys route to the SAME KEY_LCTRL/KEY_LCTRL_UP
                // codes, exactly as both Alt keys route to KEY_ALT/
                // KEY_ALT_UP two lines up. There is no consumer in the tree
                // that distinguishes left from right Ctrl, and inventing a
                // separate code would need a free byte in a range that has
                // none (see the KEY_SUPER note in cpu/isr.h).
                case 0x1D: ctrl_pressed = 0; c = KEY_LCTRL_UP; break;  // Right Ctrl release
                // #162: media-key RELEASE. These already produced nothing
                // (no case matched, so c stayed 0); the explicit cases exist
                // so the omission reads as a decision rather than an
                // oversight the next person "fixes". Volume is a press-only
                // action; there is no held state to track.
                case 0x30:   // Volume Up release
                case 0x2E:   // Volume Down release
                case 0x20:   // Mute release
                    break;
            }
            if (c != 0) {
                kb_push(c);
            }
        } else if (key == 0x2A || key == 0x36) {
            shift_pressed = 0;  // Left or right shift released
            // Send key release event
            uint16_t c = (key == 0x2A) ? KEY_LSHIFT_UP : KEY_RSHIFT_UP;
            kb_push(c);
        } else if (key == 0x1D) {
            ctrl_pressed = 0;   // Ctrl released
            // Send key release event
            uint16_t c = KEY_LCTRL_UP;
            kb_push(c);
        } else if (key == 0x38) {
            alt_pressed = 0;    // Alt released
            // (Word6 divergence catalog #2, Alt-menu, ROOT CAUSE part 1) This
            // used to ONLY clear the local `alt_pressed` flag, which is a
            // file-static variable with ZERO external readers (grepped this
            // file: nothing outside keyboard_process_scancode ever reads it).
            // So Alt release was completely invisible to every consumer,
            // including exec/win16api.c: Alt was never tracked as a modifier
            // at all, and every Alt-chord degraded to a plain character
            // keystroke (Alt+F typed "f" into the document instead of opening
            // the File menu). Push KEY_ALT_UP exactly like Ctrl does above so
            // win16api.c can see real Alt up/down transitions.
            uint16_t c = KEY_ALT_UP;
            kb_push(c);
        } else {
            // Regular key release - convert scancode to ASCII, then add release bit
            if (key < 128) {
                char base_char;
                if (shift_pressed) {
                    base_char = scancode_to_ascii_shift[key];
                } else {
                    base_char = scancode_to_ascii[key];
                }
                // Only emit release events for printable ASCII (0x20-0x7E).
                // Control chars (Backspace 0x08, Tab 0x09, Enter 0x0A, ESC 0x1B)
                // would produce release codes (0x88, 0x89, 0x8A, 0x9B) that
                // collide with the KEY_F* special-key range (0x80-0x8F).
                // In particular, Enter release 0x8A == KEY_F6 was triggering
                // a phantom terminal launch every time the user pressed Enter
                // in the shell.
                if (base_char >= 0x20 && base_char <= 0x7E) {
                    uint16_t c = (uint16_t)((uint8_t)base_char | 0x80);  // ASCII char with release bit
                    kb_push(c);
                }
            }
        }
    } else {
        // Key press
        if (extended_scancode) {
            // Extended key - arrow keys plus the cursor-editing extended
            // keys (#299), plus the Super/GUI/Windows key (#552). Arrows are
            // remapped to KEY_UP/DOWN/LEFT/RIGHT (0x80-0x83); Home/End/Delete/
            // PgUp/PgDn/Insert get their OWN cooked codes in the 0x100 block
            // (KEY_HOME..KEY_DEL, cpu/isr.h).
            //
            // #243 CORRECTION. This comment used to say they were forwarded as
            // their raw PS/2 make codes and that "these values are all below
            // 0x80 so they do not collide with the KEY_* range". Both halves
            // were true and the conclusion was still wrong: not colliding with
            // the 0x80 block is not the same as not colliding, and 0x47 is the
            // letter G. Six keys typed G O I Q R S into every app for as long
            // as that reasoning stood. Do not reintroduce a pass-through. Left GUI (0x5B) and Right GUI
            // (0x5C) both map to KEY_SUPER: real PS/2 hardware sends these
            // set-1 extended make codes directly, and usb_hid.c's HID usage
            // 0xE3/0xE7 -> set-1 0x5B/0x5C translation (emit_set1) feeds the
            // exact same path, so one switch case covers both keyboard types.
            uint16_t c = 0;
            switch (scancode) {
                case 0x48: c = KEY_UP; break;
                case 0x50: c = KEY_DOWN; break;
                case 0x4B: c = KEY_LEFT; break;
                case 0x4D: c = KEY_RIGHT; break;
                // #243: these six USED to be forwarded as their raw set-1 make
                // code, which is the whole defect. 0x47/0x4F/0x49/0x51/0x52/
                // 0x53 are the ASCII codes for G/O/I/Q/R/S, so an app could
                // not tell Home from a capital G. `echo GIOQ hello` displayed
                // `echo  hello` and PgUp inside vi entered INSERT mode. They
                // now get codes in the 0x100 block, which no ASCII character
                // and no release code can ever reach.
                case 0x47: c = KEY_HOME; break;  // Home   (was 0x47 == 'G')
                case 0x4F: c = KEY_END;  break;  // End    (was 0x4F == 'O')
                case 0x53: c = KEY_DEL;  break;  // Delete (was 0x53 == 'S')
                case 0x49: c = KEY_PGUP; break;  // Page Up   (was 0x49 == 'I')
                case 0x51: c = KEY_PGDN; break;  // Page Down (was 0x51 == 'Q')
                case 0x52: c = KEY_INS;  break;  // Insert (was 0x52 == 'R')
                case 0x5B: c = KEY_SUPER; break;  // Left GUI/Windows/Command
                case 0x5C: c = KEY_SUPER; break;  // Right GUI/Windows/Command
                // #162: SYSTEM-GLOBAL media keys. Handled RIGHT HERE and
                // deliberately left with c == 0, so they never enter the
                // cooked ring: no app, no game, no DOS guest and no lock
                // screen can swallow them, and equally none of them ever
                // receives a stray keystroke because of them. This is the
                // "before per-window routing, not after" placement #156
                // identified.
                case 0x30:   // Volume Up
                case 0x2E:   // Volume Down
                case 0x20:   // Mute
                    media_key_press(media_action_for(scancode));
                    break;
                // #763: the two keypad keys that are E0-prefixed on real set-1
                // hardware. Neither had a case, so keypad Enter and keypad /
                // produced NOTHING on this desktop, for PS/2 keyboards as much
                // as USB ones. Map them to the same characters as their main
                // block twins, which is what every other OS does.
                case 0x1C: c = '\n'; break;      // Keypad Enter
                case 0x35: c = '/';  break;      // Keypad /
                // (Word6 divergence catalog #2, Alt-menu) Right Alt/AltGr: see
                // the matching comment on the release switch above.
                case 0x38: c = KEY_ALT; break;    // Right Alt press
                // #221 phase 0: Right Ctrl press. See the matching comment on
                // the release switch above for the measurement. Setting
                // ctrl_pressed here is the half that matters most: it is what
                // makes Right Ctrl + letter fold to a control character, and
                // what makes keyboard_get_modifiers() (and therefore the new
                // SYS_KEY_MODS) tell the truth about a held Right Ctrl.
                case 0x1D: ctrl_pressed = 1; c = KEY_LCTRL; break;  // Right Ctrl press
                // #148: PrintScreen MAKE, first half (see prtsc_pending above).
                // Consumes the byte without producing a cooked key yet.
                case 0x2A: prtsc_pending = 1; break;
                // #148: PrintScreen MAKE, second half. Only fires
                // KEY_PRINTSCREEN if the first half was actually seen, so an
                // unrelated stray extended 0x37 (none is known to exist) could
                // not forge one.
                case 0x37:
                    if (prtsc_pending) { prtsc_pending = 0; c = KEY_PRINTSCREEN; }
                    break;
            }
            extended_scancode = 0;

            if (c != 0) {
                kb_push(c);
            }
        } else if (scancode == 0x2A || scancode == 0x36) {
            shift_pressed = 1;  // Shift pressed
            shift_press_tick = timer_ticks;
            // Also send as key event for games
            uint16_t c = (scancode == 0x2A) ? KEY_LSHIFT : KEY_RSHIFT;
            kb_push(c);
        } else if (scancode == 0x1D) {
            ctrl_pressed = 1;   // Ctrl pressed
            // Also send as key event for games
            uint16_t c = KEY_LCTRL;
            kb_push(c);
        } else if (scancode == 0x38) {
            alt_pressed = 1;    // Alt pressed
            // (Word6 divergence catalog #2, Alt-menu, ROOT CAUSE part 2) Push
            // KEY_ALT so exec/win16api.c can translate it to real VK_MENU and
            // generate WM_SYSKEYDOWN/WM_SYSCHAR (not WM_KEYDOWN/WM_CHAR) while
            // Alt is held, and drive Alt+mnemonic menu access. See the release
            // branch above for why this was previously a silent no-op.
            uint16_t c = KEY_ALT;
            kb_push(c);
        } else if (scancode == 0x3F) {
            // F5 key pressed
            uint16_t c = KEY_F5;
            kb_push(c);
        } else if (scancode == 0x40) {
            // F6 key pressed - reserved for GUI terminal launch (Phase J2)
            uint16_t c = KEY_F6;
            kb_push(c);
        } else if (scancode == 0x3B) {
            // F1 key pressed
            uint16_t c = KEY_F1;
            kb_push(c);
        } else if (scancode == 0x3C) {
            // F2 key pressed
            uint16_t c = KEY_F2;
            kb_push(c);
        } else if (scancode == 0x3D) {
            // F3 key pressed
            uint16_t c = KEY_F3;
            kb_push(c);
        } else if (scancode == 0x3E) {
            // F4 key pressed
            uint16_t c = KEY_F4;
            kb_push(c);
        } else if (scancode == 0x41) {
            // F7 key pressed
            uint16_t c = KEY_F7;
            kb_push(c);
        } else if (scancode == 0x42) {
            // F8 key pressed
            uint16_t c = KEY_F8;
            kb_push(c);
        } else if (scancode == 0x43) {
            // F9 key pressed
            uint16_t c = KEY_F9;
            kb_push(c);
        } else if (scancode == 0x44) {
            // F10 key pressed
            uint16_t c = KEY_F10;
            kb_push(c);
        } else if (scancode == 0x57) {
            // F11 key pressed - return special key code
            uint16_t c = KEY_F11;
            kb_push(c);
        } else if (scancode == 0x58) {
            // F12 key pressed - return special key code
            uint16_t c = KEY_F12;
            kb_push(c);
        } else if (scancode == 0x3A) {
            // Caps Lock toggle
            caps_lock ^= 1;
        } else if (scancode < 128) {
            // Auto-release shift if stuck (e.g. VNC console drops release scancode)
            if (shift_pressed && (timer_ticks - shift_press_tick) > 500) {
                shift_pressed = 0;
            }
            // Convert scancode to ASCII
            char c;
            if (shift_pressed) {
                c = scancode_to_ascii_shift[scancode];
            } else {
                c = scancode_to_ascii[scancode];
            }
            // Apply Caps Lock: inverts case for alphabetic characters only
            if (caps_lock) {
                if (c >= 'a' && c <= 'z') c = c - 'a' + 'A';
                else if (c >= 'A' && c <= 'Z') c = c - 'A' + 'a';
            }

            // Handle Ctrl combinations
            if (ctrl_pressed && c >= 'a' && c <= 'z') {
                c = c - 'a' + 1;  // Convert to control character
                // Ctrl+C (character 3) sets interrupt flag
                if (c == 3) {
                    interrupt_requested = 1;
                }
            }

            // Add to buffer if valid character. Cast through uint8_t: `c` is a
            // SIGNED char here (it is case-folded and Ctrl-arithmetic'd above),
            // so a value with bit 7 set would sign-extend to 0xFFxx in the now
            // 16-bit ring (#243).
            if (c != 0) {
                kb_push((uint8_t)c);
            }
        }
    }
}

// Real PS/2 keyboard IRQ1 handler: read the byte, run the shared scancode
// processor, then acknowledge the PIC.
//
// #763: this used to mirror the byte into the DOS tap itself, behind its own
// copy of the controller-status-byte filter. Both moved into
// keyboard_process_scancode(), which every keyboard source already calls, so
// a USB or Bluetooth keyboard feeds a DOS guest too. The bytes this path puts
// in the tap, and their order, are unchanged.
static void keyboard_handler(interrupt_frame_t *frame) {
    (void)frame;

    // Read scancode from keyboard controller
    uint8_t scancode = inb(0x60);
    g_kbd_irq_scancodes++;   // #334 delivery instrument

    keyboard_process_scancode(scancode);

    // Send EOI
    pic_send_eoi(1);
}

// Check if keyboard has a character
// #616: POLLED i8042 drain, for contexts that run with interrupts DISABLED.
//
// keyboard_has_char() below reads a ring buffer that ONLY keyboard_handler()
// (the IRQ1 ISR) fills, so it is permanently false anywhere RFLAGS.IF == 0.
// The whole early-boot window is such a context: main() does not sti() until
// long after the filesystem check, so the boot fsck's "press ESC to skip"
// prompt polled a buffer nothing could ever write to.
//
// The failure was doubly silent, and worth understanding before adding any
// other early-boot prompt. The i8042 output buffer holds exactly ONE byte:
// with nothing reading port 0x60, the first keypress latches in OBF and the
// controller then DROPS every subsequent scancode. So the prompt produced no
// effect AND no side effect no matter how many times the key was pressed
// (measured: 40 ESC presses over the check window, zero response), which reads
// exactly like a key-delivery problem and is not one.
//
// This drains pending bytes by hand and feeds them through the SAME scancode
// state machine and the SAME cooked ring buffer the ISR uses, so
// keyboard_has_char()/keyboard_get_char() keep working unchanged and no second,
// divergent keyboard path exists. keyboard_process_scancode() calls no other
// function and touches only this file's statics, so it is safe with interrupts
// in any state; a byte consumed here is simply one the ISR never sees.
//
// BOUNDED (#426): at most KBD_POLL_DRAIN_MAX bytes per call, so a wedged
// controller that permanently asserts OBF degrades to "we read 32 bytes and
// move on", never to a hang.
#define KBD_POLL_DRAIN_MAX 32
void keyboard_poll_i8042(void) {
    for (int i = 0; i < KBD_POLL_DRAIN_MAX; i++) {
        uint8_t st = inb(0x64);
        if (!(st & 0x01)) return;      // OBF clear: nothing pending
        uint8_t data = inb(0x60);
        if (st & 0x20) continue;       // bit 5 = AUX: a mouse byte, not ours
        keyboard_process_scancode(data);
    }
}

// #148: push a cooked key code directly into the same ring buffer
// keyboard_process_scancode() feeds, bypassing scancode translation. See
// cpu/isr.h for why (USB/Bluetooth HID PrintScreen: a clean single usage with
// no honest single-(code,extended) PS/2 set-1 encoding). Same ring, same
// overflow policy (drop the newest byte rather than clobber the reader), so
// keyboard_has_char()/keyboard_get_char() see it exactly like any other key.
void keyboard_push_cooked_key(uint16_t code) {
    kb_push(code);   // #243: one ring push in this file, not two
}

int keyboard_has_char(void) {
    return kb_read_idx != kb_write_idx;
}

// Get a character from keyboard buffer
// Returns int to handle special keys like KEY_UP (0x80+)
int keyboard_get_char(void) {
    if (kb_read_idx == kb_write_idx) {
        return 0;  // No character available
    }

    // #243: the ring element is uint16_t, so this widens rather than truncates;
    // KEY_HOME (0x100) and friends survive the trip. The old `(unsigned char)`
    // cast would have silently masked them to 0x00.
    int c = (int)keyboard_buffer[kb_read_idx];
    kb_read_idx = (kb_read_idx + 1) % KEYBOARD_BUFFER_SIZE;
    g_kbd_consumed++;   // #334: an injected or real key was actually CONSUMED
    return c;
}

// #334: current occupancy of the cooked-key ring (queued but not yet drained).
int keyboard_buffer_depth(void) {
    return (int)((kb_write_idx - kb_read_idx + KEYBOARD_BUFFER_SIZE) % KEYBOARD_BUFFER_SIZE);
}

// #334: reverse of the scancode->ASCII tables so the serial test-input channel
// can TYPE a string by synthesizing set-1 scancodes through the identical
// keyboard_process_scancode() path the PS/2 IRQ uses. Reuses the SAME tables as
// the forward path so the two can never drift. Returns the set-1 scancode and
// sets *need_shift, or -1 if the char is not typeable from these tables.
int keyboard_ascii_to_scancode(char c, int *need_shift) {
    for (int sc = 0; sc < 128; sc++) {
        if (scancode_to_ascii[sc] == c) { if (need_shift) *need_shift = 0; return sc; }
    }
    for (int sc = 0; sc < 128; sc++) {
        if (scancode_to_ascii_shift[sc] == c) { if (need_shift) *need_shift = 1; return sc; }
    }
    if (need_shift) *need_shift = 0;
    return -1;
}

// Clear interrupt flag
void clear_interrupt(void) {
    interrupt_requested = 0;
}

// Check if interrupt was requested (and clear it)
int check_interrupt(void) {
    if (interrupt_requested) {
        interrupt_requested = 0;
        return 1;
    }
    return 0;
}

// Initialize ISR handlers
void isr_init(void) {
    kprintf("[ISR] Registering interrupt handlers...\n");

    // Register timer handler (IRQ 0 = INT 32)
    idt_register_handler(32, timer_handler);

    // Register keyboard handler (IRQ 1 = INT 33)
    idt_register_handler(33, keyboard_handler);

    // #745 (#62): the redundant tick source's own vector. Registering the
    // handler does NOT arm the timer - cpu/tickwatch.c does that, and only
    // after measuring the native tick dead. Registered here unconditionally so
    // that when the failover does arm, the IDT entry is already in place and
    // arming is a single register write with no window in which the timer
    // fires into an unregistered vector.
    // 0x41 is free: 32 timer, 33 keyboard, 44 mouse, 0x50 HDA MSI, 240 SMP wake.
    //
    // THIS IS ONLY HALF OF WIRING A VECTOR. idt_register_handler() populates a
    // C dispatch table; the IDT GATE for 0x41 is installed separately in
    // cpu/idt.c from the irq_tick_redundant stub in cpu/idt.asm, exactly as
    // 0x50 (HDA MSI) and 240 (SMP wake) are. Doing only this half builds
    // clean, links clean and passes every gate, and then #GPs on the first
    // interrupt with error code 0x20b - measured, on the first boot of this
    // change, at the instant sti() ran.
    idt_register_handler(0x41, lapic_tick_handler);

    // #169: the AP preemption tick's own vector. Registered here, on the BSP,
    // because there is ONE C dispatch table and ONE IDT for the whole machine
    // (idt_load_ap loads this same table); an AP arming its timer later finds
    // both halves already in place. Registering the handler does NOT arm any
    // timer - tick_ap_arm() does that, per core, and only on an AP.
    // 0x42 is free: 32 timer, 33 keyboard, 44 mouse, 0x41 redundant tick,
    // 0x50 HDA MSI, 0x51 xHCI MSI, 240 SMP wake.
    idt_register_handler(0x42, ap_preempt_tick_handler);

    // #429: register the real page-fault (#PF, vector 14) handler. Before this
    // the demand-paging subsystem was dead and every fault was instantly fatal.
    // The handler resolves demand-zero / lazy mmap / COW faults and turns an
    // invalid user access into a per-process SIGSEGV.
    { extern void page_fault_handler(interrupt_frame_t *frame);
      idt_register_handler(14, page_fault_handler); }

    // #429: enable EFER.NXE on the BSP so the demand paths may mark writable
    // data pages no-execute (W^X). APs enable it in smp.c ap_entry().
    { extern void cpu_enable_nx(void); cpu_enable_nx(); }

    // Enable timer and keyboard IRQs
    pic_enable_irq(0);  // Timer
    pic_enable_irq(1);  // Keyboard

    kprintf("[ISR] Timer and keyboard handlers registered\n");
}
