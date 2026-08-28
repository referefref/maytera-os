// gm2opl.rs - #183: General MIDI interpreted onto a YM3812, and an honest
// account of everything that interpretation loses.
//
// ===========================================================================
// THE MAPPING IS LOSSY BY CONSTRUCTION. THIS IS THE LIST.
// ---------------------------------------------------------------------------
// General MIDI assumes 16 channels, 128 melodic instruments, a 47-instrument
// percussion kit on channel 10, and unlimited simultaneous notes. An OPL2 has
// NINE two-operator voices, one waveform-select bit per operator, no noise
// generator in this core, and no stereo. Every one of the following is a
// DECISION, taken here rather than left to emerge:
//
//   16 channels -> 9 voices   Voices are a POOL, not one per MIDI channel.
//                             Allocation and stealing are documented on
//                             `alloc_voice` below and counted in `steals`.
//   128 programs -> 20 patches
//                             GM_PATCH maps all 128 program numbers onto 20
//                             hand-authored two-operator patches, mostly one
//                             per GM family of eight. A trumpet and a tuba get
//                             the same patch. This is stated in the app's UI,
//                             not hidden.
//   Percussion                The core implements no rhythm mode and has no
//                             noise generator (userland/lib/opl2 says so
//                             explicitly and says why). Channel 10 is mapped to
//                             PITCHED APPROXIMATIONS: a low thump for kicks, a
//                             short bright burst for snares, a very short high
//                             metallic tick for hats and cymbals. A hi-hat made
//                             of a sine is not a hi-hat. It is audible, it is
//                             rhythmically correct, and it is not the sound.
//   Pan (CC10)                DROPPED. The YM3812 is mono. render_stereo
//                             duplicates the mono signal.
//   Pitch-bend range (RPN 0)  NOT IMPLEMENTED. Every channel bends the GM
//                             default of +/- 2 semitones. A file that sets a
//                             wider range bends too little.
//   Aftertouch                DROPPED (parsed, consumed, ignored).
//   Modulation (CC1)          Reduced to a BINARY vibrato switch at CC1 > 63,
//                             because the chip's vibrato is one bit per
//                             operator with a fixed depth, not a continuous
//                             amount.
//   Notes above MIDI 114      The chip cannot express them; see
//                             MIDI_NOTE_MAX_EXACT. They are CLAMPED to the top
//                             of block 7 and counted in `clamped`, rather than
//                             wrapping to a wrong octave.
//
// ===========================================================================
// THE PATCHES ARE AUTHORED, NOT SOURCED
// ---------------------------------------------------------------------------
// Every byte in PATCHES below was chosen here from the operator model described
// in docs/OPL2_FM_CORE.md: a modulator/carrier frequency ratio from MULT, a
// modulation index from the modulator's Total Level, an envelope from AR/DR/
// SL/RR and a harmonic bias from the waveform bit. They are NOT taken from
// GENMIDI.OP2, from DOSBox, from an AdLib driver, or from any other bank; those
// are third-party data with licences attached and importing one would need an
// ATTRIBUTION.md entry, which #61 and #87 are the record of getting wrong.
// Nothing here needs an attribution entry, and that is a claim that has to stay
// true.

// midi_tables.rs is include!d by midi.rs into this same module, so NOTE_FNUM,
// NOTE_BLOCK, VEL_ATT, BEND_RATIO and MIDI_NOTE_MAX_EXACT are in scope here.

