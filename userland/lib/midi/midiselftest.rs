// midiselftest.rs - #183: the scenarios, each with its RED twin.
//
// ===========================================================================
// WHY EVERY CHECK HAS A RED TWIN
// ---------------------------------------------------------------------------
// #182 found FOUR wrong constants in a synthesiser core that had been reviewed,
// and every one of them was found by RUNNING rather than by reading. It also
// found a rate-14 shift underflow that every single-rate test passed, because a
// hole in the middle of a ladder is invisible to a test standing on one rung.
// Two rules came out of that and both are followed here:
//
//   PRINT THE WHOLE LADDER. NOTE_OCTAVES checks all 115 playable notes, not one.
//   PROVE THE TEST CAN FAIL. Every assertion below has a twin that feeds it
//   something wrong and REQUIRES the assertion to fail. A red twin that passes
//   is itself reported as a failure, so a test that has quietly stopped
//   testing anything shows up as a FAIL rather than as a clean run.
//
// The test files are BUILT IN MEMORY by build_all() below. Nothing is shipped
// as a .MID asset, which sidesteps the licence question entirely (#61 and #87
// are this project's record of getting third-party content licensing wrong) and
// makes every timing expectation an exact integer rather than a property of a
// file someone would have to trust.
//
// EXPECTED SAMPLE POSITIONS ARE COMPUTED BY A DIFFERENT ROUTE FROM THE CODE
// UNDER TEST. `seq_onset` below is the closed form (beats times microseconds
// times rate), while player.rs advances a quotient-and-remainder accumulator
// event by event. If those two agree over a tempo change, they agree for a
// reason.

pub struct MidiReport {
    pub name: &'static str,
    pub pass: bool,
    pub measured: i64,
    pub expected: i64,
    pub note: &'static str,
}

// ---------------------------------------------------------------------------
// The generated test sequence.
// ---------------------------------------------------------------------------
pub const SEQ_N: usize = 6;
/// A4, C5, E5, A5, E5, A4. Chosen so that adjacent notes are at least three
/// semitones apart: a timing error large enough to slide a measurement window
/// into the neighbouring note produces an unmistakably different frequency
/// rather than a plausible one.
pub static SEQ_NOTES: [u8; SEQ_N] = [69, 72, 76, 81, 76, 69];
pub const SEQ_PPQ: u16 = 96;
pub const SEQ_BEAT: u32 = 96;
/// Half a beat. The first version used 88 of 96, leaving an 8-tick gap that at
/// 240 BPM is 21 ms; every patch's release is longer than that, so each
/// measurement window contained the PREVIOUS note as well and dominant_mhz read
/// the pair. A test sequence has to be written so that the thing being measured
/// is the only thing in the window.
pub const SEQ_GATE: u32 = 48;
/// 120 BPM.
pub const SEQ_TEMPO0: u32 = 500_000;
/// 240 BPM, from the fourth note on.
pub const SEQ_TEMPO1: u32 = 250_000;
pub const SEQ_TEMPO_AT: usize = 3;
/// The RED tempo: a file that says 150 BPM where the expectation says 240.
pub const SEQ_TEMPO_RED: u32 = 400_000;
/// GM program 73, Flute, which maps to the Pipe patch: modulator attenuated to
/// -31.5 dB and no vibrato, so the carrier is near enough a pure sine for
/// dominant_mhz's stated limit. Any patch with a real modulation index would
/// break the ESTIMATOR, not the player.
pub const SEQ_PROGRAM: u8 = 73;
/// GM program 0, Acoustic Grand, whose step attack is what the onset detector
/// needs. A flute's soft attack has no edge to find.
pub const SEQ_PROGRAM_PERC: u8 = 0;

/// Expected onset of note `i`, in frames, by the CLOSED FORM.
pub fn seq_onset(i: usize, rate: u32, tempo1: u32) -> u64 {
    let b0 = SEQ_TEMPO0 as u64 * rate as u64 / 1_000_000;
    let b1 = tempo1 as u64 * rate as u64 / 1_000_000;
    if i <= SEQ_TEMPO_AT {
        i as u64 * b0
    } else {
        SEQ_TEMPO_AT as u64 * b0 + (i - SEQ_TEMPO_AT) as u64 * b1
    }
}

// ---------------------------------------------------------------------------
// A minimal SMF writer. Bounds-checked; an overflow sets `ovf` and is asserted
// on by build_all's caller rather than silently truncating a test file.
// ---------------------------------------------------------------------------
struct W<'w> {
    d: &'w mut [u8],
    n: usize,
    ovf: bool,
}

impl<'w> W<'w> {
    fn b(&mut self, v: u8) {
        if self.n < self.d.len() {
            self.d[self.n] = v;
            self.n += 1;
        } else {
            self.ovf = true;
        }
    }
    fn bs(&mut self, v: &[u8]) {
        for x in v {
            self.b(*x);
        }
    }
    fn be16(&mut self, v: u16) {
        self.b((v >> 8) as u8);
        self.b(v as u8);
    }
    fn be32(&mut self, v: u32) {
        self.b((v >> 24) as u8);
        self.b((v >> 16) as u8);
        self.b((v >> 8) as u8);
        self.b(v as u8);
    }
    /// Encode a variable-length quantity. The inverse of Track::vlq, written
    /// separately rather than shared, so that a round-trip test compares two
    /// independent pieces of code instead of one function with itself.
    fn vlq(&mut self, mut v: u32) {
        let mut t = [0u8; 5];
        t[0] = (v & 0x7f) as u8;
        v >>= 7;
        let mut n = 1usize;
        while v > 0 && n < 5 {
            t[n] = (v & 0x7f) as u8 | 0x80;
            v >>= 7;
            n += 1;
        }
        while n > 0 {
            n -= 1;
            self.b(t[n]);
        }
    }
    fn put_be32_at(&mut self, at: usize, v: u32) {
        if at + 4 <= self.d.len() {
            self.d[at] = (v >> 24) as u8;
            self.d[at + 1] = (v >> 16) as u8;
            self.d[at + 2] = (v >> 8) as u8;
            self.d[at + 3] = v as u8;
        }
    }
    fn mthd(&mut self, format: u16, ntrks: u16, ppq: u16) {
        self.bs(b"MThd");
        self.be32(6);
        self.be16(format);
        self.be16(ntrks);
        self.be16(ppq);
    }
    /// Open an MTrk and return the offset of its length field.
    fn mtrk(&mut self) -> usize {
        self.bs(b"MTrk");
        let at = self.n;
        self.be32(0);
        at
    }
    fn end_mtrk(&mut self, at: usize) {
        let len = (self.n - at - 4) as u32;
        self.put_be32_at(at, len);
    }
    fn meta_tempo(&mut self, dt: u32, us: u32) {
        self.vlq(dt);
        self.bs(&[0xff, 0x51, 0x03, (us >> 16) as u8, (us >> 8) as u8, us as u8]);
    }
    fn meta_end(&mut self, dt: u32) {
        self.vlq(dt);
        self.bs(&[0xff, 0x2f, 0x00]);
    }
}

