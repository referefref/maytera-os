// rustkern/sinkown.rs - SINGLE-OWNER CLAIM over the ONE hardware audio output
// stream, so a second writer into the DMA ring cannot be constructed.
//
// New kernel logic, no C twin to strangle, so Rust per the 2026-07-16 rule.
// Same shape as fbown.rs / fetchown.rs / blkstage.rs: the INPUTS come from
// unchanged C (drivers/audio.c's stream table) and the OWNERSHIP DECISION
// lives here.
//
// ===========================================================================
// THE DEFECT THIS REMOVES, AND WHY A COMMENT WOULD NOT HAVE
// ---------------------------------------------------------------------------
// drivers/audio.c hands out MAX_STREAMS software streams and every one of them
// is backed by the SAME hardware output stream, the same BDL ring and the same
// free-running wr_slot. There is no mixing at that layer. Two live streams
// therefore interleave chunks into one ring and each reprogram the stream
// format under the other.
//
// MEASURED, 2026-08-26, VM <vmid> (golden byte copy, QEMU -audiodev wav): the
// 18.6 s boot chime was still playing when a DOS guest's /APPS/FMSYNTH opened
// its stream. From that instant hda_avail() returned 0 to the PCM pump for as
// long as it was asked, because the chime held every slot the arithmetic would
// hand out, and the pump's wait had no bound. That is the owner's report: a
// wedged FM synthesiser holding the only PCM stream while everything else on
// the machine was refused EBUSY.
//
// #205 fixed the cause by giving the PCM mixer sole responsibility for the
// hardware: audio_play_file(), audio_play_buffer() and now audio_decode_play()
// all push into the mixer instead of opening their own stream. That left the
// invariant TRUE BUT UNENFORCED, which is the worst of both states - it reads
// as guaranteed while nothing stops the next author writing an innocent
// audio_open()/audio_write() pair in a new file. A comment asking people not
// to is not a mechanism.
//
// ===========================================================================
// THE RULE
// ---------------------------------------------------------------------------
//   CLAIM    audio_open() takes the claim BEFORE it marks a stream slot
//            active. An unclaimed sink admits exactly one caller; every later
//            caller is REFUSED with no stream at all, so it has nothing to
//            write through. This is what makes the second writer impossible
//            rather than merely detected: a refused opener holds no handle.
//   VERIFY   every operation that touches the hardware (start/stop/drain/
//            write/avail) re-checks ownership against the token it was handed,
//            so a pointer kept across a close cannot reach the ring either.
//   RELEASE  audio_close() gives it back. Idempotent, and only the owner can
//            do it, so one client's teardown cannot hand the sink to nobody
//            while another still believes it holds it.
//
// The token is the caller's own audio_stream_t address, which is stable for
// the life of the claim (drivers/audio.c's `streams[]` is a static array) and
// unique among live streams. Nothing here dereferences it; it is an identity,
// not a pointer, which is exactly why this module can be memory-safe about a
// value C hands it.
//
// COUNTERS ARE PART OF THE POINT. A refusal is reported to /AUDIOLOG.TXT with
// the incumbent's token, because the failure mode this whole ticket exists for
// is an audio fault that recorded nothing anywhere the owner could read.

/// 0 is "unowned". No real audio_stream_t lives at address 0, and the C side
/// refuses a null stream long before it reaches here, so the sentinel cannot
/// collide with a genuine token.
const SINK_UNOWNED: u64 = 0;

static mut SINK_OWNER: u64 = SINK_UNOWNED;
/// Bumped on every successful claim. A caller can hold on to this to tell
/// "still my claim" from "released and re-claimed by someone else in between",
/// which a bare owner comparison cannot express once a token is reused.
static mut SINK_GEN: u32 = 0;
static mut SINK_CLAIMS: u32 = 0;
static mut SINK_REFUSALS: u32 = 0;
static mut SINK_RELEASES: u32 = 0;

/// Take the sink. Returns 1 on success, 0 if somebody else holds it.
///
/// Re-claiming with the SAME token succeeds and is a no-op on the generation:
/// it is not an error for an owner to ask twice, and treating it as one would
/// make every caller carry state it does not need.
#[no_mangle]
pub extern "C" fn sink_claim_rs(token: u64) -> i32 {
    if token == SINK_UNOWNED {
        return 0; // a null stream is never a valid owner
    }
    unsafe {
        let owner = core::ptr::read_volatile(core::ptr::addr_of!(SINK_OWNER));
        if owner == token {
            return 1; // already ours
        }
        if owner != SINK_UNOWNED {
            SINK_REFUSALS = SINK_REFUSALS.wrapping_add(1);
            return 0;
        }
        core::ptr::write_volatile(core::ptr::addr_of_mut!(SINK_OWNER), token);
        SINK_GEN = SINK_GEN.wrapping_add(1);
        SINK_CLAIMS = SINK_CLAIMS.wrapping_add(1);
        1
    }
}

/// Give the sink back. Returns 1 if this token held it, 0 otherwise.
/// Releasing a claim you do not hold is REFUSED rather than ignored: silently
/// letting a non-owner release would let one client's teardown strand another.
#[no_mangle]
pub extern "C" fn sink_release_rs(token: u64) -> i32 {
    if token == SINK_UNOWNED {
        return 0;
    }
    unsafe {
        let owner = core::ptr::read_volatile(core::ptr::addr_of!(SINK_OWNER));
        if owner != token {
            return 0;
        }
        core::ptr::write_volatile(core::ptr::addr_of_mut!(SINK_OWNER), SINK_UNOWNED);
        SINK_RELEASES = SINK_RELEASES.wrapping_add(1);
        1
    }
}

