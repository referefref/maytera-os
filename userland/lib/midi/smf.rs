// smf.rs - #183: Standard MIDI File parsing. no_std, no alloc, no float, no
// I/O. The file is a borrowed byte slice and every read is bounds-checked by
// hand, because a Ring-3 `panic = abort` turns one out-of-range index in a
// malformed file into a dead process with no diagnosis.
//
// ===========================================================================
// THE TWO PLACES NAIVE SMF PARSERS BREAK, and both are tested adversarially in
// midiselftest.rs rather than merely handled here.
// ---------------------------------------------------------------------------
// 1. VARIABLE-LENGTH QUANTITIES. Seven bits per byte, high bit set on every
//    byte but the last. The naive loop `v = (v << 7) | (b & 0x7f)` with no
//    limit reads forever on a corrupt file and silently overflows on a
//    hostile one. The spec caps a VLQ at FOUR bytes (0x0FFFFFFF); a fifth
//    continuation byte is a MALFORMED FILE and is reported, not absorbed.
// 2. RUNNING STATUS. A channel-voice status byte persists across events, so a
//    run of note-ons is 2 bytes each after the first. The subtle half is what
//    CLEARS it: any System message does, which means F0 and F7 sysex AND FF
//    meta. A parser that keeps running status across a tempo meta-event will
//    read the next event's first DATA byte as a status byte and desynchronise
//    for the rest of the track, usually producing music that starts correctly
//    and turns to noise a few bars in. That is SMF_RUN_AFTER_META in the
//    self-test, and it is required to be reported as an error rather than
//    guessed at.
//
// WHAT IS DELIBERATELY NOT HERE: no event is interpreted. This module turns
// bytes into (tick, event) and nothing else. The tempo map, the merge across
// tracks and the conversion to sample positions live in player.rs, and the
// General MIDI interpretation lives in gm2opl.rs.

// ---------------------------------------------------------------------------
// Errors. Negative, distinct, and every one of them is reachable from a file a
// user could plausibly hand us.
// ---------------------------------------------------------------------------
pub const E_OK: i32 = 0;
/// The file is shorter than the structure it declares.
pub const E_SHORT: i32 = -1;
/// No "MThd" at offset 0.
pub const E_MAGIC: i32 = -2;
/// Header format field is not 0, 1 or 2.
pub const E_FORMAT: i32 = -3;
/// Division is SMPTE (bit 15 set). Timecode-based division is not implemented.
pub const E_DIVISION: i32 = -4;
/// A variable-length quantity ran past four bytes.
pub const E_VLQ: i32 = -5;
/// A data byte appeared where a status byte was required and no running status
/// was in effect. This is the running-status desynchronisation fault.
pub const E_STATUS: i32 = -6;
/// The header declares zero tracks, or no MTrk chunk was found.
pub const E_TRACKS: i32 = -7;
/// An MTrk chunk declares a length that runs past the end of the file.
pub const E_TRUNC: i32 = -8;
/// The header declares a division of zero, which makes every tick infinite.
pub const E_PPQ: i32 = -9;

pub fn err_name(e: i32) -> &'static str {
    match e {
        E_OK => "ok",
        E_SHORT => "file shorter than declared",
        E_MAGIC => "not a MIDI file (no MThd)",
        E_FORMAT => "unknown SMF format",
        E_DIVISION => "SMPTE division not supported",
        E_VLQ => "malformed variable-length quantity",
        E_STATUS => "data byte where a status byte was required",
        E_TRACKS => "no tracks",
        E_TRUNC => "truncated track chunk",
        E_PPQ => "division is zero",
        _ => "unknown error",
    }
}