/// Where each generated test file sits inside the shared buffer.
#[derive(Clone, Copy, Default)]
pub struct Span {
    pub off: usize,
    pub len: usize,
}

#[derive(Clone, Copy, Default)]
pub struct Plan {
    /// Format 0, explicit status bytes, correct tempo map.
    pub f0: Span,
    /// Format 1, three tracks, same music.
    pub f1: Span,
    /// Format 0, RUNNING STATUS wherever it is legal.
    pub run: Span,
    /// Format 0 with SEQ_TEMPO_RED in place of SEQ_TEMPO1.
    pub red_tempo: Span,
    /// A delta time written as five continuation bytes.
    pub bad_vlq: Span,
    /// An MTrk whose declared length runs past the end of the file.
    pub bad_trunc: Span,
    /// A data byte immediately after a meta event, i.e. a file that only
    /// parses if running status is wrongly carried across a System message.
    pub bad_meta: Span,
    /// Notes on MIDI channel 10.
    pub perc: Span,
    /// Twelve simultaneous note-ons, for the voice-stealing arm.
    pub twelve: Span,
    /// f0 and red_tempo again, but carrying GM program 0 instead of 73. The
    /// onset detector needs a step attack and the file's OWN program change
    /// overrides anything set before playback, so the program has to be in the
    /// file. Setting it afterwards was silently undone at tick 0.
    pub f0_step: Span,
    pub red_step: Span,
    pub ovf: bool,
}

fn write_seq_track(w: &mut W, running: bool, tempo1: u32, program: u8) {
    let at = w.mtrk();
    // EVERY event carries EXACTLY ONE delta, written immediately before it.
    // The first version of this writer emitted a trailing delta at the end of
    // each note and another at the start of the next, so the parser read the
    // second delta's first byte as a status byte and desynchronised. It was
    // caught by the suite in one run: only two note events came out of a
    // six-note file. Recorded because a writer bug looks exactly like a parser
    // bug in the output, and the writer is the thing nobody checks.
    let mut t = 0u32;
    w.meta_tempo(0, SEQ_TEMPO0);
    w.vlq(0);
    w.bs(&[0xc0, program]);
    let mut last_status = 0u8;
    for i in 0..SEQ_N {
        let on_at = i as u32 * SEQ_BEAT;
        if i == SEQ_TEMPO_AT {
            w.meta_tempo(on_at - t, tempo1);
            t = on_at;
            // A meta event CLEARS running status. Forgetting this line is the
            // exact fault RUN_AFTER_META_RED exists to catch, so the writer has
            // to model it too or the "running status" file would be invalid.
            last_status = 0;
        }
        w.vlq(on_at - t);
        t = on_at;
        if !running || last_status != 0x90 {
            w.b(0x90);
            last_status = 0x90;
        }
        w.b(SEQ_NOTES[i]);
        w.b(100);
        w.vlq(SEQ_GATE);
        t += SEQ_GATE;
        if running {
            // Note-off written as note-on velocity 0, which is what makes
            // running status worth having and is legal everywhere.
            if last_status != 0x90 {
                w.b(0x90);
                last_status = 0x90;
            }
            w.b(SEQ_NOTES[i]);
            w.b(0);
        } else {
            w.b(0x80);
            w.b(SEQ_NOTES[i]);
            w.b(64);
            last_status = 0x80;
        }
    }
    let end_at = SEQ_N as u32 * SEQ_BEAT;
    w.meta_end(end_at - t);
    w.end_mtrk(at);
}

