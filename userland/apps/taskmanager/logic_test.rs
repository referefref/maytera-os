// logic_test.rs - host-side assertions for taskmgr's pure logic (#188).
//
// Run with `make logic-test` in this directory. It builds for the HOST (std,
// native target), not for MayteraOS, and it `include!`s the very same
// logic.rs text that main.rs includes, so what is asserted here is what ships.
//
// WHY THIS EXISTS RATHER THAN A SCREENSHOT
// ----------------------------------------
// The defect being fixed is "the control renders and does nothing". A
// screenshot of a rendered button is precisely the evidence that CANNOT tell
// the two apart, and QEMU pointer injection on this project does not reliably
// land a click (#334), so "click it and look" is a test rig that fails open.
//
// So each fix below is asserted against a RED ARM: `old_*` functions that are
// verbatim transcriptions of the shipping code at commit 3e38449b. The test
// asserts the OLD function gives the wrong answer for the same input the NEW
// one gets right. If somebody reverts the fix, the green assertion fails; if
// somebody "fixes" the test by copying the new logic into the red arm, the red
// assertion fails. Both arms have to keep disagreeing for the suite to pass.

#![allow(dead_code)]

include!("logic.rs");

// ===========================================================================
// RED ARM: the code as it shipped at 3e38449b, transcribed by hand.
// ===========================================================================

/// main.rs:1177-1178 at 3e38449b, verbatim:
///     if mx >= app.dw - 250 && mx < app.dw - 176 { app.signal_selected(SIGTERM); }
///     else if mx >= app.dw - 170 && mx < app.dw - 96 { app.signal_selected(SIGKILL); }
/// There was no third branch. The "Prio +/-" button drawn at main.rs:784
/// (x = dw-90, w = 74, i.e. dw-90 .. dw-16) is not mentioned anywhere in it.
fn old_proc_foot_hit(dw: i32, mx: i32) -> FootAct {
    if mx >= dw - 250 && mx < dw - 176 { FootAct::EndTask }
    else if mx >= dw - 170 && mx < dw - 96 { FootAct::Kill }
    else { FootAct::None }
}

/// main.rs:784 at 3e38449b, verbatim: the rectangle the old button was DRAWN in.
fn old_prio_btn_drawn(dw: i32) -> Btn { Btn { x: dw - 90, w: 74 } }

/// main.rs:1181-1187 at 3e38449b: outside the footer, the ONLY thing a click in
/// the Processes body could do was select a row, and only at or below ltop.
/// Anything in the header band fell through to nothing.
fn old_body_click(my: i32) -> &'static str {
    let ltop = PAD + TAB_H + 6 + 18;
    if my >= ltop { "row-select" } else { "nothing" }
}

/// draw_scheduled() at 3e38449b ended at main.rs:997 with the empty-list text.
/// There was no footer and no button of any kind on the Scheduled tab, and the
/// EVENT_MOUSE_DOWN chain (main.rs:1169-1210) has no `Tab::Scheduled` branch,
/// so no click anywhere on that tab did anything.
fn old_sched_foot_hit(_mx: i32) -> SchedAct { SchedAct::None }

// ===========================================================================
// Harness
// ===========================================================================
use std::sync::atomic::{AtomicU32, Ordering};
static PASS: AtomicU32 = AtomicU32::new(0);
static FAIL: AtomicU32 = AtomicU32::new(0);

fn check(name: &str, cond: bool, detail: String) {
    if cond {
        PASS.fetch_add(1, Ordering::Relaxed);
        println!("  ok   {:<58} {}", name, detail);
    } else {
        FAIL.fetch_add(1, Ordering::Relaxed);
        println!("  FAIL {:<58} {}", name, detail);
    }
}

fn key(name: &str, pid: u32, state: u32, core: i32, cpu: u32, mem: u32) -> RowKey {
    let mut k = ROWKEY_ZERO;
    let b = name.as_bytes();
    for i in 0..b.len().min(31) { k.name[i] = b[i]; }
    k.pid = pid; k.state = state; k.core = core; k.cpu = cpu; k.mem = mem;
    k
}

fn order(col: SortCol, desc: bool, keys: &[RowKey]) -> Vec<u32> {
    let mut perm = [0usize; 64];
    sort_perm(col, desc, keys, keys.len(), &mut perm);
    (0..keys.len()).map(|i| keys[perm[i]].pid).collect()
}

