// logic.rs - taskmgr's pure geometry and ordering decisions (#188).
//
// WHY THIS FILE EXISTS, and why it is `include!`d rather than being a module:
//
// The defect this file was created to kill is "a control that RENDERS and DOES
// NOTHING". Both instances found in this app on 2026-08-20 had the same shape:
// the DRAW code and the HIT-TEST code each carried their OWN copy of a
// coordinate, and the two copies disagreed.
//
//   - The "Prio +/-" button was drawn spanning dw-90 .. dw-16 while the footer
//     hit-test only ever tested dw-250..dw-176 and dw-170..dw-96. Clicking the
//     button did nothing at all; priority could only be changed with the +/-
//     KEYS, which nothing on screen mentioned.
//   - The Processes column headers were drawn from six separate `a.dw - N`
//     literals and were never hit-tested at all.
//
// A second literal is exactly how those coordinates drifted apart, so there is
// now ONE definition of each: `proc_cols()`, `proc_btns()`, `sched_btns()`.
// Draw code and hit-test code both call them. It is no longer POSSIBLE for the
// button you can see and the rectangle that responds to a click to disagree,
// because there is only one rectangle.
//
// Everything here is pure: integers in, decisions out, NO syscalls, no globals,
// no `unsafe`. That is what lets `make logic-test` build this same file for the
// HOST with std and assert on it directly (see logic_test.rs). Those assertions
// are the reason we can say the click lands rather than hoping it does, and the
// red arm of that test is a verbatim transcription of the OLD hit-test, which
// still returns "nothing" for the pixels the Prio button occupies.
//
// Keep it std-agnostic: no `use` statements, no allocation, core-only
// constructs. main.rs is `#![no_std]`; logic_test.rs is an ordinary host
// binary; both `include!` this text unchanged.

// ---------------------------------------------------------------------------
// Shared layout metrics. These live here, not in main.rs, because hit-testing
// needs every one of them and a copy is the bug.
// ---------------------------------------------------------------------------
pub const TAB_H: i32 = 26;
pub const ROW_H: i32 = 20;
pub const PAD: i32 = 10;

/// Y of the column-header text row on any list tab.
pub const LIST_HDR_Y: i32 = PAD + TAB_H + 6;
/// Y of the first data row: the header band is LIST_HDR_Y .. LIST_TOP_Y.
pub const LIST_TOP_Y: i32 = LIST_HDR_Y + 18;
/// Footer button height, and the footer's Y for a window of height `dh`.
pub const FOOT_H: i32 = 26;
pub fn foot_y(dh: i32) -> i32 { dh - 36 }

/// A button/column rectangle in X only (Y comes from the band it lives in).
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub struct Btn { pub x: i32, pub w: i32 }
impl Btn {
    pub fn hit(&self, mx: i32) -> bool { mx >= self.x && mx < self.x + self.w }
}

// ---------------------------------------------------------------------------
// Processes tab: columns and click-to-sort
// ---------------------------------------------------------------------------
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum SortCol { Name, Pid, State, Core, Cpu, Mem }

/// X of each column's text. THE one definition; draw_processes() and
/// header_col_at() both call it.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub struct ProcCols {
    pub name: i32,
    pub pid: i32,
    pub state: i32,
    pub core: i32,
    pub cpu: i32,
    pub mem: i32,
}
pub fn proc_cols(dw: i32) -> ProcCols {
    ProcCols {
        name: PAD + 4,
        pid: dw - 330,
        state: dw - 268,
        core: dw - 196,
        cpu: dw - 140,
        mem: dw - 90,
    }
}

/// Which column header was clicked, for a click already known to be in the
/// header band (LIST_HDR_Y .. LIST_TOP_Y). Each column owns the strip from its
/// own text X up to the next column's text X, so the bands are contiguous and
/// there is no dead gap between them: a click anywhere on the header row sorts
/// by something, which is what the owner will expect after clicking once.
pub fn header_col_at(dw: i32, mx: i32) -> Option<SortCol> {
    let c = proc_cols(dw);
    if mx < PAD || mx >= dw - PAD { return None; }
    if mx < c.pid { return Some(SortCol::Name); }
    if mx < c.state { return Some(SortCol::Pid); }
    if mx < c.core { return Some(SortCol::State); }
    if mx < c.cpu { return Some(SortCol::Core); }
    if mx < c.mem { return Some(SortCol::Cpu); }
    Some(SortCol::Mem)
}

/// The direction a column starts in when you first sort by it. "Biggest first"
/// is the useful answer for a magnitude (who is eating the CPU / the RAM);
/// "A first" is the useful answer for a name or an id you are hunting for.
pub fn sort_default_desc(c: SortCol) -> bool {
    match c {
        SortCol::Cpu | SortCol::Mem | SortCol::Core => true,
        SortCol::Name | SortCol::Pid | SortCol::State => false,
    }
}