/// A two-operator patch: the eleven register bytes that define a voice, plus a
/// transpose. Laid out as raw register values rather than as decoded fields,
/// because raw values are what `write_reg` takes and decoding them into fields
/// and back is a second place to get the bit positions wrong.
#[derive(Clone, Copy)]
pub struct Patch {
    /// 0x20 + op: AM | VIB | EGT | KSR | MULT
    pub m_av: u8,
    /// 0x40 + op: KSL | TL
    pub m_ktl: u8,
    /// 0x60 + op: AR | DR
    pub m_ad: u8,
    /// 0x80 + op: SL | RR
    pub m_sr: u8,
    /// 0xE0 + op: waveform select
    pub m_ws: u8,
    pub c_av: u8,
    pub c_ktl: u8,
    pub c_ad: u8,
    pub c_sr: u8,
    pub c_ws: u8,
    /// 0xC0 + ch: FB | CNT. CNT = 1 is additive, and then BOTH operators are
    /// audible, which is why `apply_level` looks at this bit.
    pub fb_cnt: u8,
    /// Semitone transpose applied before the note is looked up.
    pub xpose: i8,
}

const fn p(m_av: u8, m_ktl: u8, m_ad: u8, m_sr: u8, m_ws: u8,
           c_av: u8, c_ktl: u8, c_ad: u8, c_sr: u8, c_ws: u8,
           fb_cnt: u8, xpose: i8) -> Patch {
    Patch { m_av, m_ktl, m_ad, m_sr, m_ws, c_av, c_ktl, c_ad, c_sr, c_ws, fb_cnt, xpose }
}

/// Patch slot names, for the app's UI. Index-parallel with PATCHES.
pub static PATCH_NAMES: [&str; 20] = [
    "Piano", "Bell", "Organ", "Guitar", "Bass", "Strings", "Choir", "Brass",
    "Reed", "Pipe", "Lead", "Pad", "FX", "Ethnic", "Perc", "Noise",
    "Kick", "Snare", "Hat", "Tom",
];