fn main() {
    // Two window widths: the app's own default, and a narrow one, because
    // every column X is `dw - N` and a fixed-width test proves nothing about
    // the arithmetic.
    let widths = [760i32, 1024, 640];

    println!("== P0.1  Prio button: drawn rectangle vs hit-tested rectangle ==");
    for &dw in widths.iter() {
        let drawn = old_prio_btn_drawn(dw);
        let lo = drawn.x;
        let mid = drawn.x + drawn.w / 2;
        let hi = drawn.x + drawn.w - 1;
        // RED: every pixel of the button that WAS drawn was dead.
        check("RED old: left edge of drawn Prio button -> nothing",
              old_proc_foot_hit(dw, lo) == FootAct::None,
              format!("dw={} mx={} -> {:?}", dw, lo, old_proc_foot_hit(dw, lo)));
        check("RED old: centre of drawn Prio button -> nothing",
              old_proc_foot_hit(dw, mid) == FootAct::None,
              format!("dw={} mx={} -> {:?}", dw, mid, old_proc_foot_hit(dw, mid)));
        check("RED old: right edge of drawn Prio button -> nothing",
              old_proc_foot_hit(dw, hi) == FootAct::None,
              format!("dw={} mx={} -> {:?}", dw, hi, old_proc_foot_hit(dw, hi)));
        // GREEN: the same strip now resolves, and it resolves to a DIRECTION.
        check("GREEN new: left half of the Prio strip -> PrioDown",
              proc_foot_hit(dw, lo + 2) == FootAct::PrioDown,
              format!("dw={} mx={} -> {:?}", dw, lo + 2, proc_foot_hit(dw, lo + 2)));
        check("GREEN new: right half of the Prio strip -> PrioUp",
              proc_foot_hit(dw, hi - 2) == FootAct::PrioUp,
              format!("dw={} mx={} -> {:?}", dw, hi - 2, proc_foot_hit(dw, hi - 2)));
    }

    println!("\n== P0.1  every drawn footer button responds where it is drawn ==");
    for &dw in widths.iter() {
        let want = [FootAct::EndTask, FootAct::Kill, FootAct::PrioDown, FootAct::PrioUp];
        for (i, b) in proc_btns(dw).iter().enumerate() {
            let mut all = true;
            for mx in b.x..(b.x + b.w) {
                if proc_foot_hit(dw, mx) != want[i] { all = false; }
            }
            check("every pixel of the drawn rect maps to its own action",
                  all, format!("dw={} btn{} x={}..{} -> {:?}", dw, i, b.x, b.x + b.w - 1, want[i]));
        }
        // And nothing outside them fires.
        check("gap between Kill and Prio- is inert",
              proc_foot_hit(dw, dw - 95) == FootAct::None,
              format!("dw={} mx={} -> {:?}", dw, dw - 95, proc_foot_hit(dw, dw - 95)));
        check("End Task keeps its original x (dw-250)",
              proc_btns(dw)[0].x == dw - 250, format!("x={}", proc_btns(dw)[0].x));
        check("Kill keeps its original x (dw-170)",
              proc_btns(dw)[1].x == dw - 170, format!("x={}", proc_btns(dw)[1].x));
        check("Prio pair stays inside the old dw-90..dw-16 strip",
              proc_btns(dw)[2].x == dw - 90 && proc_btns(dw)[3].x + proc_btns(dw)[3].w == dw - 16,
              format!("{}..{}", proc_btns(dw)[2].x, proc_btns(dw)[3].x + proc_btns(dw)[3].w));
    }

    println!("\n== P0.2  column headers are hit-tested at all ==");
    for &dw in widths.iter() {
        let c = proc_cols(dw);
        // RED: the header band produced no action whatsoever.
        for my in LIST_HDR_Y..LIST_TOP_Y {
            check("RED old: click in the header band -> nothing",
                  old_body_click(my) == "nothing",
                  format!("my={} -> {}", my, old_body_click(my)));
            break; // one representative row is enough to name; loop below covers the band
        }
        let band_dead = (LIST_HDR_Y..LIST_TOP_Y).all(|my| old_body_click(my) == "nothing");
        check("RED old: the WHOLE header band was dead",
              band_dead, format!("my {}..{}", LIST_HDR_Y, LIST_TOP_Y - 1));
        // GREEN: each header's own X resolves to its own column.
        let cases = [(c.name, SortCol::Name), (c.pid, SortCol::Pid), (c.state, SortCol::State),
                     (c.core, SortCol::Core), (c.cpu, SortCol::Cpu), (c.mem, SortCol::Mem)];
        for (x, want) in cases.iter() {
            check("GREEN new: header X resolves to its own column",
                  header_col_at(dw, *x) == Some(*want),
                  format!("dw={} mx={} -> {:?} (want {:?})", dw, x, header_col_at(dw, *x), want));
        }
        check("outside the list (left of PAD) sorts nothing",
              header_col_at(dw, PAD - 1).is_none(), format!("dw={}", dw));
        check("outside the list (right margin) sorts nothing",
              header_col_at(dw, dw - PAD).is_none(), format!("dw={}", dw));
        // Contiguous: no dead pixel between PAD and dw-PAD.
        let contiguous = (PAD..(dw - PAD)).all(|mx| header_col_at(dw, mx).is_some());
        check("no dead gap anywhere across the header row", contiguous, format!("dw={}", dw));
    }

    println!("\n== P0.2  click-to-sort state machine ==");
    check("clicking the SAME column reverses it",
          sort_next(SortCol::Cpu, true, SortCol::Cpu) == (SortCol::Cpu, false),
          format!("{:?}", sort_next(SortCol::Cpu, true, SortCol::Cpu)));
    check("clicking it a third time reverses back",
          sort_next(SortCol::Cpu, false, SortCol::Cpu) == (SortCol::Cpu, true),
          format!("{:?}", sort_next(SortCol::Cpu, false, SortCol::Cpu)));
    check("a NEW column adopts its natural direction (Name -> ascending)",
          sort_next(SortCol::Cpu, true, SortCol::Name) == (SortCol::Name, false),
          format!("{:?}", sort_next(SortCol::Cpu, true, SortCol::Name)));
    check("a NEW column adopts its natural direction (Mem -> descending)",
          sort_next(SortCol::Name, false, SortCol::Mem) == (SortCol::Mem, true),
          format!("{:?}", sort_next(SortCol::Name, false, SortCol::Mem)));
    check("the default startup state is CPU descending",
          sort_default_desc(SortCol::Cpu), "sort_default_desc(Cpu)".into());
    // The 'S' cycle must visit every column and return home.
    let mut s = SortCol::Cpu;
    let mut seen = vec![];
    for _ in 0..6 { s = sort_cycle(s); seen.push(format!("{:?}", s)); }
    check("the keyboard cycle visits all 6 columns and returns",
          s == SortCol::Cpu && seen.len() == 6, seen.join(" -> "));
    check("glyph shows direction", sort_glyph(true) == b'v' && sort_glyph(false) == b'^',
          "desc=v asc=^".into());

    println!("\n== P0.2  ordering, including the quiet cases ==");
    let rows = vec![
        key("zsh", 7, 2, 1, 5, 900),
        key("Aardvark", 3, 3, -1, 40, 100),
        key("mid", 5, 1, 0, 40, 4000),
    ];
    check("Name ascending is case-insensitive",
          order(SortCol::Name, false, &rows) == vec![3, 5, 7],
          format!("{:?}", order(SortCol::Name, false, &rows)));
    check("Name descending is the exact reverse",
          order(SortCol::Name, true, &rows) == vec![7, 5, 3],
          format!("{:?}", order(SortCol::Name, true, &rows)));
    check("Pid ascending", order(SortCol::Pid, false, &rows) == vec![3, 5, 7],
          format!("{:?}", order(SortCol::Pid, false, &rows)));
    check("Mem descending puts the biggest first",
          order(SortCol::Mem, true, &rows) == vec![5, 7, 3],
          format!("{:?}", order(SortCol::Mem, true, &rows)));
    check("equal CPU ties break on pid, so the order is TOTAL (no 1 Hz jitter)",
          order(SortCol::Cpu, true, &rows) == vec![3, 5, 7],
          format!("{:?} (pids 3 and 5 both at 40%)", order(SortCol::Cpu, true, &rows)));
    check("Core: -1 (not on a core) sorts below core 0",
          order(SortCol::Core, false, &rows) == vec![3, 5, 7],
          format!("{:?}", order(SortCol::Core, false, &rows)));
    // QUIET CASES.
    let one = vec![key("only", 1, 2, 0, 0, 0)];
    check("QUIET: sorting a one-row list is a no-op, not a panic",
          order(SortCol::Name, true, &one) == vec![1], "n=1".into());
    let none: Vec<RowKey> = vec![];
    check("QUIET: sorting an empty list is a no-op, not a panic",
          order(SortCol::Cpu, true, &none).is_empty(), "n=0".into());
    // All rows identical except pid: still a total order, both directions.
    let same = vec![key("same", 9, 1, 0, 0, 0), key("same", 2, 1, 0, 0, 0), key("same", 5, 1, 0, 0, 0)];
    check("QUIET: all-identical rows still order deterministically",
          order(SortCol::Name, false, &same) == vec![2, 5, 9],
          format!("{:?}", order(SortCol::Name, false, &same)));
    check("QUIET: ...and the pid tiebreak does NOT reverse with the column",
          order(SortCol::Name, true, &same) == vec![2, 5, 9],
          format!("{:?}", order(SortCol::Name, true, &same)));
    // n larger than the slice must clamp rather than index out of bounds.
    let mut perm = [0usize; 64];
    sort_perm(SortCol::Pid, false, &rows, 9999, &mut perm);
    check("QUIET: an over-large n clamps instead of reading past the end",
          perm[0] == 1 && perm[1] == 2 && perm[2] == 0, format!("{:?}", &perm[..3]));

    println!("\n== P1.3  Scheduled tab Enable/Disable ==");
    for b in sched_btns().iter() {
        for mx in b.x..(b.x + b.w) {
            check("RED old: no click anywhere on Scheduled did anything",
                  old_sched_foot_hit(mx) == SchedAct::None, format!("mx={}", mx));
            break;
        }
    }
    let red_dead = sched_btns().iter().all(|b| (b.x..(b.x + b.w)).all(|mx| old_sched_foot_hit(mx) == SchedAct::None));
    check("RED old: the whole Scheduled footer strip was dead", red_dead, "".into());
    check("GREEN new: Enable button responds across its whole width",
          (sched_btns()[0].x..(sched_btns()[0].x + sched_btns()[0].w)).all(|mx| sched_foot_hit(mx) == SchedAct::Enable),
          format!("x={}..{}", sched_btns()[0].x, sched_btns()[0].x + sched_btns()[0].w - 1));
    check("GREEN new: Disable button responds across its whole width",
          (sched_btns()[1].x..(sched_btns()[1].x + sched_btns()[1].w)).all(|mx| sched_foot_hit(mx) == SchedAct::Disable),
          format!("x={}..{}", sched_btns()[1].x, sched_btns()[1].x + sched_btns()[1].w - 1));
    check("Scheduled footer geometry MATCHES the Services footer (PAD, PAD+80, w=74)",
          sched_btns()[0] == Btn { x: PAD, w: 74 } && sched_btns()[1] == Btn { x: PAD + 80, w: 74 },
          format!("{:?}", sched_btns()));
    check("the gap between Enable and Disable is inert",
          sched_foot_hit(PAD + 76) == SchedAct::None, format!("mx={}", PAD + 76));

    println!("\n== P2.6  update speed ==");
    check("Normal is the 1000 ms the app has always used",
          speed_ms(Speed::Normal) == 1000, format!("{}", speed_ms(Speed::Normal)));
    check("High is faster, Low is slower",
          speed_ms(Speed::High) < 1000 && speed_ms(Speed::Low) > 1000,
          format!("high={} low={}", speed_ms(Speed::High), speed_ms(Speed::Low)));
    check("Paused stops SAMPLING but keeps a real event timeout (window stays alive)",
          !speed_samples(Speed::Paused) && speed_ms(Speed::Paused) > 0,
          format!("samples={} ms={}", speed_samples(Speed::Paused), speed_ms(Speed::Paused)));
    check("every other speed samples", speed_samples(Speed::High) && speed_samples(Speed::Normal) && speed_samples(Speed::Low), "".into());
    let mut sp = Speed::High;
    for _ in 0..4 { sp = speed_cycle(sp); }
    check("the speed cycle returns to where it started", sp == Speed::High, "".into());
    for (i, b) in speed_btns().iter().enumerate() {
        let want = [Speed::High, Speed::Normal, Speed::Low, Speed::Paused][i];
        check("each speed button responds across its own width",
              (b.x..(b.x + b.w)).all(|mx| speed_at(mx) == Some(want)),
              format!("{:?} x={}..{}", want, b.x, b.x + b.w - 1));
    }
    check("speed buttons never overlap the Overall/Per-core pair (which end at PAD+166)",
          speed_btns()[0].x > PAD + 166, format!("first speed x={}", speed_btns()[0].x));

    let p = PASS.load(Ordering::Relaxed);
    let f = FAIL.load(Ordering::Relaxed);
    println!("\n{} passed, {} failed", p, f);
    if f > 0 { std::process::exit(1); }
}