/// Click-to-sort state machine: a new column adopts its natural direction, the
/// same column again reverses. Returns the (column, descending) to adopt.
pub fn sort_next(cur: SortCol, cur_desc: bool, clicked: SortCol) -> (SortCol, bool) {
    if cur == clicked { (clicked, !cur_desc) } else { (clicked, sort_default_desc(clicked)) }
}

/// The 'S' key cycles the sort column so the feature is reachable with no
/// pointer at all (the iMac target has spent whole builds with no working
/// mouse, and a mouse-only control is untestable over a serial line).
pub fn sort_cycle(c: SortCol) -> SortCol {
    match c {
        SortCol::Name => SortCol::Pid,
        SortCol::Pid => SortCol::State,
        SortCol::State => SortCol::Core,
        SortCol::Core => SortCol::Cpu,
        SortCol::Cpu => SortCol::Mem,
        SortCol::Mem => SortCol::Name,
    }
}

/// The header glyph showing the current direction. ASCII on purpose: the TTF
/// path takes bytes, and a multi-byte arrow would render as mojibake.
pub fn sort_glyph(desc: bool) -> u8 { if desc { b'v' } else { b'^' } }

// ---------------------------------------------------------------------------
// Row ordering. Keys are extracted from ProcInfo by the caller so that nothing
// in this file has to know the kernel ABI, and so the host test can build rows
// by hand.
//
// NOTE ON THE CPU COLUMN: sorting by CPU descending is NOT done here. It is
// done by the SHARED `proccpu_sort()` in libc/proccpu.c, the same object file
// /APPS/top and /APPS/SYSMON call, and CPU-ascending is that same shared sort
// followed by a reverse. This file's comparator is never used for CPU-desc.
// Writing a second CPU ranking here is exactly the five-copies mistake #178
// spent a ticket undoing.
// ---------------------------------------------------------------------------
#[derive(Clone, Copy)]
pub struct RowKey {
    pub name: [u8; 32],
    pub pid: u32,
    pub state: u32,
    pub core: i32,
    pub cpu: u32,
    pub mem: u32,
}
pub const ROWKEY_ZERO: RowKey = RowKey { name: [0; 32], pid: 0, state: 0, core: 0, cpu: 0, mem: 0 };

fn lower(c: u8) -> u8 { if c >= b'A' && c <= b'Z' { c + 32 } else { c } }

/// Case-insensitive compare of two NUL-terminated fixed arrays. -1 / 0 / 1.
fn name_cmp(a: &[u8; 32], b: &[u8; 32]) -> i32 {
    let mut i = 0;
    while i < 32 {
        let x = lower(a[i]);
        let y = lower(b[i]);
        if x != y { return if x < y { -1 } else { 1 }; }
        if x == 0 { return 0; }
        i += 1;
    }
    0
}

fn ucmp(a: u64, b: u64) -> i32 { if a < b { -1 } else if a > b { 1 } else { 0 } }

/// Compare on the CHOSEN column only. -1 / 0 / 1.
pub fn row_cmp(col: SortCol, a: &RowKey, b: &RowKey) -> i32 {
    match col {
        SortCol::Name => name_cmp(&a.name, &b.name),
        SortCol::Pid => ucmp(a.pid as u64, b.pid as u64),
        SortCol::State => ucmp(a.state as u64, b.state as u64),
        // running_cpu is -1 for "not on a core right now", which must sort as
        // the lowest value, not wrap to a huge unsigned.
        SortCol::Core => {
            if a.core < b.core { -1 } else if a.core > b.core { 1 } else { 0 }
        }
        SortCol::Cpu => ucmp(a.cpu as u64, b.cpu as u64),
        SortCol::Mem => ucmp(a.mem as u64, b.mem as u64),
    }
}

/// Strict "a comes before b" under (column, direction).
///
/// The TIEBREAK IS ALWAYS PID ASCENDING, in both directions, and that is
/// deliberate on two counts. It makes the order TOTAL, so two rows with the
/// same CPU% cannot swap places on every 1 Hz refresh and look like a flicker
/// bug. And it does not itself reverse, because "sort descending" is a
/// statement about the column the user clicked, not about the hidden key used
/// to settle ties; reversing the tiebreak too makes equal rows shuffle for no
/// reason the user can see.
pub fn row_before(col: SortCol, desc: bool, a: &RowKey, b: &RowKey) -> bool {
    let c = row_cmp(col, a, b);
    if c != 0 { return if desc { c > 0 } else { c < 0 }; }
    a.pid < b.pid
}