/// The twenty patches. Slots 16..19 are the percussion approximations and are
/// never reachable from a program change; only `perc_map` selects them.
pub static PATCHES: [Patch; 20] = [
    // 0 Piano: FM, 1:1 ratio, percussive envelope, moderate feedback for bite.
    p(0x01, 0x14, 0xF4, 0x26, 0x00,  0x01, 0x00, 0xF5, 0x37, 0x00,  0x0C, 0),
    // 1 Bell / chromatic percussion: inharmonic 1:7 ratio, long ringing decay.
    p(0x07, 0x0C, 0xF6, 0x48, 0x00,  0x01, 0x00, 0xF6, 0x59, 0x00,  0x00, 0),
    // 2 Organ: additive (CNT=1), 1:2, no decay, drawbar-ish.
    p(0x02, 0x0B, 0xF0, 0x06, 0x00,  0x21, 0x00, 0xF0, 0x06, 0x00,  0x01, 0),
    // 3 Guitar: plucked, strong feedback, quick decay to nothing.
    p(0x01, 0x0F, 0xF5, 0x37, 0x00,  0x01, 0x00, 0xF6, 0x48, 0x00,  0x0E, 0),
    // 4 Bass: 1:1 with a firm decay and a low sustain, feedback for growl.
    p(0x01, 0x0D, 0xF6, 0x48, 0x00,  0x01, 0x00, 0xF7, 0x59, 0x00,  0x0A, 0),
    // 5 Strings: sustaining (EGT=1), slow attack, vibrato on both operators.
    p(0x61, 0x11, 0x83, 0x15, 0x00,  0x61, 0x00, 0x74, 0x25, 0x00,  0x06, 0),
    // 6 Choir / ensemble: sustaining, very soft attack, half-sine carrier.
    p(0x61, 0x16, 0x62, 0x14, 0x00,  0x61, 0x00, 0x63, 0x24, 0x01,  0x04, 0),
    // 7 Brass: sustaining with a bright attack peak, feedback for the rasp.
    p(0x21, 0x0E, 0xC2, 0x16, 0x00,  0x21, 0x00, 0xB3, 0x26, 0x00,  0x0C, 0),
    // 8 Reed: sustaining, half-sine modulator gives the odd-harmonic reediness.
    p(0x21, 0x10, 0xD3, 0x25, 0x01,  0x21, 0x00, 0xC3, 0x35, 0x00,  0x08, 0),
    // 9 Pipe / flute: nearly pure carrier, modulator almost silent, vibrato.
    p(0x21, 0x2A, 0xA2, 0x27, 0x00,  0x21, 0x00, 0xA2, 0x37, 0x00,  0x00, 0),
    // 10 Synth lead: sustaining, maximum feedback, pulse-sine carrier.
    p(0x21, 0x08, 0xF2, 0x15, 0x00,  0x21, 0x00, 0xF2, 0x25, 0x03,  0x0E, 0),
    // 11 Synth pad: very slow attack and release, sustaining.
    p(0x61, 0x18, 0x52, 0x13, 0x00,  0x61, 0x00, 0x43, 0x13, 0x00,  0x04, 0),
    // 12 Synth effects: deliberately inharmonic 3:5, long tail.
    p(0x03, 0x0A, 0x84, 0x27, 0x02,  0x05, 0x00, 0x75, 0x38, 0x02,  0x0E, 0),
    // 13 Ethnic / plucked: short, bright, 1:2.
    p(0x02, 0x0E, 0xF7, 0x59, 0x00,  0x01, 0x00, 0xF8, 0x6A, 0x00,  0x0A, 0),
    // 14 Percussive (tuned): very fast decay, no sustain at all.
    p(0x01, 0x0C, 0xF9, 0x7B, 0x00,  0x01, 0x00, 0xFA, 0x8C, 0x00,  0x08, 0),
    // 15 Sound effects: high MULT on both, harsh, medium tail.
    p(0x0E, 0x06, 0xF4, 0x37, 0x02,  0x0F, 0x00, 0xF5, 0x48, 0x03,  0x0E, 0),
    // ---- percussion approximations. See perc_map and the honesty list above.
    // 16 Kick: low, 1:1, instant attack, immediate decay, no sustain.
    p(0x01, 0x08, 0xFA, 0x8F, 0x00,  0x01, 0x00, 0xFC, 0xAF, 0x00,  0x0A, 0),
    // 17 Snare: mid, inharmonic 1:5, short bright burst.
    p(0x05, 0x06, 0xFB, 0x9F, 0x02,  0x01, 0x00, 0xFC, 0xAF, 0x03,  0x0E, 0),
    // 18 Hat / cymbal: very high MULT both sides, extremely short.
    p(0x0F, 0x04, 0xFD, 0xBF, 0x02,  0x0E, 0x00, 0xFD, 0xCF, 0x03,  0x0E, 0),
    // 19 Tom: pitched thump, slightly longer than the kick.
    p(0x01, 0x0A, 0xF9, 0x7E, 0x00,  0x01, 0x00, 0xFA, 0x9E, 0x00,  0x0C, 0),
];

/// Program number to patch slot. Mostly one slot per GM family of eight, with
/// the handful of overrides that are worth having because the family patch is
/// badly wrong for them.
pub static GM_PATCH: [u8; 128] = [
    // 0-7 piano
    0, 0, 0, 0, 0, 0, 0, 0,
    // 8-15 chromatic percussion
    1, 1, 1, 1, 1, 1, 1, 1,
    // 16-23 organ
    2, 2, 2, 2, 2, 2, 2, 8,      // 23 tango accordion sounds better as a reed
    // 24-31 guitar
    3, 3, 3, 3, 3, 3, 3, 12,     // 31 guitar harmonics: an effect, not a guitar
    // 32-39 bass
    4, 4, 4, 4, 4, 4, 4, 4,
    // 40-47 strings
    5, 5, 5, 5, 5, 5, 13, 13,    // 46 harp, 47 timpani are plucked/struck
    // 48-55 ensemble
    6, 6, 6, 6, 6, 6, 6, 15,     // 55 orchestra hit
    // 56-63 brass
    7, 7, 7, 7, 7, 7, 7, 7,
    // 64-71 reed
    8, 8, 8, 8, 8, 8, 8, 8,
    // 72-79 pipe
    9, 9, 9, 9, 9, 9, 9, 9,
    // 80-87 synth lead
    10, 10, 10, 10, 10, 10, 10, 10,
    // 88-95 synth pad
    11, 11, 11, 11, 11, 11, 11, 11,
    // 96-103 synth effects
    12, 12, 12, 12, 12, 12, 12, 12,
    // 104-111 ethnic
    13, 13, 13, 13, 13, 13, 13, 13,
    // 112-119 percussive
    14, 14, 14, 14, 14, 14, 14, 14,
    // 120-127 sound effects
    15, 15, 15, 15, 15, 15, 15, 15,
];