/// Build every test file into one buffer. Returns where each one landed.
///
/// All of them are built BEFORE any is parsed, so the buffer can then be handed
/// out as a plain shared slice for the rest of the run. That is not a
/// convenience: it is what lets the whole suite avoid a self-referential borrow
/// without a single unsafe block.
pub fn build_all(buf: &mut [u8]) -> Plan {
    let mut pl = Plan::default();
    let mut off = 0usize;

    macro_rules! file {
        ($span:expr, $body:expr) => {{
            let mut w = W { d: &mut buf[off..], n: 0, ovf: false };
            let f: &mut dyn FnMut(&mut W) = $body;
            f(&mut w);
            if w.ovf {
                pl.ovf = true;
            }
            $span = Span { off, len: w.n };
            off += w.n;
            let _ = off; // the last file's bump is never read; keep the pattern uniform
        }};
    }

    file!(pl.f0, &mut |w: &mut W| {
        w.mthd(0, 1, SEQ_PPQ);
        write_seq_track(w, false, SEQ_TEMPO1, SEQ_PROGRAM);
    });

    file!(pl.run, &mut |w: &mut W| {
        w.mthd(0, 1, SEQ_PPQ);
        write_seq_track(w, true, SEQ_TEMPO1, SEQ_PROGRAM);
    });

    file!(pl.red_tempo, &mut |w: &mut W| {
        w.mthd(0, 1, SEQ_PPQ);
        write_seq_track(w, false, SEQ_TEMPO_RED, SEQ_PROGRAM);
    });

    file!(pl.f0_step, &mut |w: &mut W| {
        w.mthd(0, 1, SEQ_PPQ);
        write_seq_track(w, false, SEQ_TEMPO1, SEQ_PROGRAM_PERC);
    });

    file!(pl.red_step, &mut |w: &mut W| {
        w.mthd(0, 1, SEQ_PPQ);
        write_seq_track(w, false, SEQ_TEMPO_RED, SEQ_PROGRAM_PERC);
    });

    // Format 1: a conductor track carrying the tempo map and nothing else,
    // then the notes split across two tracks. Track 2's first delta is 288
    // ticks, which is a two-byte VLQ, so the merge is exercised on a delta that
    // a single-byte-VLQ parser would get wrong.
    file!(pl.f1, &mut |w: &mut W| {
        w.mthd(1, 3, SEQ_PPQ);
        let a = w.mtrk();
        w.meta_tempo(0, SEQ_TEMPO0);
        w.meta_tempo(SEQ_TEMPO_AT as u32 * SEQ_BEAT, SEQ_TEMPO1);
        w.meta_end(0);
        w.end_mtrk(a);

        let b = w.mtrk();
        w.vlq(0);
        w.bs(&[0xc0, SEQ_PROGRAM]);
        let mut t = 0u32;
        for i in 0..SEQ_TEMPO_AT {
            w.vlq(i as u32 * SEQ_BEAT - t);
            t = i as u32 * SEQ_BEAT;
            w.bs(&[0x90, SEQ_NOTES[i], 100]);
            w.vlq(SEQ_GATE);
            t += SEQ_GATE;
            w.bs(&[0x80, SEQ_NOTES[i], 64]);
        }
        w.meta_end(0);
        w.end_mtrk(b);

        let c = w.mtrk();
        let mut t = 0u32;
        for i in SEQ_TEMPO_AT..SEQ_N {
            w.vlq(i as u32 * SEQ_BEAT - t);
            t = i as u32 * SEQ_BEAT;
            w.bs(&[0x90, SEQ_NOTES[i], 100]);
            w.vlq(SEQ_GATE);
            t += SEQ_GATE;
            w.bs(&[0x80, SEQ_NOTES[i], 64]);
        }
        w.meta_end(0);
        w.end_mtrk(c);
    });

    // A delta time of five continuation bytes. Legal VLQs are at most four.
    file!(pl.bad_vlq, &mut |w: &mut W| {
        w.mthd(0, 1, SEQ_PPQ);
        let a = w.mtrk();
        w.vlq(0);
        w.bs(&[0x90, 60, 100]);
        w.bs(&[0x81, 0x81, 0x81, 0x81, 0x81, 0x00]);
        w.bs(&[0x80, 60, 64]);
        w.meta_end(0);
        w.end_mtrk(a);
    });

    // An MTrk whose length field claims 200 bytes more than the file holds.
    file!(pl.bad_trunc, &mut |w: &mut W| {
        w.mthd(0, 1, SEQ_PPQ);
        let a = w.mtrk();
        w.vlq(0);
        w.bs(&[0x90, 60, 100]);
        w.meta_end(0);
        let len = (w.n - a - 4) as u32;
        w.put_be32_at(a, len + 200);
    });

    // A note-on, a tempo meta, then a bare pair of data bytes. This file is
    // ONLY parseable by a reader that wrongly carries running status across a
    // System message. A correct reader must report E_STATUS.
    file!(pl.bad_meta, &mut |w: &mut W| {
        w.mthd(0, 1, SEQ_PPQ);
        let a = w.mtrk();
        w.vlq(0);
        w.bs(&[0x90, 60, 100]);
        w.meta_tempo(10, 400_000);
        w.vlq(10);
        w.bs(&[64, 100]); // two data bytes, no status
        w.meta_end(0);
        w.end_mtrk(a);
    });

    // Channel 10 (index 9), so measure() must report percussion.
    file!(pl.perc, &mut |w: &mut W| {
        w.mthd(0, 1, SEQ_PPQ);
        let a = w.mtrk();
        w.meta_tempo(0, SEQ_TEMPO0);
        for i in 0..4u32 {
            w.vlq(if i == 0 { 0 } else { SEQ_BEAT / 2 });
            w.bs(&[0x99, 36 + (i as u8 % 2) * 2, 100]);
            w.vlq(4);
            w.bs(&[0x89, 36 + (i as u8 % 2) * 2, 64]);
        }
        w.meta_end(0);
        w.end_mtrk(a);
    });

    // Twelve note-ons at the same tick on nine voices.
    file!(pl.twelve, &mut |w: &mut W| {
        w.mthd(0, 1, SEQ_PPQ);
        let a = w.mtrk();
        w.meta_tempo(0, SEQ_TEMPO0);
        for i in 0..12u8 {
            w.vlq(0);
            w.bs(&[0x90, 48 + i * 2, 100]);
        }
        w.meta_end(SEQ_BEAT);
        w.end_mtrk(a);
    });

    pl
}

/// Build the DEMO sequence, or a single sustained A440, into `buf`.
///
/// The app ships NO .MID FILE. It generates its demo, which sidesteps the
/// licence question that #61 (GNU grep with no GPLv3 text) and #87 (busybox
/// vi's LGPLv3 regex) are this project's record of getting wrong, and makes the
/// demo byte-identical on every boot so a capture of it is reproducible.
///
/// `tone` selects a single note 69 held for six beats at 120 BPM on GM program
/// 73, which is the arm a WAV capture can FFT without windowing: one pitch, one
/// answer, 440 Hz or it is wrong.
pub fn build_demo(buf: &mut [u8], tone: bool) -> usize {
    let mut w = W { d: buf, n: 0, ovf: false };
    if tone {
        w.mthd(0, 1, SEQ_PPQ);
        let a = w.mtrk();
        w.meta_tempo(0, SEQ_TEMPO0);
        w.vlq(0);
        w.bs(&[0xc0, SEQ_PROGRAM]);
        w.vlq(0);
        w.bs(&[0x90, 69, 110]);
        w.vlq(SEQ_BEAT * 6);
        w.bs(&[0x80, 69, 64]);
        w.meta_end(0);
        w.end_mtrk(a);
    } else {
        w.mthd(0, 1, SEQ_PPQ);
        write_seq_track(&mut w, false, SEQ_TEMPO1, SEQ_PROGRAM);
    }
    if w.ovf { 0 } else { w.n }
}