/// Stable insertion sort producing a PERMUTATION, so the caller can apply the
/// same permutation to procs[] and to the parallel cpu_pct[] and they cannot
/// come apart. n is at most MAXP (64), so an insertion sort is the right tool.
pub fn sort_perm(col: SortCol, desc: bool, keys: &[RowKey], n: usize, out: &mut [usize]) {
    let n = if n > keys.len() { keys.len() } else { n };
    let n = if n > out.len() { out.len() } else { n };
    for i in 0..n { out[i] = i; }
    let mut i = 1;
    while i < n {
        let v = out[i];
        let mut j = i;
        while j > 0 && row_before(col, desc, &keys[v], &keys[out[j - 1]]) {
            out[j] = out[j - 1];
            j -= 1;
        }
        out[j] = v;
        i += 1;
    }
}

// ---------------------------------------------------------------------------
// Processes tab footer
// ---------------------------------------------------------------------------
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum FootAct { None, EndTask, Kill, PrioDown, PrioUp }

/// End Task | Kill | Prio - | Prio +, right-aligned. End Task and Kill keep the
/// exact X they have always had; the single dead "Prio +/-" button is replaced
/// by the two buttons its label was promising, inside the SAME dw-90..dw-16
/// strip it already occupied, so nothing else on the footer moves.
pub fn proc_btns(dw: i32) -> [Btn; 4] {
    [
        Btn { x: dw - 250, w: 74 },  // End Task
        Btn { x: dw - 170, w: 74 },  // Kill
        Btn { x: dw - 90, w: 35 },   // Prio -
        Btn { x: dw - 51, w: 35 },   // Prio +
    ]
}

pub fn proc_foot_hit(dw: i32, mx: i32) -> FootAct {
    let b = proc_btns(dw);
    if b[0].hit(mx) { FootAct::EndTask }
    else if b[1].hit(mx) { FootAct::Kill }
    else if b[2].hit(mx) { FootAct::PrioDown }
    else if b[3].hit(mx) { FootAct::PrioUp }
    else { FootAct::None }
}

// ---------------------------------------------------------------------------
// Scheduled tab footer. Deliberately the SAME geometry as the Services tab's
// Start/Stop pair, because it is the same idea and the owner should not have to
// learn two layouts for it.
// ---------------------------------------------------------------------------
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum SchedAct { None, Enable, Disable }

pub fn sched_btns() -> [Btn; 2] {
    [Btn { x: PAD, w: 74 }, Btn { x: PAD + 80, w: 74 }]
}

pub fn sched_foot_hit(mx: i32) -> SchedAct {
    let b = sched_btns();
    if b[0].hit(mx) { SchedAct::Enable }
    else if b[1].hit(mx) { SchedAct::Disable }
    else { SchedAct::None }
}

// ---------------------------------------------------------------------------
// Details tab: the connections scope toggle (this process / all processes).
// ---------------------------------------------------------------------------
pub fn conn_scope_btn(dw: i32) -> Btn { Btn { x: dw - 130, w: 120 } }

// ---------------------------------------------------------------------------
// Update speed. `ms` is what goes to win_get_event()'s timeout; PAUSED still
// uses a real timeout so the window stays responsive to events, it just does
// not re-sample.
// ---------------------------------------------------------------------------
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Speed { High, Normal, Low, Paused }

pub fn speed_ms(s: Speed) -> i32 {
    match s {
        Speed::High => 500,
        Speed::Normal => 1000,
        Speed::Low => 4000,
        Speed::Paused => 1000,
    }
}
pub fn speed_samples(s: Speed) -> bool { !matches!(s, Speed::Paused) }
pub fn speed_cycle(s: Speed) -> Speed {
    match s {
        Speed::High => Speed::Normal,
        Speed::Normal => Speed::Low,
        Speed::Low => Speed::Paused,
        Speed::Paused => Speed::High,
    }
}
pub fn speed_label(s: Speed) -> &'static [u8] {
    match s {
        Speed::High => b"High\0",
        Speed::Normal => b"Normal\0",
        Speed::Low => b"Low\0",
        Speed::Paused => b"Paused\0",
    }
}

/// The four speed buttons on the Performance tab, to the right of the
/// Overall / Per-core pair (which end at PAD + 166).
pub fn speed_btns() -> [Btn; 4] {
    [
        Btn { x: PAD + 220, w: 52 },
        Btn { x: PAD + 276, w: 62 },
        Btn { x: PAD + 342, w: 48 },
        Btn { x: PAD + 394, w: 62 },
    ]
}
pub fn speed_at(mx: i32) -> Option<Speed> {
    let b = speed_btns();
    if b[0].hit(mx) { Some(Speed::High) }
    else if b[1].hit(mx) { Some(Speed::Normal) }
    else if b[2].hit(mx) { Some(Speed::Low) }
    else if b[3].hit(mx) { Some(Speed::Paused) }
    else { None }
}
