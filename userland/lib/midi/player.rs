// player.rs - #183: the tempo map, the multi-track merge, and sample-accurate
// scheduling of MIDI events against rendered audio.
//
// ===========================================================================
// THERE IS NO CLOCK IN THIS FILE, AND THAT IS THE DESIGN
// ---------------------------------------------------------------------------
// The obvious way to play a MIDI file is a loop that sleeps until the next
// event is due. On MayteraOS that is a trap with a name: `timer_ticks` is NOT a
// wall clock, because KVM replays lost ticks in BURSTS, so any `deadline =
// timer_ticks + N` can fire instantly under vCPU starvation (see blame.md and
// docs). A player built that way plays at the wrong speed under load and looks
// exactly like a synthesis bug, which is the worst possible way for a timing
// fault to present.
//
// So nothing here waits for anything. EVENT POSITIONS ARE SAMPLE POSITIONS.
// The tempo map converts ticks to samples, `render_block` renders exactly up to
// the next event, applies it, and continues. The only thing that paces
// playback is the audio sink accepting the buffer, which is the one clock in
// the system that is definitionally correct for audio: if the sink has taken
// 44100 frames, one second of music has been produced, whatever the scheduler
// was doing. `SYS_MONO_US` is used in the app for CPU-cost measurement and for
// nothing else.
//
// A consequence worth stating: the timing tests in midiselftest.rs need no VM,
// no audio device and no timer. They assert on sample indices in a buffer, and
// a sample index is exact.
//
// ===========================================================================
// THE TICK-TO-SAMPLE CONVERSION IS EXACT, NOT ROUNDED PER EVENT
// ---------------------------------------------------------------------------
// samples = dt * us_per_quarter * rate / (ppq * 1000000). Computed as a
// quotient and a REMAINDER that carries across events, so the error never
// accumulates: after a thousand events the position is the exact rounded value,
// not a thousand roundings. The remainder survives tempo changes, because a
// tempo change alters the numerator, not the denominator.

/// Ticks-to-samples with a carried remainder.
#[derive(Clone, Copy)]
pub struct TickClock {
    q: u64,
    r: u64,
    d: u64,
    rem: u64,
    pub us_per_qn: u32,
    rate: u32,
    ppq: u16,
}

impl TickClock {
    pub const fn new() -> TickClock {
        TickClock { q: 0, r: 0, d: 1, rem: 0, us_per_qn: 500_000, rate: 44100, ppq: 96 }
    }

    pub fn init(&mut self, rate: u32, ppq: u16) {
        self.rate = rate;
        self.ppq = if ppq == 0 { 1 } else { ppq };
        self.d = self.ppq as u64 * 1_000_000;
        self.rem = 0;
        // 500000 us per quarter note is 120 BPM, the value every SMF is
        // assumed to start at when it carries no tempo meta-event.
        self.set_tempo(500_000);
    }

    pub fn set_tempo(&mut self, us: u32) {
        // A zero or absurd tempo would divide by zero or stop the song dead.
        // Clamp to the 24-bit field's usable range: 1 us per quarter note is
        // nonsense but finite, and the 0xFFFFFF ceiling is the field's own.
        let us = us.clamp(1, 0x00FF_FFFF);
        self.us_per_qn = us;
        let n = us as u64 * self.rate as u64;
        self.q = n / self.d;
        self.r = n % self.d;
    }

    /// Beats per minute, rounded, for display only.
    pub fn bpm(&self) -> u32 {
        (60_000_000 + self.us_per_qn as u64 / 2) as u32 / self.us_per_qn
    }

    /// Samples in `dt` ticks at the current tempo, exact with carry.
    ///
    /// OVERFLOW BOUND, stated because it is not obvious: `dt` comes from a
    /// variable-length quantity and so is at most 0x0FFFFFFF, and `r` is less
    /// than `d` which is at most 32767 * 1000000. Their product is 8.8e18,
    /// inside u64's 1.8e19. `saturating_mul` is there for the case this
    /// reasoning is wrong rather than as a substitute for it.
    pub fn advance(&mut self, dt: u64) -> u64 {
        let whole = dt.saturating_mul(self.q);
        let frac = dt.saturating_mul(self.r).saturating_add(self.rem);
        self.rem = frac % self.d;
        whole + frac / self.d
    }
}

pub const MAX_TRACKS: usize = 64;

/// How long to keep rendering after the last event, so releases finish instead
/// of being cut off by the file ending. Bounded, because a patch with RR=0
/// releases over forty seconds and a player that waits for true silence would
/// appear to hang at the end of every song.
const TAIL_FRAMES_DIV: u64 = 2; // rate / 2 = half a second