/// GM percussion key to (patch slot, the note actually played).
///
/// The note is FORCED because a percussion key number is an instrument
/// identifier, not a pitch: GM key 42 means "closed hi-hat", and playing note
/// 42 (F#1, 92 Hz) would produce a low buzz. The chosen pitches place each
/// group where its approximation is least wrong.
pub fn perc_map(key: u8) -> (usize, u8) {
    match key {
        // Bass drums and the low end of the kit.
        35 | 36 => (16, 24),
        41 | 43 => (19, 33),
        45 | 47 => (19, 38),
        48 | 50 => (19, 43),
        // Snares, rim, claps, and the mid burst family.
        37 => (17, 62),
        38 | 40 => (17, 58),
        39 => (17, 64),
        // Hi-hats: closed, pedal, open.
        42 => (18, 90),
        44 => (18, 88),
        46 => (18, 86),
        // Cymbals, ride, splash, china.
        49 | 51 | 52 | 53 | 55 | 57 | 59 => (18, 84),
        // Toms not covered above.
        54 | 56 | 58 => (18, 80),
        // Everything else in the kit: a short mid tick, audible and neutral.
        _ => (14, 70),
    }
}

// ---------------------------------------------------------------------------
// Voice pool.
// ---------------------------------------------------------------------------
#[derive(Clone, Copy)]
pub struct Voice {
    /// MIDI channel that owns this voice while `on` or releasing.
    pub mch: u8,
    /// MIDI note number as WRITTEN IN THE FILE (percussion keys included), for
    /// display. The pitch actually played may differ; see perc_map.
    pub note: u8,
    /// Note actually sounding, after transpose and percussion remapping.
    pub play_note: u8,
    pub on: bool,
    /// True between a note-off and the sustain pedal being released.
    pub held: bool,
    /// Sample position of the last key-on. The stealing order.
    pub started: u64,
    /// Sample position of the last key-off, or u64::MAX while keyed.
    pub released: u64,
    /// Patch slot currently programmed into this chip channel, or 255.
    pub patch: u8,
    fnum: u16,
    block: u8,
}

impl Voice {
    const fn new() -> Voice {
        Voice { mch: 0, note: 0, play_note: 0, on: false, held: false,
                started: 0, released: 0, patch: 255, fnum: 0, block: 0 }
    }
}

#[derive(Clone, Copy)]
struct ChState {
    program: u8,
    volume: u8,
    expression: u8,
    /// 14-bit pitch bend, 8192 is centre.
    bend: u16,
    sustain: bool,
    vibrato: bool,
}

impl ChState {
    const fn new() -> ChState {
        // GM defaults: volume 100, expression 127, bend centred.
        ChState { program: 0, volume: 100, expression: 127, bend: 8192,
                  sustain: false, vibrato: false }
    }
}

/// How long after a key-off a voice counts as FREE rather than merely
/// preferred. 1/8 second is longer than the release of every patch above whose
/// RR is 6 or more, and it is a fixed number rather than a query because the
/// core reports silence for the WHOLE CHIP, not per channel.
const RELEASE_GRACE_DIV: u64 = 8;

pub struct FmSynth {
    pub chip: Opl2,
    pub voices: [Voice; NUM_CHANNELS],
    ch: [ChState; 16],
    rate: u32,
    now: u64,
    /// First OPL channel reserved for percussion, or NUM_CHANNELS for none.
    perc_base: usize,
    // ---- counters, all of which the app displays, because they ARE the
    // statement of what the mapping lost.
    pub notes: u64,
    pub steals: u64,
    pub clamped: u64,
    pub dropped: u64,
}