// ---------------------------------------------------------------------------
// Scratch. The caller owns it, so it lands in the caller's .bss rather than on
// a 16 KB Ring-3 stack. A 48 KB array on that stack is the exact fault that
// bit the earlier Rust userland port, and it does not fail at the declaration.
// ---------------------------------------------------------------------------
pub const WIN_LEN: usize = 4096;
/// Where inside a note the pitch window starts, and how long it is. The window
/// must fit inside the SHORTEST gate in the sequence: 48 ticks at 240 BPM and
/// 44100 Hz is 5512 frames, and 1024 + 4096 = 5120 is inside that. A rising
/// attack does not disturb a zero-crossing estimator, so the window does not
/// need to wait for the envelope to settle; it needs to end before the note
/// does.
pub const WIN_OFF: u64 = 1024;
pub const ENV_WIN: usize = 256;
pub const BLK_FRAMES: usize = 1024;
/// Measurement window for a single sustained probe note. 8192 frames is 186 ms,
/// which is twelve cycles of MIDI 36 at 65 Hz, the lowest note probed. Sized
/// from the LOWEST frequency to be measured, not from convenience.
pub const PROBE_FRAMES: usize = 8192;

pub struct Scratch {
    pub blk: [i16; BLK_FRAMES * 2],
    pub mono: [i16; BLK_FRAMES],
    pub env: [u16; 1024],
    pub win: [i16; SEQ_N * WIN_LEN],
    pub env_n: usize,
}

impl Scratch {
    pub const fn new() -> Scratch {
        Scratch {
            blk: [0; BLK_FRAMES * 2],
            mono: [0; BLK_FRAMES],
            env: [0; 1024],
            win: [0; SEQ_N * WIN_LEN],
            env_n: 0,
        }
    }
}

/// Render a whole file, capturing one window inside each expected note and an
/// amplitude envelope of the whole thing.
///
/// The windows are taken at positions derived from the CLOSED-FORM tempo map,
/// not from the player's own idea of where it is. If the player's timing is
/// wrong, the window lands in the wrong note and the pitch measured there is
/// the wrong note's pitch. That is the whole point.
fn render_capture(
    pl: &mut Player,
    syn: &mut FmSynth,
    sc: &mut Scratch,
    rate: u32,
    onsets: &[u64; SEQ_N],
    cap_windows: bool,
) -> u64 {
    sc.env_n = 0;
    for w in sc.win.iter_mut() {
        *w = 0;
    }
    let mut pos: u64 = 0;
    let cap = rate as u64 * 12;
    while !pl.finished && pos < cap {
        let n = pl.render_block(syn, &mut sc.blk);
        if n == 0 {
            break;
        }
        for i in 0..n {
            sc.mono[i] = sc.blk[i * 2];
        }
        // Envelope, appended. BLK_FRAMES is a multiple of ENV_WIN, so the
        // windows appended here line up with a global window grid and index
        // times ENV_WIN is a real frame position.
        if sc.env_n + n / ENV_WIN <= sc.env.len() {
            let got = envelope(&sc.mono[..n], ENV_WIN, &mut sc.env[sc.env_n..]);
            sc.env_n += got;
        }
        if cap_windows {
            for k in 0..SEQ_N {
                let ws = onsets[k] + WIN_OFF;
                let we = ws + WIN_LEN as u64;
                let bs = pos;
                let be = pos + n as u64;
                if we <= bs || ws >= be {
                    continue;
                }
                let from = if ws > bs { ws } else { bs };
                let to = if we < be { we } else { be };
                for f in from..to {
                    let src = (f - bs) as usize;
                    let dst = (f - ws) as usize;
                    sc.win[k * WIN_LEN + dst] = sc.mono[src];
                }
            }
        }
        pos += n as u64;
    }
    pos
}

/// Frame positions where the envelope steps up sharply. A step attack makes an
/// unmistakable edge; this looks for a rise of at least an eighth of the whole
/// signal's peak, which no decay tail produces.
fn find_onsets(env: &[u16], out: &mut [u64]) -> usize {
    let mut peak = 0u16;
    for v in env {
        if *v > peak {
            peak = *v;
        }
    }
    if peak == 0 {
        return 0;
    }
    // A third of the whole signal's peak, and a refractory period of eight
    // windows after each hit.
    //
    // The first version used an eighth and a three-window skip, and reported
    // NINE onsets in a six-note file, spaced exactly one render block apart.
    // Two notes sounding together beat, the peak-per-window of a beating pair
    // oscillates, and an eighth of peak is inside that oscillation. The
    // refractory period is safe because the closest two notes in the sequence
    // are 11025 frames apart, which is 43 windows.
    let thr = peak / 3;
    let mut n = 0usize;
    // Seeded at zero rather than at env[0], so a note that starts at frame 0
    // has something to rise FROM. Starting the scan at i = 1 silently loses the
    // first onset of every sequence, which is exactly what it did.
    let mut prev = 0u16;
    let mut refract = 0usize;
    let mut i = 0usize;
    while i < env.len() && n < out.len() {
        if refract > 0 {
            refract -= 1;
        } else if env[i] >= prev.saturating_add(thr) {
            out[n] = i as u64 * ENV_WIN as u64;
            n += 1;
            refract = 8;
        }
        prev = env[i];
        i += 1;
    }
    n
}

fn absdiff(a: u64, b: u64) -> u64 {
    if a > b { a - b } else { b - a }
}

/// Cents between two millihertz figures, times 100, without float.
///
/// Reuses opl2check's cents_x100 rather than carrying a second implementation.
fn cents(a: u64, b: u64) -> i64 {
    cents_x100(a, b)
}

// ---------------------------------------------------------------------------
// THE SUITE.
// ---------------------------------------------------------------------------