// ---------------------------------------------------------------------------
// Event kinds. Small integers rather than an enum with payloads, so an Event is
// Copy, fixed size, and storable in a `[Event; N]` with no allocator.
// ---------------------------------------------------------------------------
pub const EV_NONE: u8 = 0;
pub const EV_NOTE_OFF: u8 = 1;
pub const EV_NOTE_ON: u8 = 2;
pub const EV_CC: u8 = 3;
pub const EV_PROGRAM: u8 = 4;
pub const EV_PITCH: u8 = 5;
pub const EV_TEMPO: u8 = 6;
pub const EV_END: u8 = 7;
/// Parsed correctly, length consumed correctly, deliberately not acted on
/// (aftertouch, sysex, time signature, key signature, track names).
pub const EV_OTHER: u8 = 8;

#[derive(Clone, Copy)]
pub struct Event {
    pub kind: u8,
    /// MIDI channel 0..15 for channel-voice events, 0 otherwise.
    pub ch: u8,
    /// note / controller / program number.
    pub a: u8,
    /// velocity / controller value.
    pub b: u8,
    /// microseconds per quarter note, EV_TEMPO only.
    pub tempo: u32,
}

impl Event {
    pub const fn none() -> Event {
        Event { kind: EV_NONE, ch: 0, a: 0, b: 0, tempo: 0 }
    }
}

// ---------------------------------------------------------------------------
// One track's cursor.
// ---------------------------------------------------------------------------
#[derive(Clone, Copy)]
pub struct Track<'a> {
    data: &'a [u8],
    pos: usize,
    /// Absolute tick of `ev`. Valid only while `live` is true.
    pub tick: u64,
    /// Running status byte, 0 when none is in effect.
    run: u8,
    /// True while `ev` holds an undelivered event.
    pub live: bool,
    pub ev: Event,
    /// First error seen on this track. The track stops at it; the rest of the
    /// file still plays, and the player reports the count.
    pub err: i32,
}