impl FmSynth {
    pub const fn new() -> FmSynth {
        FmSynth {
            chip: Opl2::new(),
            voices: [Voice::new(); NUM_CHANNELS],
            ch: [ChState::new(); 16],
            rate: 44100,
            now: 0,
            perc_base: NUM_CHANNELS,
            notes: 0,
            steals: 0,
            clamped: 0,
            dropped: 0,
        }
    }

    /// `percussion` reserves the top two chip channels for channel 10. The
    /// player decides this by SCANNING THE FILE for a channel-10 note-on before
    /// playback starts, so a file with no drums keeps all nine melodic voices
    /// and a file with drums never has its whole kit stolen by a string pad.
    pub fn init(&mut self, rate: u32, percussion: bool) {
        self.chip.init(rate);
        self.rate = rate;
        self.now = 0;
        self.voices = [Voice::new(); NUM_CHANNELS];
        self.ch = [ChState::new(); 16];
        self.perc_base = if percussion { NUM_CHANNELS - 2 } else { NUM_CHANNELS };
        self.notes = 0;
        self.steals = 0;
        self.clamped = 0;
        self.dropped = 0;
        self.chip.write_reg(0x01, 0x20); // waveform select enable
        self.chip.write_reg(0x08, 0x00); // NTS clear
        self.chip.write_reg(0xBD, 0x00); // no rhythm mode, shallow AM/VIB
        for c in 0..NUM_CHANNELS {
            self.chip.write_reg(0xB0 + c as u8, 0x00);
        }
    }

    pub fn perc_reserved(&self) -> bool {
        self.perc_base < NUM_CHANNELS
    }

    /// Advance the synth's idea of now by `frames`. The player calls this after
    /// each render so that "oldest voice" is measured in SAMPLES, which is the
    /// same clock the events are scheduled on, and never in wall-clock time.
    pub fn advance(&mut self, frames: u64) {
        self.now += frames;
    }

    pub fn now(&self) -> u64 {
        self.now
    }

    fn program_patch(&mut self, v: usize, slot: usize) {
        if self.voices[v].patch as usize == slot {
            return;
        }
        let pa = PATCHES[slot];
        let m = op_offset(v as u8);
        let c = m + 3;
        self.chip.write_reg(0x20 + m, pa.m_av);
        self.chip.write_reg(0x40 + m, pa.m_ktl);
        self.chip.write_reg(0x60 + m, pa.m_ad);
        self.chip.write_reg(0x80 + m, pa.m_sr);
        self.chip.write_reg(0xE0 + m, pa.m_ws);
        self.chip.write_reg(0x20 + c, pa.c_av);
        self.chip.write_reg(0x40 + c, pa.c_ktl);
        self.chip.write_reg(0x60 + c, pa.c_ad);
        self.chip.write_reg(0x80 + c, pa.c_sr);
        self.chip.write_reg(0xE0 + c, pa.c_ws);
        self.chip.write_reg(0xC0 + v as u8, pa.fb_cnt);
        self.voices[v].patch = slot as u8;
    }