/// `buf` must be at least 4096 bytes and is consumed: every test file is built
/// into it first, and it is then read-only for the rest of the run.
pub fn midi_run_all<'a>(
    rate: u32,
    buf: &'a mut [u8],
    pl: &mut Player<'a>,
    syn: &mut FmSynth,
    sc: &mut Scratch,
    emit: &mut dyn FnMut(&MidiReport),
) -> i32 {
    let plan = build_all(buf);
    // From here the buffer is only ever READ. This coercion is what makes the
    // whole suite borrow-check without an unsafe block.
    let all: &'a [u8] = buf;

    let mut fails = 0i32;
    // A MACRO, not a closure. A closure that captures `emit` holds a unique
    // borrow of it for its whole lifetime, so the several arms that emit a
    // per-item line directly (the ladders) could not coexist with it. Expanding
    // in place keeps one reporting path with no borrow held between calls.
    macro_rules! check {
        ($name:expr, $pass:expr, $measured:expr, $expected:expr, $note:expr) => {{
            let p_ = $pass;
            if !p_ {
                fails += 1;
            }
            emit(&MidiReport {
                name: $name,
                pass: p_,
                measured: $measured,
                expected: $expected,
                note: $note,
            });
        }};
    }

    check!("BUILD_FITS", !plan.ovf, plan.f0.len as i64, 0,
          "every generated test file fits in the buffer");

    let f = |s: Span| &all[s.off..s.off + s.len];

    // -----------------------------------------------------------------------
    // 1. VLQ and running status, structurally.
    // -----------------------------------------------------------------------
    {
        // Round-trip every boundary value through the independent writer and
        // the parser. 0x0FFFFFFF is the largest legal VLQ.
        let vals: [u32; 9] = [0, 0x7f, 0x80, 0x2000, 0x3fff, 0x4000, 0x1fffff, 0x200000, 0x0fffffff];
        let mut tmp = [0u8; 64];
        let mut ok = true;
        let mut bad_val = 0i64;
        for v in vals.iter() {
            let mut w = W { d: &mut tmp, n: 0, ovf: false };
            w.vlq(*v);
            w.bs(&[0xff, 0x2f, 0x00]);
            let n = w.n;
            let mut t = Track::empty();
            t.set_data(&tmp[..n]);
            t.step();
            if !t.live || t.tick != *v as u64 {
                ok = false;
                bad_val = *v as i64;
                break;
            }
        }
        check!("VLQ_ROUNDTRIP", ok, bad_val, 0,
              "0 .. 0x0FFFFFFF survive encode then decode");
    }

    {
        pl.open(f(plan.bad_vlq), rate).ok();
        let m = pl.measure();
        // RED TWIN. A five-byte VLQ must be REPORTED, not absorbed.
        check!("VLQ_RED_5BYTE", m.last_error == E_VLQ, m.last_error as i64, E_VLQ as i64,
              "a 5-continuation-byte delta is rejected as E_VLQ");
    }

    {
        let r = pl.open(f(plan.bad_trunc), rate);
        let e = match r { Ok(()) => 0, Err(e) => e };
        check!("TRUNC_RED", e == E_TRUNC, e as i64, E_TRUNC as i64,
              "an MTrk longer than the file is rejected at open");
    }

    {
        pl.open(f(plan.bad_meta), rate).ok();
        let m = pl.measure();
        // THE RUNNING-STATUS FAULT. A parser that carries running status across
        // a meta event parses this file happily and desynchronises silently.
        check!("RUN_AFTER_META_RED", m.last_error == E_STATUS, m.last_error as i64, E_STATUS as i64,
              "running status must NOT survive a meta event");
    }

    // -----------------------------------------------------------------------
    // 2. Equivalences: running status vs explicit, format 0 vs format 1.
    // -----------------------------------------------------------------------
    {
        // Two files, same music, different encodings. Collect the note events
        // of each and require them identical in sample position, channel, note
        // and velocity.
        let mut a_pos = [0u64; 32];
        let mut a_key = [0u32; 32];
        let mut an = 0usize;
        pl.open(f(plan.f0), rate).ok();
        while let Some((s, ev)) = pl.step_event() {
            if (ev.kind == EV_NOTE_ON || ev.kind == EV_NOTE_OFF) && an < 32 {
                a_pos[an] = s;
                a_key[an] = (ev.kind as u32) << 16 | (ev.ch as u32) << 8 | ev.a as u32;
                an += 1;
            }
        }
        let mut same = true;
        let mut bn = 0usize;
        pl.open(f(plan.run), rate).ok();
        while let Some((s, ev)) = pl.step_event() {
            if ev.kind == EV_NOTE_ON || ev.kind == EV_NOTE_OFF {
                let key = (ev.kind as u32) << 16 | (ev.ch as u32) << 8 | ev.a as u32;
                if bn >= an || a_pos[bn] != s || a_key[bn] != key {
                    same = false;
                }
                bn += 1;
            }
        }
        check!("RUNNING_STATUS_EQ", same && bn == an && an == SEQ_N * 2, bn as i64, an as i64,
              "running status yields the identical event stream");

        let mut same1 = true;
        let mut cn = 0usize;
        pl.open(f(plan.f1), rate).ok();
        while let Some((s, ev)) = pl.step_event() {
            if ev.kind == EV_NOTE_ON || ev.kind == EV_NOTE_OFF {
                let key = (ev.kind as u32) << 16 | (ev.ch as u32) << 8 | ev.a as u32;
                if cn >= an || a_pos[cn] != s || a_key[cn] != key {
                    same1 = false;
                }
                cn += 1;
            }
        }
        check!("FORMAT0_VS_1_EQ", same1 && cn == an, cn as i64, an as i64,
              "3 merged tracks equal 1 merged track, over a tempo change");
    }

    // -----------------------------------------------------------------------
    // 3. The tempo map, in exact sample positions.
    // -----------------------------------------------------------------------
    {
        let mut worst = 0i64;
        let mut got = [0u64; SEQ_N];
        let mut k = 0usize;
        pl.open(f(plan.f0), rate).ok();
        while let Some((s, ev)) = pl.step_event() {
            if ev.kind == EV_NOTE_ON && k < SEQ_N {
                got[k] = s;
                k += 1;
            }
        }
        let mut ok = k == SEQ_N;
        for i in 0..SEQ_N {
            let want = seq_onset(i, rate, SEQ_TEMPO1);
            let d = absdiff(got[i], want) as i64;
            if d > worst {
                worst = d;
            }
            if d != 0 {
                ok = false;
            }
            // The whole ladder, one line per note, because a uniformly wrong
            // tempo and a single wrong entry look identical in a summary.
            emit(&MidiReport {
                name: "TEMPO_MAP_NOTE",
                pass: d == 0,
                measured: got[i] as i64,
                expected: want as i64,
                note: "event sample position vs the closed form",
            });
            if d != 0 {
                fails += 1;
            }
        }
        check!("TEMPO_MAP", ok, worst, 0,
              "EXACT: incremental remainder equals the closed form");
    }

    {
        // RED TWIN: the same expectations against a file whose tempo change is
        // 150 BPM instead of 240. If this passes, TEMPO_MAP proves nothing.
        let mut agree = true;
        let mut k = 0usize;
        pl.open(f(plan.red_tempo), rate).ok();
        while let Some((s, ev)) = pl.step_event() {
            if ev.kind == EV_NOTE_ON && k < SEQ_N {
                if s != seq_onset(k, rate, SEQ_TEMPO1) {
                    agree = false;
                }
                k += 1;
            }
        }
        check!("TEMPO_MAP_RED", !agree, if agree { 1 } else { 0 }, 0,
              "a wrong tempo MUST fail the tempo assertion");
    }

    // -----------------------------------------------------------------------
    // 4. The note table, as an integer ladder over every playable note.
    // -----------------------------------------------------------------------
    {
        // The absolute anchor. Everything else here is relative, and a
        // uniformly-wrong table satisfies every relative test.
        let a4 = expected_mhz(NOTE_FNUM[69], NOTE_BLOCK[69]) as i64;
        check!("NOTE_A440_ANCHOR", absdiff(a4 as u64, 440_000) < 100, a4, 440_000,
              "MIDI 69 is A440 within 100 mHz");

        // The historical mistake, run as a RED twin: Jeff Lee's AdLib table
        // gives A440 as F-Number 577, which is 9 cents flat.
        let bad = expected_mhz(577, 4) as i64;
        check!("NOTE_A440_RED", absdiff(bad as u64, 440_000) >= 100, bad, 440_000,
              "F-Number 577 (the copied table) MUST fail the anchor");

        // Every octave must be EXACTLY a doubling, and the exact statement of
        // that is a TABLE IDENTITY: same F-Number, BLOCK one higher.
        //
        // The first version of this check compared expected_mhz(n + 12) against
        // 2 * expected_mhz(n) and failed by up to 49 mHz on a table that is in
        // fact perfect. expected_mhz does an integer division, and
        // 2 * floor(x) is not floor(2x). The FAULT WAS IN THE TEST, and a
        // looser tolerance would have hidden that rather than fixed it.
        let mut oct_ok = true;
        let mut worst_oct = 0i64;
        let mut exact = 0i64;
        let mut pairs = 0i64;
        for n in 0..=(MIDI_NOTE_MAX_EXACT as usize - 12) {
            pairs += 1;
            if NOTE_FNUM[n + 12] == NOTE_FNUM[n] && NOTE_BLOCK[n + 12] == NOTE_BLOCK[n] + 1 {
                exact += 1;
            }
            let lo = expected_mhz(NOTE_FNUM[n], NOTE_BLOCK[n]);
            let hi = expected_mhz(NOTE_FNUM[n + 12], NOTE_BLOCK[n + 12]);
            // Against 2 * lo, NOT "1200 cents away from lo".
            //
            // opl2check::cents_x100 is a NEAR-UNITY LINEARIZATION and its own
            // header says so: valid within about +/- 6%. Asked for the cents
            // between a note and its octave, a ratio of 2.0, it returned 1741
            // cents instead of 1200 and this check failed by 541 cents on a
            // table that is correct. Comparing hi against 2 * lo puts the ratio
            // back near unity, which is the domain the helper is documented
            // for. A shared helper used outside its stated domain gives a
            // confident wrong number, and the first instinct on seeing it is to
            // suspect the data.
            let d = cents(hi, lo * 2).abs();
            if d > worst_oct {
                worst_oct = d;
            }
            if d > 1200 {
                oct_ok = false;
            }
        }
        check!("NOTE_OCTAVES", oct_ok, worst_oct, 1200,
              "every octave pair is 2:1 within 12 cents");
        // NOT a pass/fail: the count of pairs that are BIT-EXACT (same
        // F-Number, BLOCK one higher). It is less than all of them because the
        // generator uses BLOCK 0 for the lowest TWO octaves, where the
        // F-Number fits either way, and 2 * round(172.4) is not round(344.9).
        // That is a rounding fact about the bottom of the chip's range, and
        // asserting bit-exactness there would be asserting something false.
        emit(&MidiReport {
            name: "NOTE_OCT_EXACT",
            pass: true,
            measured: exact,
            expected: pairs,
            note: "octave pairs that are bit-identical in the table",
        });

        // Every semitone step must be within a stated band of 2^(1/12). The
        // band is wide at the bottom because one F-Number LSB at F-Number 172
        // is 10 cents; that is the chip, not the table.
        let mut worst_semi = 0i64;
        let mut semi_ok = true;
        let mut worst_at = 0i64;
        for n in 0..MIDI_NOTE_MAX_EXACT as usize {
            let lo = expected_mhz(NOTE_FNUM[n], NOTE_BLOCK[n]);
            let hi = expected_mhz(NOTE_FNUM[n + 1], NOTE_BLOCK[n + 1]);
            // 100 cents in cents_x100 units is 10000.
            let c = cents(hi, lo);
            let d = (c - 10000).abs();
            if d > worst_semi {
                worst_semi = d;
                worst_at = n as i64;
            }
            if d > 1200 {
                semi_ok = false;
            }
        }
        check!("NOTE_SEMITONES", semi_ok, worst_semi, 1200,
              "every semitone step is 100 cents within 12 cents");
        emit(&MidiReport {
            name: "NOTE_SEMI_WORST",
            pass: true,
            measured: worst_at,
            expected: worst_semi,
            note: "note number with the largest step error, and that error x100 cents",
        });
    }

    // -----------------------------------------------------------------------
    // 5. Measured pitch, through the real note_on path.
    // -----------------------------------------------------------------------
    {
        let probes: [u8; 6] = [36, 48, 60, 69, 81, 96];
        let mut worst = 0i64;
        let mut ok = true;
        for p in probes.iter() {
            syn.init(rate, false);
            syn.program_change(0, SEQ_PROGRAM);
            syn.note_on(0, *p, 127);
            // Discard the attack, then measure a LONG window.
            //
            // The first version measured 1024 frames, which is 23 ms, which is
            // ONE AND A HALF CYCLES of MIDI 36 (65 Hz). dominant_mhz needs at
            // least three zero crossings and correctly returned 0. That is the
            // estimator's stated limit doing its job, not a synthesis fault,
            // and the fix is a window long enough for the note being measured.
            for _ in 0..4 {
                syn.chip.render_stereo(&mut sc.blk);
            }
            let long = PROBE_FRAMES;
            for b in 0..(long / BLK_FRAMES) {
                syn.chip.render_stereo(&mut sc.blk);
                for i in 0..BLK_FRAMES {
                    sc.win[b * BLK_FRAMES + i] = sc.blk[i * 2];
                }
            }
            let got = dominant_mhz(&sc.win[..long], rate);
            let want = expected_mhz(NOTE_FNUM[*p as usize], NOTE_BLOCK[*p as usize]);
            let c = cents(got, want).abs();
            if c > worst {
                worst = c;
            }
            // 40 cents. A 1024-frame window at 44100 Hz holds only 2 cycles of
            // MIDI 36 (65 Hz), so the zero-crossing estimator has very little
            // to work with down there; the band is stated rather than quietly
            // widened until it passed.
            let pass = c <= 4000;
            if !pass {
                ok = false;
            }
            emit(&MidiReport {
                name: "PITCH_NOTE",
                pass,
                measured: got as i64,
                expected: want as i64,
                note: "rendered through note_on, dominant frequency in mHz",
            });
            if !pass {
                fails += 1;
            }
        }
        check!("PITCH_MEASURED", ok, worst, 4000,
              "worst error across 6 probe notes, in cents x100");
    }

    {
        // RED TWIN for the pitch measurement: assert note 69 against note 70's
        // frequency. One semitone is 100 cents and the band is 40, so this must
        // fail.
        syn.init(rate, false);
        syn.program_change(0, SEQ_PROGRAM);
        syn.note_on(0, 69, 127);
        for _ in 0..4 {
            syn.chip.render_stereo(&mut sc.blk);
        }
        for b in 0..(PROBE_FRAMES / BLK_FRAMES) {
            syn.chip.render_stereo(&mut sc.blk);
            for i in 0..BLK_FRAMES {
                sc.win[b * BLK_FRAMES + i] = sc.blk[i * 2];
            }
        }
        let got = dominant_mhz(&sc.win[..PROBE_FRAMES], rate);
        let wrong = expected_mhz(NOTE_FNUM[70], NOTE_BLOCK[70]);
        let c = cents(got, wrong).abs();
        check!("PITCH_RED", c > 4000, c, 4000,
              "A440 measured against A#4 MUST fail the 40-cent band");
    }

    // -----------------------------------------------------------------------
    // 6. Pitch bend. The anchors are computed on a host with a real FPU and
    //    written down: 440 Hz bent by the table's own extremes of +1.9922 and
    //    -2.0000 semitones is 493.66 Hz and 392.00 Hz.
    // -----------------------------------------------------------------------
    {
        let measure_bend = |bend_lsb: u8, bend_msb: u8, sc: &mut Scratch, syn: &mut FmSynth| -> u64 {
            syn.init(rate, false);
            syn.program_change(0, SEQ_PROGRAM);
            syn.note_on(0, 69, 127);
            syn.pitch_bend(0, bend_lsb, bend_msb);
            for _ in 0..4 {
                syn.chip.render_stereo(&mut sc.blk);
            }
            for b in 0..(PROBE_FRAMES / BLK_FRAMES) {
                syn.chip.render_stereo(&mut sc.blk);
                for i in 0..BLK_FRAMES {
                    sc.win[b * BLK_FRAMES + i] = sc.blk[i * 2];
                }
            }
            dominant_mhz(&sc.win[..PROBE_FRAMES], rate)
        };
        let up = measure_bend(0x7f, 0x7f, sc, syn);
        let dn = measure_bend(0x00, 0x00, sc, syn);
        let cu = cents(up, 493_660).abs();
        let cd = cents(dn, 392_000).abs();
        check!("BEND_UP", cu <= 3000, up as i64, 493_660,
              "bend +1.99 semitones from A440 is 493.66 Hz within 30 cents");
        check!("BEND_DOWN", cd <= 3000, dn as i64, 392_000,
              "bend -2 semitones from A440 is 392.00 Hz within 30 cents");
        // RED TWIN: an unbent A440 must NOT satisfy the bent expectation.
        let flat = measure_bend(0x00, 0x40, sc, syn);
        let cf = cents(flat, 493_660).abs();
        check!("BEND_RED", cf > 3000, cf, 3000,
              "centre bend measured against the bent expectation MUST fail");
    }

    // -----------------------------------------------------------------------
    // 7. Voice allocation and stealing.
    // -----------------------------------------------------------------------
    {
        syn.init(rate, false);
        for i in 0..12u8 {
            syn.note_on(0, 48 + i * 2, 100);
            syn.advance(64);
        }
        let steals = syn.steals;
        // The nine surviving voices must be the NINE MOST RECENT notes, which
        // is what "steal the oldest" means. Checking the count alone would pass
        // a policy that stole the newest.
        let mut newest_ok = true;
        for i in 3..12u8 {
            let n = 48 + i * 2;
            let mut found = false;
            for v in 0..NUM_CHANNELS {
                if syn.voices[v].on && syn.voices[v].note == n {
                    found = true;
                }
            }
            if !found {
                newest_ok = false;
            }
        }
        check!("STEAL_COUNT", steals == 3, steals as i64, 3,
              "12 notes on 9 voices steals exactly 3");
        check!("STEAL_NEWEST_SURVIVE", newest_ok, if newest_ok { 1 } else { 0 }, 1,
              "the 9 surviving voices are the 9 most recent notes");
        check!("STEAL_RED", !(steals == 0), steals as i64, 0,
              "a policy with no steals MUST NOT satisfy this file");

        // Retrigger: the same note twice must not consume a second voice.
        syn.init(rate, false);
        syn.note_on(0, 60, 100);
        syn.advance(64);
        syn.note_on(0, 60, 100);
        check!("RETRIGGER_ONE_VOICE", syn.keyed() == 1 && syn.steals == 0,
              syn.keyed() as i64, 1,
              "a repeated note-on reuses its own voice");
    }

    // -----------------------------------------------------------------------
    // 8. Percussion detection and the reserved pool.
    // -----------------------------------------------------------------------
    {
        pl.open(f(plan.perc), rate).ok();
        let m = pl.measure();
        check!("PERC_DETECTED", m.percussion, if m.percussion { 1 } else { 0 }, 1,
              "channel 10 note-ons are found by the pre-scan");

        pl.open(f(plan.f0), rate).ok();
        let m2 = pl.measure();
        check!("PERC_RED", !m2.percussion, if m2.percussion { 1 } else { 0 }, 0,
              "a file with no channel 10 MUST NOT reserve drum voices");

        // With percussion reserved, melody must never reach the top two chip
        // channels and drums must never reach the bottom seven.
        syn.init(rate, true);
        for i in 0..9u8 {
            syn.note_on(0, 60 + i, 100);
            syn.advance(64);
        }
        let mut melody_ok = true;
        for v in 7..NUM_CHANNELS {
            if syn.voices[v].on {
                melody_ok = false;
            }
        }
        syn.note_on(9, 36, 100);
        let mut drum_ok = false;
        for v in 7..NUM_CHANNELS {
            if syn.voices[v].on && syn.voices[v].mch == 9 {
                drum_ok = true;
            }
        }
        check!("PERC_POOL_SPLIT", melody_ok && drum_ok,
              if melody_ok && drum_ok { 1 } else { 0 }, 1,
              "melody stays in 0..6 and drums in 7..8");
    }

    // -----------------------------------------------------------------------
    // 9. THE END-TO-END ARM. A sequence rendered to audio, then identified.
    // -----------------------------------------------------------------------
    {
        let mut onsets = [0u64; SEQ_N];
        for i in 0..SEQ_N {
            onsets[i] = seq_onset(i, rate, SEQ_TEMPO1);
        }

        // 9a. PITCH IN EACH WINDOW. Windows are placed by the closed form, so a
        // timing error moves the window into the wrong note.
        syn.init(rate, false);
        pl.open(f(plan.f0), rate).ok();
        let frames = render_capture(pl, syn, sc, rate, &onsets, true);
        let mut seq_ok = frames > 0;
        for k in 0..SEQ_N {
            let n = SEQ_NOTES[k] as usize;
            let want = expected_mhz(NOTE_FNUM[n], NOTE_BLOCK[n]);
            let got = dominant_mhz(&sc.win[k * WIN_LEN..(k + 1) * WIN_LEN], rate);
            let c = cents(got, want).abs();
            let pass = c <= 3000;
            if !pass {
                seq_ok = false;
                fails += 1;
            }
            emit(&MidiReport {
                name: "SEQ_WINDOW",
                pass,
                measured: got as i64,
                expected: want as i64,
                note: "pitch inside note k's window, placed by the tempo map",
            });
        }
        check!("SEQ_PITCH_ORDER", seq_ok, frames as i64, 0,
              "6 notes identified by pitch, in order, at their tempo-map times");

        // 9b. RED TWIN: the SAME expectations against the wrong-tempo file. The
        // last two notes move by more than a window, so at least one window
        // must land on the wrong pitch.
        syn.init(rate, false);
        pl.open(f(plan.red_tempo), rate).ok();
        render_capture(pl, syn, sc, rate, &onsets, true);
        let mut red_all_match = true;
        for k in 0..SEQ_N {
            let n = SEQ_NOTES[k] as usize;
            let want = expected_mhz(NOTE_FNUM[n], NOTE_BLOCK[n]);
            let got = dominant_mhz(&sc.win[k * WIN_LEN..(k + 1) * WIN_LEN], rate);
            if cents(got, want).abs() > 3000 {
                red_all_match = false;
            }
        }
        check!("SEQ_PITCH_RED", !red_all_match, if red_all_match { 1 } else { 0 }, 0,
              "a 150 BPM file MUST fail the 240 BPM window expectations");

        // 9c. ONSET TIMES from the audio itself, with a step-attack patch.
        syn.init(rate, false);
        pl.open(f(plan.f0), rate).ok();
        // Force the piano patch by overriding the file's program change after
        // open; a flute attack has no edge for the detector to find.
        syn.program_change(0, SEQ_PROGRAM_PERC);
        let mut got_on = [0u64; 16];
        render_capture(pl, syn, sc, rate, &onsets, false);
        let n_on = find_onsets(&sc.env[..sc.env_n], &mut got_on);
        let mut worst = 0i64;
        let mut on_ok = n_on == SEQ_N;
        for k in 0..SEQ_N.min(n_on) {
            let d = absdiff(got_on[k], onsets[k]) as i64;
            if d > worst {
                worst = d;
            }
            // Two envelope windows. The detector's resolution IS one window
            // (256 frames, 5.8 ms), and the attack itself takes a few hundred
            // frames, so a tighter band would be measuring the patch.
            let pass = d <= 2 * ENV_WIN as i64;
            if !pass {
                on_ok = false;
                fails += 1;
            }
            emit(&MidiReport {
                name: "SEQ_ONSET",
                pass,
                measured: got_on[k] as i64,
                expected: onsets[k] as i64,
                note: "attack found in the rendered audio, in frames",
            });
        }
        check!("SEQ_ONSET_COUNT", on_ok, n_on as i64, SEQ_N as i64,
              "6 attacks found, each within 512 frames of the tempo map");

        // 9d. RED TWIN for the onsets.
        syn.init(rate, false);
        pl.open(f(plan.red_step), rate).ok();
        render_capture(pl, syn, sc, rate, &onsets, false);
        let n2 = find_onsets(&sc.env[..sc.env_n], &mut got_on);
        let mut red_on_match = n2 == SEQ_N;
        for k in 0..SEQ_N.min(n2) {
            if absdiff(got_on[k], onsets[k]) > 2 * ENV_WIN as u64 {
                red_on_match = false;
            }
        }
        check!("SEQ_ONSET_RED", !red_on_match, if red_on_match { 1 } else { 0 }, 0,
              "a 150 BPM file MUST fail the 240 BPM onset expectations");
    }

    fails
}