impl<'a> Track<'a> {
    pub const fn empty() -> Track<'a> {
        Track { data: &[], pos: 0, tick: 0, run: 0, live: false, ev: Event::none(), err: E_OK }
    }

    pub fn is_empty(&self) -> bool {
        self.data.is_empty()
    }

    /// The track's byte range. Needed by Player::rewind, which rebuilds a
    /// cursor over the same bytes rather than re-parsing the whole file.
    pub fn data_slice(&self) -> &'a [u8] {
        self.data
    }

    pub fn set_data(&mut self, d: &'a [u8]) {
        self.data = d;
    }

    fn byte(&mut self) -> Option<u8> {
        let b = *self.data.get(self.pos)?;
        self.pos += 1;
        Some(b)
    }

    fn peek(&self) -> Option<u8> {
        self.data.get(self.pos).copied()
    }

    /// A variable-length quantity, four bytes maximum.
    fn vlq(&mut self) -> Result<u32, i32> {
        let mut v: u32 = 0;
        let mut n = 0;
        loop {
            let b = match self.byte() {
                Some(b) => b,
                None => return Err(E_SHORT),
            };
            v = (v << 7) | (b & 0x7f) as u32;
            n += 1;
            if b & 0x80 == 0 {
                return Ok(v);
            }
            // FOUR bytes is the whole legal range. The fifth continuation byte
            // is the check the naive loop leaves out, and without it a corrupt
            // file walks the parser off the end of the track (or, with a bounds
            // check but no limit, silently wraps a u32).
            if n >= 4 {
                return Err(E_VLQ);
            }
        }
    }

    fn skip(&mut self, n: usize) -> Result<(), i32> {
        match self.pos.checked_add(n) {
            Some(p) if p <= self.data.len() => {
                self.pos = p;
                Ok(())
            }
            _ => Err(E_SHORT),
        }
    }

    /// Decode the next event into `self.ev`, setting `self.tick` to its
    /// absolute tick. Returns false at end of track or on the first error.
    pub fn step(&mut self) -> bool {
        self.live = false;
        if self.err != E_OK || self.pos >= self.data.len() {
            return false;
        }
        match self.step_inner() {
            Ok(true) => {
                self.live = true;
                true
            }
            Ok(false) => false,
            Err(e) => {
                self.err = e;
                false
            }
        }
    }

    fn step_inner(&mut self) -> Result<bool, i32> {
        let dt = self.vlq()?;
        self.tick = self.tick.wrapping_add(dt as u64);

        let mut st = match self.peek() {
            Some(b) => b,
            None => return Err(E_SHORT),
        };

        if st >= 0x80 {
            self.pos += 1;
            if st < 0xf0 {
                // A channel-voice status byte ARMS running status.
                self.run = st;
            } else {
                // EVERY System message clears it: F0/F7 sysex, FF meta, and the
                // system-common bytes below. This is the line whose absence
                // desynchronises a parser after the first tempo change.
                self.run = 0;
            }
        } else {
            // A data byte. Legal only under running status.
            if self.run == 0 {
                return Err(E_STATUS);
            }
            st = self.run;
        }

        let kind = st & 0xf0;
        let ch = st & 0x0f;

        // Channel voice messages.
        if st < 0xf0 {
            let d1 = self.byte().ok_or(E_SHORT)?;
            // A data byte must have bit 7 clear. If it does not, the stream is
            // desynchronised and continuing would produce plausible nonsense.
            if d1 >= 0x80 {
                return Err(E_STATUS);
            }
            let two = matches!(kind, 0x80 | 0x90 | 0xa0 | 0xb0 | 0xe0);
            let d2 = if two {
                let b = self.byte().ok_or(E_SHORT)?;
                if b >= 0x80 {
                    return Err(E_STATUS);
                }
                b
            } else {
                0
            };
            self.ev = match kind {
                // Note-on with velocity 0 IS a note-off. Files rely on this,
                // because it is what makes running status worth having.
                0x90 if d2 == 0 => Event { kind: EV_NOTE_OFF, ch, a: d1, b: 0, tempo: 0 },
                0x90 => Event { kind: EV_NOTE_ON, ch, a: d1, b: d2, tempo: 0 },
                0x80 => Event { kind: EV_NOTE_OFF, ch, a: d1, b: d2, tempo: 0 },
                0xb0 => Event { kind: EV_CC, ch, a: d1, b: d2, tempo: 0 },
                0xc0 => Event { kind: EV_PROGRAM, ch, a: d1, b: 0, tempo: 0 },
                0xe0 => Event { kind: EV_PITCH, ch, a: d1, b: d2, tempo: 0 },
                // 0xa0 poly aftertouch, 0xd0 channel aftertouch: consumed with
                // the right length, not acted on.
                _ => Event { kind: EV_OTHER, ch, a: d1, b: d2, tempo: 0 },
            };
            return Ok(true);
        }

        // Meta events.
        if st == 0xff {
            let ty = self.byte().ok_or(E_SHORT)?;
            let len = self.vlq()? as usize;
            let start = self.pos;
            self.skip(len)?;
            if ty == 0x2f {
                self.ev = Event { kind: EV_END, ch: 0, a: 0, b: 0, tempo: 0 };
                return Ok(true);
            }
            if ty == 0x51 && len == 3 {
                let t = (self.data[start] as u32) << 16
                    | (self.data[start + 1] as u32) << 8
                    | (self.data[start + 2] as u32);
                self.ev = Event { kind: EV_TEMPO, ch: 0, a: 0, b: 0, tempo: t };
                return Ok(true);
            }
            self.ev = Event { kind: EV_OTHER, ch: 0, a: ty, b: 0, tempo: 0 };
            return Ok(true);
        }

        // Sysex, both forms, skipped by their declared length.
        if st == 0xf0 || st == 0xf7 {
            let len = self.vlq()? as usize;
            self.skip(len)?;
            self.ev = Event { kind: EV_OTHER, ch: 0, a: st, b: 0, tempo: 0 };
            return Ok(true);
        }

        // System common. Not legal inside an SMF track, but files in the wild
        // contain them, so consume the right number of bytes rather than
        // desynchronising. F4 and F5 are undefined and have no length, so they
        // are the one case that must stop.
        let n = match st {
            0xf1 | 0xf3 => 1,
            0xf2 => 2,
            0xf6 | 0xf8 | 0xfa | 0xfb | 0xfc | 0xfe => 0,
            _ => return Err(E_STATUS),
        };
        self.skip(n)?;
        self.ev = Event { kind: EV_OTHER, ch: 0, a: st, b: 0, tempo: 0 };
        Ok(true)
    }
}