    /// Total Level for the audible operator(s).
    ///
    /// Velocity, CC7 volume and CC11 expression are three ATTENUATIONS IN dB
    /// and they add, which is what multiplying three linear gains means. The
    /// patch's own TL is the fourth term and it is what makes one patch quieter
    /// than another at the same velocity.
    fn apply_level(&mut self, v: usize, vel: u8) {
        let slot = self.voices[v].patch as usize;
        if slot >= PATCHES.len() {
            return;
        }
        let pa = PATCHES[slot];
        let mch = self.voices[v].mch as usize;
        let att = VEL_ATT[(vel & 0x7f) as usize] as u32
            + VEL_ATT[(self.ch[mch].volume & 0x7f) as usize] as u32
            + VEL_ATT[(self.ch[mch].expression & 0x7f) as usize] as u32;
        let m = op_offset(v as u8);
        let c = m + 3;
        let ctl = ((pa.c_ktl & 0x3f) as u32 + att).min(63) as u8;
        self.chip.write_reg(0x40 + c, (pa.c_ktl & 0xc0) | ctl);
        // CNT = 1 is additive and BOTH operators reach the output, so the
        // modulator is a second audible voice and must be attenuated too.
        // Getting this wrong makes every organ note ignore its velocity.
        if pa.fb_cnt & 0x01 != 0 {
            let mtl = ((pa.m_ktl & 0x3f) as u32 + att).min(63) as u8;
            self.chip.write_reg(0x40 + m, (pa.m_ktl & 0xc0) | mtl);
        }
    }

    /// F-Number and BLOCK for a note on a channel, including its pitch bend.
    ///
    /// The bend is a 16.16 ratio applied to the F-Number. If the result exceeds
    /// the 10-bit field, the BLOCK is raised and the F-Number halved, which is
    /// the same pitch; without that a bend near the top of a block wraps to a
    /// wrong note instead of bending.
    fn pitch_of(&self, mch: usize, note: u8) -> (u16, u8, bool) {
        let n = note.min(127) as usize;
        let clamped = note > MIDI_NOTE_MAX_EXACT;
        let mut fnum = NOTE_FNUM[n] as u32;
        let mut block = NOTE_BLOCK[n];
        let bend = self.ch[mch].bend;
        if bend != 8192 {
            let r = BEND_RATIO[(bend >> 5) as usize] as u64;
            fnum = ((fnum as u64 * r) >> 16) as u32;
            while fnum > 1023 && block < 7 {
                fnum >>= 1;
                block += 1;
            }
            if fnum > 1023 {
                fnum = 1023;
            }
            if fnum == 0 {
                fnum = 1;
            }
        }
        (fnum as u16, block, clamped)
    }

    fn write_pitch(&mut self, v: usize, keyed: bool) {
        let fnum = self.voices[v].fnum;
        let block = self.voices[v].block;
        self.chip.write_reg(0xA0 + v as u8, (fnum & 0xff) as u8);
        let hi = ((fnum >> 8) & 0x03) as u8 | (block << 2) | if keyed { 0x20 } else { 0x00 };
        self.chip.write_reg(0xB0 + v as u8, hi);
    }

    /// Choose a chip channel for a new note. THE STEALING POLICY, stated:
    ///
    ///   1. The same MIDI channel and note already sounding: RETRIGGER that
    ///      voice. A file that sends two note-ons for one note without a
    ///      note-off between (common, and legal) must not consume two voices.
    ///   2. A voice that has never been used.
    ///   3. A voice whose key has been off for at least an eighth of a second,
    ///      by which time every patch here is inaudible.
    ///   4. The voice released LONGEST ago. Still fading, but the quietest
    ///      thing available.
    ///   5. Otherwise the OLDEST SOUNDING voice, by key-on sample position.
    ///      This is a `steal` and it is counted. Oldest-first rather than
    ///      quietest-first because computing per-voice loudness would need
    ///      per-channel envelope state the core does not expose, and because
    ///      stealing the oldest is what a listener expects: the note that has
    ///      been ringing longest is the one they have stopped attending to.
    ///
    /// Percussion, when reserved, allocates only within the top two channels
    /// and melody only within the bottom seven, so a busy melody can never
    /// silence the drums and a fill can never silence the melody.
    fn alloc_voice(&mut self, mch: u8, note: u8, perc: bool) -> usize {
        let (lo, hi) = if !self.perc_reserved() {
            (0, NUM_CHANNELS)
        } else if perc {
            (self.perc_base, NUM_CHANNELS)
        } else {
            (0, self.perc_base)
        };

        // 1. retrigger
        for v in lo..hi {
            if self.voices[v].on && self.voices[v].mch == mch && self.voices[v].note == note {
                return v;
            }
        }
        // 2. never used
        for v in lo..hi {
            if self.voices[v].patch == 255 {
                return v;
            }
        }
        let grace = (self.rate as u64) / RELEASE_GRACE_DIV;
        // 3. released long enough to be silent
        for v in lo..hi {
            if !self.voices[v].on && !self.voices[v].held
                && self.now.saturating_sub(self.voices[v].released) >= grace
            {
                return v;
            }
        }
        // 4. released longest ago
        let mut best = usize::MAX;
        let mut best_rel = u64::MAX;
        for v in lo..hi {
            if !self.voices[v].on && !self.voices[v].held && self.voices[v].released < best_rel {
                best_rel = self.voices[v].released;
                best = v;
            }
        }
        if best != usize::MAX {
            return best;
        }
        // 5. steal the oldest sounding
        let mut oldest = lo;
        let mut oldest_start = u64::MAX;
        for v in lo..hi {
            if self.voices[v].started < oldest_start {
                oldest_start = self.voices[v].started;
                oldest = v;
            }
        }
        self.steals += 1;
        oldest
    }