pub struct Player<'a> {
    pub hdr: SmfHeader,
    tracks: [Track<'a>; MAX_TRACKS],
    ntrk: usize,
    pub clock: TickClock,
    /// Tick of the most recently converted position.
    tick: u64,
    /// Sample position of `tick`.
    at_sample: u64,
    /// Absolute sample position of the next pending event.
    next_sample: u64,
    /// Frames rendered so far. THE position readout.
    pub cur: u64,
    /// True once every track has ended.
    pub ended: bool,
    /// True once the tail has also been rendered.
    pub finished: bool,
    tail_left: u64,
    rate: u32,
    /// Tracks that stopped early on a parse error.
    pub track_errors: u32,
    pub last_error: i32,
    pub events: u64,
    /// Format 2 files: tracks after the first are NOT played. Counted here so
    /// the app can say so rather than silently playing a third of the file.
    pub format2_ignored: usize,
}

impl<'a> Player<'a> {
    pub const fn new() -> Player<'a> {
        Player {
            hdr: SmfHeader { format: 0, ntrks: 0, ppq: 96, tracks_used: 0, tracks_dropped: 0 },
            tracks: [Track::empty(); MAX_TRACKS],
            ntrk: 0,
            clock: TickClock::new(),
            tick: 0,
            at_sample: 0,
            next_sample: 0,
            cur: 0,
            ended: true,
            finished: true,
            tail_left: 0,
            rate: 44100,
            track_errors: 0,
            last_error: E_OK,
            events: 0,
            format2_ignored: 0,
        }
    }

    /// Parse `d` and arm playback at sample zero.
    pub fn open(&mut self, d: &'a [u8], rate: u32) -> Result<(), i32> {
        self.tracks = [Track::empty(); MAX_TRACKS];
        let hdr = smf_open(d, &mut self.tracks)?;
        self.ntrk = hdr.tracks_used;
        self.format2_ignored = 0;
        // FORMAT 2 is a set of INDEPENDENT sequences, not simultaneous tracks.
        // Playing them together, which is what the format 0/1 merge below does,
        // would overlay unrelated pieces of music. Playing only the first is the
        // conventional reader behaviour and it is REPORTED, not silent.
        if hdr.format == 2 && self.ntrk > 1 {
            self.format2_ignored = self.ntrk - 1;
            self.ntrk = 1;
        }
        self.hdr = hdr;
        self.rate = rate;
        self.clock.init(rate, self.hdr.ppq);
        self.tick = 0;
        self.at_sample = 0;
        self.cur = 0;
        self.track_errors = 0;
        self.last_error = E_OK;
        self.events = 0;
        self.ended = false;
        self.finished = false;
        self.tail_left = rate as u64 / TAIL_FRAMES_DIV;
        self.resolve_next();
        Ok(())
    }

    /// Return to sample zero without re-parsing the header.
    pub fn rewind(&mut self) {
        for i in 0..self.ntrk {
            let d = self.tracks[i].data_slice();
            self.tracks[i] = Track::empty();
            self.tracks[i].set_data(d);
            self.tracks[i].step();
        }
        self.clock.init(self.rate, self.hdr.ppq);
        self.tick = 0;
        self.at_sample = 0;
        self.cur = 0;
        self.track_errors = 0;
        self.events = 0;
        self.ended = false;
        self.finished = false;
        self.tail_left = self.rate as u64 / TAIL_FRAMES_DIV;
        self.resolve_next();
    }

    /// The live track with the smallest tick. Ties go to the LOWEST TRACK
    /// INDEX, so the merge is deterministic and two runs of the same file
    /// produce byte-identical audio.
    fn earliest(&self) -> Option<usize> {
        let mut best = usize::MAX;
        let mut best_tick = u64::MAX;
        for i in 0..self.ntrk {
            if self.tracks[i].live && self.tracks[i].tick < best_tick {
                best_tick = self.tracks[i].tick;
                best = i;
            }
        }
        if best == usize::MAX { None } else { Some(best) }
    }

    /// Convert the next pending event's tick into an absolute sample position.
    fn resolve_next(&mut self) {
        match self.earliest() {
            Some(i) => {
                let t = self.tracks[i].tick;
                let dt = t.saturating_sub(self.tick);
                self.at_sample += self.clock.advance(dt);
                self.tick = t;
                self.next_sample = self.at_sample;
            }
            None => {
                for i in 0..self.ntrk {
                    if self.tracks[i].err != E_OK {
                        self.track_errors += 1;
                        self.last_error = self.tracks[i].err;
                    }
                }
                self.ended = true;
            }
        }
    }

    /// Pop the earliest event and hand it to `synth`. `synth` may be None,
    /// which is how `measure` walks the file without making a sound.
    fn pop(&mut self, synth: Option<&mut FmSynth>) {
        let i = match self.earliest() {
            Some(i) => i,
            None => return,
        };
        let ev = self.tracks[i].ev;
        self.tracks[i].step();
        self.events += 1;
        match ev.kind {
            EV_TEMPO => self.clock.set_tempo(ev.tempo),
            _ => {
                if let Some(s) = synth {
                    match ev.kind {
                        EV_NOTE_ON => s.note_on(ev.ch, ev.a, ev.b),
                        EV_NOTE_OFF => s.note_off(ev.ch, ev.a),
                        EV_CC => s.control_change(ev.ch, ev.a, ev.b),
                        EV_PROGRAM => s.program_change(ev.ch, ev.a),
                        EV_PITCH => s.pitch_bend(ev.ch, ev.a, ev.b),
                        _ => {}
                    }
                }
            }
        }
        self.resolve_next();
    }

    /// Render `out` (interleaved stereo) and apply every event that falls
    /// inside it AT ITS EXACT SAMPLE POSITION. Returns frames rendered, which
    /// is always out.len()/2 until the song and its tail are done.
    pub fn render_block(&mut self, synth: &mut FmSynth, out: &mut [i16]) -> usize {
        let frames = out.len() / 2;
        let mut done = 0usize;
        while done < frames {
            if !self.ended {
                // Every event whose position has arrived, before rendering one
                // more sample. A `while` rather than an `if`, because any
                // number of events can share a tick and a chord is exactly
                // that case.
                let mut guard = 0u32;
                while !self.ended && self.next_sample <= self.cur {
                    self.pop(Some(&mut *synth));
                    guard += 1;
                    // A file whose tracks all sit on one tick forever cannot
                    // wedge the render loop. 100k events at one instant is
                    // already pathological; stopping is better than hanging,
                    // and the count is visible as `events`.
                    if guard > 100_000 {
                        self.ended = true;
                    }
                }
                if self.ended {
                    continue;
                }
                let want = (self.next_sample - self.cur).min((frames - done) as u64) as usize;
                if want == 0 {
                    continue;
                }
                let s = done * 2;
                let e = s + want * 2;
                synth.chip.render_stereo(&mut out[s..e]);
                synth.advance(want as u64);
                self.cur += want as u64;
                done += want;
            } else {
                if self.tail_left == 0 {
                    self.finished = true;
                    // Silence the remainder rather than leaving stale samples
                    // in the caller's buffer, which would be an audible click
                    // on the last block of every song.
                    for x in out[done * 2..].iter_mut() {
                        *x = 0;
                    }
                    return done;
                }
                let want = ((frames - done) as u64).min(self.tail_left) as usize;
                let s = done * 2;
                let e = s + want * 2;
                synth.chip.render_stereo(&mut out[s..e]);
                synth.advance(want as u64);
                self.cur += want as u64;
                self.tail_left -= want as u64;
                done += want;
                // A tail that has gone silent early is over. This is the one
                // place the chip's own silence detector is the right question,
                // because it is about the WHOLE chip and nothing is keyed.
                if synth.chip.is_silent() {
                    self.tail_left = 0;
                }
            }
        }
        done
    }

    /// Pop the next event and return it with its absolute sample position.
    /// Playback does not use this; the self-test and `measure` do, so that the
    /// event stream two arms compare is produced by the SAME code path that
    /// playback uses.
    pub fn step_event(&mut self) -> Option<(u64, Event)> {
        let i = self.earliest()?;
        let ev = self.tracks[i].ev;
        let s = self.next_sample;
        self.pop(None);
        Some((s, ev))
    }

    /// Walk the whole file without synthesising, then rewind.
    ///
    /// This exists so the app can show a total duration and a progress bar, and
    /// so the percussion reservation is decided from the file rather than
    /// guessed. It uses THE SAME parser and THE SAME tempo map as playback, so
    /// the total it reports and the position playback reaches cannot disagree.
    pub fn measure(&mut self) -> Measure {
        let mut m = Measure { total_frames: 0, events: 0, percussion: false,
                              notes: 0, channels: 0, track_errors: 0, last_error: E_OK };
        while let Some((_, ev)) = self.step_event() {
            if ev.kind == EV_NOTE_ON && ev.b > 0 {
                m.notes += 1;
                m.channels |= 1u16 << (ev.ch & 0x0f);
                if ev.ch & 0x0f == 9 {
                    m.percussion = true;
                }
            }
            m.events = self.events;
            if m.events > 4_000_000 {
                break;
            }
        }
        m.total_frames = self.at_sample;
        m.track_errors = self.track_errors;
        m.last_error = self.last_error;
        self.rewind();
        m
    }
}

#[derive(Clone, Copy)]
pub struct Measure {
    /// Total length in frames, from the tempo map. Excludes the release tail.
    pub total_frames: u64,
    pub events: u64,
    pub notes: u64,
    /// True if any note-on lands on MIDI channel 10 (index 9).
    pub percussion: bool,
    /// Bit per MIDI channel that carries at least one note.
    pub channels: u16,
    pub track_errors: u32,
    pub last_error: i32,
}