/// Does this token currently hold the sink? One volatile read, no lock, no
/// allocation: safe to call on every write and inside a wait condition.
#[no_mangle]
pub extern "C" fn sink_is_owner_rs(token: u64) -> i32 {
    if token == SINK_UNOWNED {
        return 0;
    }
    unsafe {
        let owner = core::ptr::read_volatile(core::ptr::addr_of!(SINK_OWNER));
        if owner == token { 1 } else { 0 }
    }
}

/// The incumbent's token, so a refusal can NAME who holds it.
#[no_mangle]
pub extern "C" fn sink_owner_rs() -> u64 {
    unsafe { core::ptr::read_volatile(core::ptr::addr_of!(SINK_OWNER)) }
}

/// claims / refusals / releases / generation, for the one-line audio-log
/// summary. A boot with refusals > 0 means somebody tried to build a second
/// writer and was stopped, which is worth seeing rather than inferring.
#[no_mangle]
pub extern "C" fn sink_stats_rs(claims: *mut u32, refusals: *mut u32,
                                releases: *mut u32, gen: *mut u32) {
    unsafe {
        if !claims.is_null()   { *claims = SINK_CLAIMS; }
        if !refusals.is_null() { *refusals = SINK_REFUSALS; }
        if !releases.is_null() { *releases = SINK_RELEASES; }
        if !gen.is_null()      { *gen = SINK_GEN; }
    }
}

// ===========================================================================
// SELF-TEST. Returns a bitmask; 0 = PASS. Reported to /AUDIOLOG.TXT at audio
// init, because an ownership rule that is silently wrong presents as "audio
// stopped working after the first track" and gets blamed on the driver.
//
// It runs against the LIVE statics and restores them, so it tests the shipped
// code rather than a copy of it. Called once, before any stream exists.
// ===========================================================================
#[no_mangle]
pub extern "C" fn sink_selftest_rs() -> u32 {
    let mut mask: u32 = 0;
    let saved_owner = unsafe { core::ptr::read_volatile(core::ptr::addr_of!(SINK_OWNER)) };
    let saved = unsafe { (SINK_GEN, SINK_CLAIMS, SINK_REFUSALS, SINK_RELEASES) };
    unsafe { core::ptr::write_volatile(core::ptr::addr_of_mut!(SINK_OWNER), SINK_UNOWNED) };

    let a: u64 = 0x1000;
    let b: u64 = 0x2000;

    // bit 0: the first claimant gets it, and is the owner.
    if sink_claim_rs(a) != 1 { mask |= 1 << 0; }
    if sink_is_owner_rs(a) != 1 { mask |= 1 << 0; }

    // bit 1: THE WHOLE POINT. A second claimant is REFUSED, and does not
    // become the owner as a side effect.
    if sink_claim_rs(b) != 0 { mask |= 1 << 1; }
    if sink_is_owner_rs(b) != 0 { mask |= 1 << 1; }
    if sink_is_owner_rs(a) != 1 { mask |= 1 << 1; }

    // bit 2: a non-owner cannot RELEASE. Without this, the refused second
    // opener could still strand the first by tidying up after itself.
    if sink_release_rs(b) != 0 { mask |= 1 << 2; }
    if sink_is_owner_rs(a) != 1 { mask |= 1 << 2; }

    // bit 3: the owner can release, and the sink is then free for the next.
    if sink_release_rs(a) != 1 { mask |= 1 << 3; }
    if sink_is_owner_rs(a) != 0 { mask |= 1 << 3; }
    if sink_claim_rs(b) != 1 { mask |= 1 << 3; }
    if sink_release_rs(b) != 1 { mask |= 1 << 3; }

    // bit 4: re-claiming with the same token is idempotent, not an error.
    if sink_claim_rs(a) != 1 { mask |= 1 << 4; }
    if sink_claim_rs(a) != 1 { mask |= 1 << 4; }
    if sink_release_rs(a) != 1 { mask |= 1 << 4; }
    if sink_release_rs(a) != 0 { mask |= 1 << 4; }   // second release: not owner

    // bit 5: the null token is never an owner and can never claim. audio.c
    // refuses a null stream first, but a sentinel that could be claimed would
    // make "unowned" and "owned by nobody's stream" the same state.
    if sink_claim_rs(SINK_UNOWNED) != 0 { mask |= 1 << 5; }
    if sink_is_owner_rs(SINK_UNOWNED) != 0 { mask |= 1 << 5; }
    if sink_owner_rs() != SINK_UNOWNED { mask |= 1 << 5; }

    // bit 6: the generation MOVES on a real claim, so "still my claim" can be
    // told from "released and re-claimed by someone else".
    {
        let g0 = unsafe { SINK_GEN };
        if sink_claim_rs(a) != 1 { mask |= 1 << 6; }
        let g1 = unsafe { SINK_GEN };
        if sink_claim_rs(a) != 1 { mask |= 1 << 6; }
        let g2 = unsafe { SINK_GEN };
        if g1 == g0 { mask |= 1 << 6; }   // a fresh claim must advance it
        if g2 != g1 { mask |= 1 << 6; }   // a repeat claim must not
        sink_release_rs(a);
    }

    unsafe {
        core::ptr::write_volatile(core::ptr::addr_of_mut!(SINK_OWNER), saved_owner);
        SINK_GEN = saved.0;
        SINK_CLAIMS = saved.1;
        SINK_REFUSALS = saved.2;
        SINK_RELEASES = saved.3;
    }
    mask
}