    pub fn note_on(&mut self, mch: u8, note: u8, vel: u8) {
        if vel == 0 {
            self.note_off(mch, note);
            return;
        }
        let mch = (mch & 0x0f) as usize;
        let perc = mch == 9;
        let (slot, mut play) = if perc {
            perc_map(note)
        } else {
            let s = GM_PATCH[self.ch[mch].program as usize] as usize;
            let x = PATCHES[s].xpose as i32 + note as i32;
            (s, x.clamp(0, 127) as u8)
        };
        if play > MIDI_NOTE_MAX_EXACT {
            self.clamped += 1;
            play = play.min(127);
        }
        let v = self.alloc_voice(mch as u8, note, perc);

        // Key OFF before reprogramming. Changing an operator's registers under
        // a keyed voice does not restart the envelope, so the new note would
        // inherit the old one's envelope phase and, on a stolen voice, its
        // decay tail. This one write is the difference between a clean
        // retrigger and notes that fade in.
        if self.voices[v].on {
            self.write_pitch(v, false);
        }
        self.program_patch(v, slot);
        self.voices[v].mch = mch as u8;
        self.voices[v].note = note;
        self.voices[v].play_note = play;
        self.voices[v].on = true;
        self.voices[v].held = false;
        self.voices[v].started = self.now;
        self.voices[v].released = u64::MAX;
        let (fnum, block, _) = self.pitch_of(mch, play);
        self.voices[v].fnum = fnum;
        self.voices[v].block = block;
        self.apply_level(v, vel);
        self.write_pitch(v, true);
        self.notes += 1;
    }

    pub fn note_off(&mut self, mch: u8, note: u8) {
        let mch = mch & 0x0f;
        for v in 0..NUM_CHANNELS {
            if self.voices[v].on && self.voices[v].mch == mch && self.voices[v].note == note {
                if self.ch[mch as usize].sustain {
                    // The pedal is down. The note stops being a note and starts
                    // being a held one; it is released when the pedal is.
                    self.voices[v].held = true;
                    self.voices[v].on = false;
                } else {
                    self.voices[v].on = false;
                    self.voices[v].held = false;
                    self.voices[v].released = self.now;
                    self.write_pitch(v, false);
                }
            }
        }
    }

    pub fn program_change(&mut self, mch: u8, prog: u8) {
        self.ch[(mch & 0x0f) as usize].program = prog & 0x7f;
    }