// ---------------------------------------------------------------------------
// Header.
// ---------------------------------------------------------------------------
pub struct SmfHeader {
    /// 0 = one multi-channel track, 1 = simultaneous tracks, 2 = independent
    /// sequences.
    pub format: u16,
    /// Track count the header DECLARES.
    pub ntrks: u16,
    /// Ticks per quarter note. SMPTE division is rejected at open.
    pub ppq: u16,
    /// Tracks actually parsed into the caller's array, which is min(ntrks,
    /// chunks present, array capacity).
    pub tracks_used: usize,
    /// Tracks present in the file that did not fit in the caller's array.
    pub tracks_dropped: usize,
}

fn be16(d: &[u8], i: usize) -> Option<u16> {
    Some((*d.get(i)? as u16) << 8 | *d.get(i + 1)? as u16)
}

fn be32(d: &[u8], i: usize) -> Option<u32> {
    Some((*d.get(i)? as u32) << 24
        | (*d.get(i + 1)? as u32) << 16
        | (*d.get(i + 2)? as u32) << 8
        | *d.get(i + 3)? as u32)
}

/// Parse a Standard MIDI File and fill `tracks` with one cursor per MTrk.
///
/// Chunks whose type is neither MThd nor MTrk are SKIPPED BY THEIR DECLARED
/// LENGTH, which the spec requires of any reader; several sequencers emit
/// proprietary chunks and a reader that stops at the first unknown one refuses
/// files that are perfectly valid.
pub fn smf_open<'a>(d: &'a [u8], tracks: &mut [Track<'a>]) -> Result<SmfHeader, i32> {
    if d.len() < 14 {
        return Err(E_SHORT);
    }
    if &d[0..4] != b"MThd" {
        return Err(E_MAGIC);
    }
    let hlen = be32(d, 4).ok_or(E_SHORT)? as usize;
    if hlen < 6 {
        return Err(E_SHORT);
    }
    let format = be16(d, 8).ok_or(E_SHORT)?;
    let ntrks = be16(d, 10).ok_or(E_SHORT)?;
    let division = be16(d, 12).ok_or(E_SHORT)?;
    if format > 2 {
        return Err(E_FORMAT);
    }
    // Bit 15 set means the division is SMPTE frames plus subframes, a genuinely
    // different timing model. Rejected rather than half-implemented.
    if division & 0x8000 != 0 {
        return Err(E_DIVISION);
    }
    if division == 0 {
        return Err(E_PPQ);
    }
    if ntrks == 0 {
        return Err(E_TRACKS);
    }

    let mut pos = 8usize.checked_add(hlen).ok_or(E_SHORT)?;
    let mut used = 0usize;
    let mut dropped = 0usize;

    while pos + 8 <= d.len() {
        let ty = &d[pos..pos + 4];
        let len = be32(d, pos + 4).ok_or(E_SHORT)? as usize;
        let body = pos + 8;
        let end = body.checked_add(len).ok_or(E_TRUNC)?;
        if end > d.len() {
            // A track that claims more bytes than the file holds. Reported, not
            // silently clipped: a clipped track plays a truncated song and
            // looks like a playback bug.
            return Err(E_TRUNC);
        }
        if ty == b"MTrk" {
            if used < tracks.len() {
                let mut t = Track::empty();
                t.data = &d[body..end];
                t.step();
                tracks[used] = t;
                used += 1;
            } else {
                dropped += 1;
            }
        }
        pos = end;
    }

    if used == 0 {
        return Err(E_TRACKS);
    }

    Ok(SmfHeader { format, ntrks, ppq: division, tracks_used: used, tracks_dropped: dropped })
}
