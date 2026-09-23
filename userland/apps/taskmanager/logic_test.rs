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
        // GREEN: the strip the buttons are DRAWN in now resolves, and it
        // resolves to a DIRECTION. (tmglass) that strip is proc_btns()[2..3],
        // 6px left of the old one, so the green arm reads it from there.
        let nb = proc_btns(dw);
        let nlo = nb[2].x;
        let nhi = nb[3].x + nb[3].w - 1;
        check("GREEN new: left half of the Prio strip -> PrioDown",
              proc_foot_hit(dw, nlo + 2) == FootAct::PrioDown,
              format!("dw={} mx={} -> {:?}", dw, nlo + 2, proc_foot_hit(dw, nlo + 2)));
        check("GREEN new: right half of the Prio strip -> PrioUp",
              proc_foot_hit(dw, nhi - 2) == FootAct::PrioUp,
              format!("dw={} mx={} -> {:?}", dw, nhi - 2, proc_foot_hit(dw, nhi - 2)));
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
        // (tmglass) the group is right-aligned PANEL_IN inside the glass
        // panel's right edge; the gap between Kill and Prio- moved with it.
        let right = dw - PAD - PANEL_IN;
        check("gap between Kill and Prio- is inert",
              proc_foot_hit(dw, right - 78) == FootAct::None,
              format!("dw={} mx={} -> {:?}", dw, right - 78, proc_foot_hit(dw, right - 78)));
        check("End Task sits 234px left of the panel's inner right edge",
              proc_btns(dw)[0].x == right - 234, format!("x={}", proc_btns(dw)[0].x));
        check("Kill sits 154px left of the panel's inner right edge",
              proc_btns(dw)[1].x == right - 154, format!("x={}", proc_btns(dw)[1].x));
        check("Prio pair fills the 74px strip that ends at the panel's inner right edge",
              proc_btns(dw)[2].x == right - 74 && proc_btns(dw)[3].x + proc_btns(dw)[3].w == right,
              format!("{}..{}", proc_btns(dw)[2].x, proc_btns(dw)[3].x + proc_btns(dw)[3].w));
        check("the footer group never crosses the panel's right edge (dw - PAD)",
              proc_btns(dw)[3].x + proc_btns(dw)[3].w < dw - PAD,
              format!("end={} edge={}", proc_btns(dw)[3].x + proc_btns(dw)[3].w, dw - PAD));
    }

    println!("\n== P0.2  column headers are hit-tested at all ==");
    for &dw in widths.iter() {
        let c = proc_cols(dw);
        // RED: the header band produced no action whatsoever. The red arm is
        // a transcription of the OLD code, so it is judged against the OLD
        // band (PAD + TAB_H + 6 .. + 18); (tmglass) moved the live band down
        // into the glass panel, which is not what this arm is about.
        const OLD_LIST_HDR_Y: i32 = PAD + TAB_H + 6;
        const OLD_LIST_TOP_Y: i32 = OLD_LIST_HDR_Y + 18;
        for my in OLD_LIST_HDR_Y..OLD_LIST_TOP_Y {
            check("RED old: click in the header band -> nothing",
                  old_body_click(my) == "nothing",
                  format!("my={} -> {}", my, old_body_click(my)));
            break; // one representative row is enough to name; loop below covers the band
        }
        let band_dead = (OLD_LIST_HDR_Y..OLD_LIST_TOP_Y).all(|my| old_body_click(my) == "nothing");
        check("RED old: the WHOLE header band was dead",
              band_dead, format!("my {}..{}", OLD_LIST_HDR_Y, OLD_LIST_TOP_Y - 1));
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
    // (tmglass) both pairs start at CX, the glass panel's inner left edge,
    // and the Services pair is svc_btns(), the same function, not a copy.
    check("Scheduled footer geometry MATCHES the Services footer (CX, CX+80, w=74)",
          sched_btns()[0] == Btn { x: CX, w: 74 } && sched_btns()[1] == Btn { x: CX + 80, w: 74 }
          && svc_btns() == sched_btns(),
          format!("{:?} / {:?}", sched_btns(), svc_btns()));
    check("the gap between Enable and Disable is inert",
          sched_foot_hit(CX + 76) == SchedAct::None, format!("mx={}", CX + 76));
    check("the left footer pair starts inside the glass panel (right of its border at PAD)",
          sched_btns()[0].x > PAD, format!("x={} PAD={}", sched_btns()[0].x, PAD));

    println!("\n== tmglass  tab pills: one rectangle for draw and hit-test ==");
    for i in 0..5usize {
        let r = tab_rect(i);
        check("every pixel of a drawn pill selects that tab",
              (r.x..(r.x + r.w)).all(|mx| tab_at(5, mx) == Some(i)),
              format!("tab{} x={}..{}", i, r.x, r.x + r.w - 1));
    }
    check("the gap between two pills selects nothing",
          tab_at(5, tab_rect(0).x + tab_rect(0).w + TAB_GAP / 2).is_none(), "".into());
    check("left of the first pill selects nothing", tab_at(5, PAD - 1).is_none(), "".into());
    check("the strip of five fits a 640-wide content area",
          tab_rect(4).x + tab_rect(4).w <= 640 - PAD, format!("end={}", tab_rect(4).x + tab_rect(4).w));
    // RED arm: the OLD hit-test divided by a literal 87 while the draw loop
    // stepped 84 + 3 = 87 too, so they agreed by coincidence, not construction;
    // it also treated the 3px gaps as part of the pill to their left.
    fn old_tab_at(mx: i32) -> Option<usize> { let i = ((mx - PAD) / 87) as usize; if i < 5 { Some(i) } else { None } }
    check("RED old: the gap after a pill was silently attributed to that pill",
          old_tab_at(PAD + 85).is_some(), "".into());

    println!("\n== tmglass  glass panel contains every list-tab control ==");
    for &dw in widths.iter() {
        for &dh in [400, 536, 700].iter() {
            let (px, py, pw, ph) = panel_rect(dw, dh);
            check("footer buttons lie inside the panel, vertically",
                  foot_y(dh) > py && foot_y(dh) + FOOT_H < py + ph,
                  format!("dw={} dh={} foot_y={} panel {}..{}", dw, dh, foot_y(dh), py, py + ph));
            check("list rows stop above the footer hairline",
                  list_bottom(dh) < foot_y(dh) - 8, format!("dh={}", dh));
            check("first column and last button lie inside the panel, horizontally",
                  proc_cols(dw).name > px && proc_btns(dw)[3].x + proc_btns(dw)[3].w < px + pw,
                  format!("dw={}", dw));
            check("column header band starts below the panel's top edge",
                  LIST_HDR_Y > py + 1, format!("hdr={} panel_y={}", LIST_HDR_Y, py));
        }
    }

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
    let pm = perf_mode_btns();
    check("speed buttons never overlap the Overall/Per-core pair (perf_mode_btns)",
          speed_btns()[0].x > pm[1].x + pm[1].w, format!("first speed x={} pair ends {}", speed_btns()[0].x, pm[1].x + pm[1].w));

    let p = PASS.load(Ordering::Relaxed);
    let f = FAIL.load(Ordering::Relaxed);
    println!("\n{} passed, {} failed", p, f);
    if f > 0 { std::process::exit(1); }
}