    pub fn pitch_bend(&mut self, mch: u8, lsb: u8, msb: u8) {
        let mch = (mch & 0x0f) as usize;
        self.ch[mch].bend = ((msb as u16 & 0x7f) << 7) | (lsb as u16 & 0x7f);
        for v in 0..NUM_CHANNELS {
            if self.voices[v].mch as usize == mch && (self.voices[v].on || self.voices[v].held) {
                let play = self.voices[v].play_note;
                let (fnum, block, _) = self.pitch_of(mch, play);
                self.voices[v].fnum = fnum;
                self.voices[v].block = block;
                self.write_pitch(v, self.voices[v].on || self.voices[v].held);
            }
        }
    }

    pub fn control_change(&mut self, mch: u8, cc: u8, val: u8) {
        let m = (mch & 0x0f) as usize;
        match cc {
            1 => {
                // The chip's vibrato is one bit with a fixed depth, so a
                // continuous modulation wheel becomes a switch. Stated in the
                // lossy list at the top of this file.
                let on = val >= 64;
                if on != self.ch[m].vibrato {
                    self.ch[m].vibrato = on;
                    for v in 0..NUM_CHANNELS {
                        if self.voices[v].mch as usize == m && self.voices[v].patch != 255 {
                            let pa = PATCHES[self.voices[v].patch as usize];
                            let mo = op_offset(v as u8);
                            // OR, not replace: a patch that asks for vibrato
                            // (strings, choir, pad) keeps it when the file
                            // sends CC1 = 0, and a patch that does not gains it
                            // when the file asks. Replacing would silently
                            // strip the vibrato out of every string patch in
                            // any file that touches CC1.
                            let bit = if on { 0x40 } else { 0x00 };
                            self.chip.write_reg(0x20 + mo, pa.m_av | bit);
                            self.chip.write_reg(0x20 + mo + 3, pa.c_av | bit);
                        }
                    }
                }
            }
            7 | 11 => {
                if cc == 7 {
                    self.ch[m].volume = val & 0x7f;
                } else {
                    self.ch[m].expression = val & 0x7f;
                }
                // Re-level the sounding voices, so a volume swell or a fade-out
                // written as CC11 actually swells. A player that only reads
                // volume at note-on plays every fade at full level.
                for v in 0..NUM_CHANNELS {
                    if self.voices[v].mch as usize == m && self.voices[v].on {
                        // Velocity is folded into the level already written, so
                        // re-derive from full velocity and let the two channel
                        // controls do the work. The velocity component is
                        // therefore lost on a re-level; accepted, because
                        // storing per-voice velocity to re-multiply it is
                        // state that would then need to survive a steal.
                        self.apply_level(v, 127);
                    }
                }
            }
            64 => {
                let down = val >= 64;
                self.ch[m].sustain = down;
                if !down {
                    for v in 0..NUM_CHANNELS {
                        if self.voices[v].mch as usize == m && self.voices[v].held {
                            self.voices[v].held = false;
                            self.voices[v].released = self.now;
                            self.write_pitch(v, false);
                        }
                    }
                }
            }
            120 | 123 => self.all_off_channel(m as u8),
            121 => {
                // Reset all controllers. The PROGRAM is not a controller and
                // must survive, which is why this is not a bare assignment.
                let prog = self.ch[m].program;
                self.ch[m] = ChState::new();
                self.ch[m].program = prog;
            }
            _ => {}
        }
    }

    pub fn all_off_channel(&mut self, mch: u8) {
        for v in 0..NUM_CHANNELS {
            if self.voices[v].mch == mch && (self.voices[v].on || self.voices[v].held) {
                self.voices[v].on = false;
                self.voices[v].held = false;
                self.voices[v].released = self.now;
                self.write_pitch(v, false);
            }
        }
    }

    pub fn all_off(&mut self) {
        for m in 0..16 {
            self.all_off_channel(m);
        }
    }

    /// Voices currently keyed. The chip's own `active_voices()` counts
    /// envelopes that are still fading, which is a different and also useful
    /// number; the app shows both.
    pub fn keyed(&self) -> u32 {
        let mut n = 0;
        for v in 0..NUM_CHANNELS {
            if self.voices[v].on {
                n += 1;
            }
        }
        n
    }
}
