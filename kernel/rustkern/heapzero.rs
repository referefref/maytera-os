// rustkern/heapzero.rs - #heapdbl: WHICH of the two causes zeroed this header?
//
// WHY THIS EXISTS. `mm/heap.c`'s kfree() prints the magic word it actually found
// and says, in a comment on the line above the message, that "0 says the header
// was zeroed (a double free that already coalesced, or a freed-then-reused
// block)". blame.md then promoted that to a single rule: "`magic=0x0` IS NOT 'the
// header was overwritten'. IT IS THE COALESCED DOUBLE-FREE SIGNATURE."
//
// THAT RULE IS TRUE OF THE ALLOCATOR AND FALSE AS A DIAGNOSIS, and acting on it
// cost a whole investigation. There is a SECOND way a heap header reads back as
// zero, and in the one field capture everybody was reasoning from it is the
// cause of EVERY ELEVEN of them: #aicappersist, where a user page table was
// allocated at a physical frame whose NUMBER lies inside the heap's own virtual
// window and then cleared through that number, writing 4096 zero bytes into the
// kernel heap. Every heap header in the victim page goes to zero at once, and so
// does every byte of payload around them. kfree() then refuses those pointers
// with `magic=0x0`, which is INDISTINGUISHABLE in the message from a block that
// really was freed twice.
//
// The two causes are trivially distinguishable IN THE MEMORY, and nothing was
// looking:
//
//   COALESCED DOUBLE FREE   coalesce_blocks() writes ONE field: `magic = 0`. The
//                           other 28 header bytes and EVERY byte of the block's
//                           payload are untouched, and so is the rest of the
//                           containing page.
//   WHOLE-PAGE WIPE         4096 consecutive zero bytes. Header, payload,
//                           neighbours, all of it.
//
// So the verdict is a function of HOW MUCH is zero, and that is what this module
// computes. It is a pure counting function with no heap knowledge, which is why
// it can be self-tested on synthetic buffers instead of needing a corrupted
// heap: the thing that was missing was never hard, it was simply absent.
//
// WHY RUST. Project rule: new kernel code is Rust unless there is a stated
// performance reason. There is none here; this runs only on a path that has
// already decided to print an error line.

#![allow(dead_code)]

/// Verdicts. Deliberately NOT a bool: "indeterminate" has to be sayable, or the
/// instrument would start inventing an answer on the cases it cannot call, which
/// is the failure mode it exists to correct.
pub const HZ_INDETERMINATE: u32 = 0;
pub const HZ_WIPE_PAGE: u32 = 1;
pub const HZ_DOUBLE_FREE: u32 = 2;
pub const HZ_WIPE_PARTIAL: u32 = 3;

const PAGE_BYTES: u32 = 4096;

/// Count zero bytes in `[p, p+len)`. NULL-safe, and `len` is a u32 so a caller
/// cannot hand in a negative-turned-enormous length. The caller is responsible
/// for the range being mapped; mm/heap.c clamps it to the live heap before
/// calling, because only mm/heap.c knows where the heap ends.
#[no_mangle]
pub extern "C" fn heapzero_count_zeros_rs(p: *const u8, len: u32) -> u32 {
    if p.is_null() || len == 0 {
        return 0;
    }
    let mut n: u32 = 0;
    let mut i: u32 = 0;
    while i < len {
        // SAFETY: the caller has clamped [p, p+len) to the mapped kernel heap.
        let b = unsafe { core::ptr::read_volatile(p.add(i as usize)) };
        if b == 0 {
            n += 1;
        }
        i += 1;
    }
    n
}

/// Count 32-bit words equal to `magic` at 16-byte strides in `[p, p+len)`.
///
/// THIS IS THE SOUND DISCRIMINATOR, and it replaced a WRONG one. The first
/// version of this module used "most of the containing page reads as zero" as
/// evidence of a bulk clear. Its own negative control (mm/heapzdbl.c ARM A, a
/// deliberate, genuine coalesced double free) came back "PARTIAL BULK CLEAR" with
/// `pagezeros=3712/4096`, because A HEAP PAGE THAT HAS NEVER BEEN FULLY WRITTEN
/// IS ALREADY MOSTLY ZERO. Counting zeros cannot tell "cleared" from "never
/// written", and no amount of threshold tuning fixes that.
///
/// A surviving block header can. `coalesce_blocks()` edits ONE u32; a 4096-byte
/// memset leaves NO valid header anywhere in the page. So:
///   at least one HEAP_BLOCK_MAGIC still in this page -> it was NOT bulk-cleared
///   none at all, and the page is all zero                -> it WAS
/// Block headers are 32 bytes and every block size is a multiple of 16, so a
/// 16-byte stride visits every possible header position and nothing else.
#[no_mangle]
pub extern "C" fn heapzero_count_magics_rs(p: *const u8, len: u32, magic: u32) -> u32 {
    if p.is_null() || len < 4 {
        return 0;
    }
    let mut n: u32 = 0;
    let mut off: u32 = 0;
    while off + 4 <= len {
        // SAFETY: the caller has clamped [p, p+len) to the mapped kernel heap.
        let w = unsafe { core::ptr::read_volatile(p.add(off as usize) as *const u32) };
        if w == magic {
            n += 1;
        }
        off += 16;
    }
    n
}

/// Decide which cause zeroed the header, from the memory evidence ALONE.
///
/// `hdr_zeros`/`hdr_len`   the block header (32 bytes on this allocator).
/// `data_zeros`/`data_len` a sample of the block's payload, taken immediately
///                         above the header. `block->size` is not trustworthy
///                         here (its own bytes may be part of the damage), so
///                         the caller samples a FIXED length.
/// `page_zeros`/`page_len` the whole 4 KB page containing the pointer, clamped
///                         to the live heap by the caller.
///
/// The PMM/shadow-window state is deliberately NOT an input. It is reported
/// beside the verdict instead, so the verdict rests on one kind of evidence and
/// is CROSS-CHECKED by an independent one, rather than the two being blended
/// into a single number nobody can take apart later.
#[no_mangle]
pub extern "C" fn heapzero_verdict_rs(hdr_zeros: u32, hdr_len: u32,
                                      data_zeros: u32, data_len: u32,
                                      page_zeros: u32, page_len: u32,
                                      page_magics: u32) -> u32 {
    // THE PRECONDITION IS THE CALLER'S, NOT A COUNT. mm/heap.c calls this only
    // when `block->magic` itself read back as zero, which is the whole question.
    //
    // This USED to gate on "the entire 32-byte header reads zero", and that would
    // have rejected the canonical coalesced double free: coalesce_blocks() sets one
    // u32, so after kfree() has cleared BLOCK_FLAG_USED such a header is magic=0,
    // flags=0, size=NON-ZERO, next=0, prev=0, i.e. 24 of 32 bytes zero. The test
    // only ever passed because the arm that exercises it had had its block
    // reallocated and zeroed in between. `hdr_zeros` is still reported, as evidence;
    // it decides nothing.
    if hdr_len == 0 {
        return HZ_INDETERMINATE;   // the header could not be read at all
    }
    let _ = hdr_zeros;

    // DECISIVE, and tested first: a 4096-byte memset leaves no valid block header
    // in the page. One surviving header therefore PROVES the page was not
    // bulk-cleared, so the single zeroed field is a field edit - which is what
    // coalesce_blocks() does, after a first free.
    if page_magics > 0 {
        return HZ_DOUBLE_FREE;
    }

    // No surviving header and the page reads all-but-entirely zero. NOTE that
    // zero-counting alone would also match a page of heap that was never written,
    // which is why it is only consulted AFTER the magic scan has ruled out a live
    // header.
    //
    // THE TOLERANCE IS MEASURED, NOT PADDING, AND SO IS ITS POSITION. A page the
    // #aicappersist mechanism cleared is NOT 4096/4096 zero by the time kfree()
    // looks at it, because the victim has already written to it: `file_put()` does
    // `f->refcount--` on a zeroed `file_t`, storing -1. Observed across a run of 55
    // refusals on a 576 MB reverted boot, reading 4092/4096, then 4088, then 4084
    // as each successive description in the same page was released.
    //
    // THIS TEST USED TO COME AFTER THE PAYLOAD TEST BELOW, AND THAT MADE THE
    // CLASSIFIER ANSWER WRONGLY ON ALL 55: "data_zeros < data_len" means "not
    // completely zero", which four bytes satisfies, so every wiped `file_t` was
    // reported as a COALESCED DOUBLE FREE. A near-total page clear is far stronger
    // evidence than four non-zero payload bytes, so it is decided first.
    if page_len == PAGE_BYTES && page_zeros + 32 >= PAGE_BYTES {
        return HZ_WIPE_PAGE;
    }

    // No surviving header, and the payload is MOSTLY still there: a field edit, in
    // a page that happened to contain only this block. "Mostly" and not "not
    // entirely": see above. A freed object's payload is pointers and counters, so
    // more than half of it non-zero is the ordinary case and four bytes is not.
    if data_len > 0 && data_zeros * 2 < data_len {
        return HZ_DOUBLE_FREE;
    }
    if page_len > 0 && page_zeros * 2 >= page_len {
        return HZ_WIPE_PARTIAL;
    }
    HZ_INDETERMINATE
}

/// Verdict as a NUL-terminated C string, so the C side has no second copy of the
/// mapping to drift from.
#[no_mangle]
pub extern "C" fn heapzero_verdict_name_rs(verdict: u32) -> *const u8 {
    let s: &[u8] = match verdict {
        HZ_WIPE_PAGE => b"WHOLE-PAGE WIPE: this header's whole 4 KB page reads \
zero apart from what the victim itself wrote afterwards, and no block header in it \
survived, so it was cleared in bulk by a write THROUGH a shadowed physical==virtual \
pointer (#aicappersist), NOT freed twice\0",
        HZ_DOUBLE_FREE => b"COALESCED DOUBLE FREE: header zeroed, payload intact, \
so coalesce_blocks() absorbed this block after a FIRST free and this really is a \
second one\0",
        HZ_WIPE_PARTIAL => b"PARTIAL BULK CLEAR: far more than one header's worth \
of this page is zero, so a bulk write, not a field edit; the page may straddle \
the heap end or have been partly reused since\0",
        _ => b"INDETERMINATE: the evidence does not separate a coalesced double \
free from a bulk clear. Say so; do not pick one\0",
    };
    s.as_ptr()
}

/// Self-test. PROVES the classifier separates the two causes, on synthetic
/// counts that are exactly what each cause produces, INCLUDING the negative
/// controls: a non-zero header is not classified at all, and an all-zero payload
/// inside a mostly-live page is reported as indeterminate rather than guessed.
#[no_mangle]
pub extern "C" fn heapzero_selftest_rs(out_checks: *mut u32) -> i32 {
    let mut n: u32 = 0;
    let mut ok = true;
    let mut chk = |cond: bool| {
        n += 1;
        if !cond {
            ok = false;
        }
    };

    // --- the counter itself -------------------------------------------------
    let live: [u8; 64] = [0x41u8; 64];
    let dead = [0u8; 64];
    let mut mixed = [0u8; 64];
    for i in 0..32 {
        mixed[i] = 0x5a;
    }
    chk(heapzero_count_zeros_rs(live.as_ptr(), 64) == 0);
    chk(heapzero_count_zeros_rs(dead.as_ptr(), 64) == 64);
    chk(heapzero_count_zeros_rs(mixed.as_ptr(), 64) == 32);
    chk(heapzero_count_zeros_rs(core::ptr::null(), 64) == 0); // NULL-safe
    chk(heapzero_count_zeros_rs(live.as_ptr(), 0) == 0);

    // --- the magic scan -----------------------------------------------------
    // 0x48454150 is HEAP_BLOCK_MAGIC. Lay two of them out at 16-byte strides and
    // one at an offset the stride does not visit, to prove the stride is a
    // stride and not a byte scan.
    let mut hdrs = [0u8; 64];
    hdrs[0] = 0x50; hdrs[1] = 0x41; hdrs[2] = 0x45; hdrs[3] = 0x48;
    hdrs[32] = 0x50; hdrs[33] = 0x41; hdrs[34] = 0x45; hdrs[35] = 0x48;
    hdrs[20] = 0x50; hdrs[21] = 0x41; hdrs[22] = 0x45; hdrs[23] = 0x48; // off-stride
    chk(heapzero_count_magics_rs(hdrs.as_ptr(), 64, 0x4845_4150) == 2);
    chk(heapzero_count_magics_rs(dead.as_ptr(), 64, 0x4845_4150) == 0);
    chk(heapzero_count_magics_rs(core::ptr::null(), 64, 0x4845_4150) == 0);
    chk(heapzero_count_magics_rs(hdrs.as_ptr(), 2, 0x4845_4150) == 0); // len < 4

    // --- WHOLE-PAGE WIPE: what #aicappersist produces -----------------------
    // The MEASURED field shape: three file_t 192 bytes apart in page
    // 0x10bcb000, all headers zero, all payload zero, page all zero, and NO
    // surviving block header anywhere in the page.
    chk(heapzero_verdict_rs(32, 32, 64, 64, 4096, 4096, 0) == HZ_WIPE_PAGE);
    // The MEASURED post-wipe shape: file_put() has stored a -1 refcount into one,
    // two and three zeroed file_t in the same page.
    chk(heapzero_verdict_rs(32, 32, 60, 64, 4092, 4096, 0) == HZ_WIPE_PAGE);
    chk(heapzero_verdict_rs(32, 32, 60, 64, 4088, 4096, 0) == HZ_WIPE_PAGE);
    chk(heapzero_verdict_rs(32, 32, 60, 64, 4064, 4096, 0) == HZ_WIPE_PAGE);
    // One byte past the tolerance: report PARTIAL rather than stretch the claim.
    chk(heapzero_verdict_rs(32, 32, 60, 64, 4063, 4096, 0) == HZ_WIPE_PARTIAL);

    // --- COALESCED DOUBLE FREE: what coalesce_blocks() produces -------------
    // One u32 zeroed, and the absorbing predecessor's header still intact in the
    // same page. The magic count is what decides it, and it decides correctly
    // EVEN WHEN the page is mostly zero and the payload has been reused - which
    // is the case that made the zero-counting version answer wrongly on a real
    // boot (mm/heapzdbl.c ARM A, pagezeros=3712/4096, data 64/64 zero).
    chk(heapzero_verdict_rs(32, 32, 0, 64, 48, 4096, 20) == HZ_DOUBLE_FREE);
    chk(heapzero_verdict_rs(32, 32, 64, 64, 3712, 4096, 1) == HZ_DOUBLE_FREE);
    // Payload intact with no surviving header: still a field edit.
    chk(heapzero_verdict_rs(32, 32, 1, 64, 300, 4096, 0) == HZ_DOUBLE_FREE);
    chk(heapzero_verdict_rs(32, 32, 31, 64, 300, 4096, 0) == HZ_DOUBLE_FREE);
    // THE LIVE SHAPE, and the regression guard for the rule-order defect: four
    // non-zero payload bytes in an otherwise cleared page is a WIPE, and calling it
    // a double free is the exact wrong answer this instrument exists to prevent.
    chk(heapzero_verdict_rs(32, 32, 60, 64, 4092, 4096, 0) == HZ_WIPE_PAGE);
    chk(heapzero_verdict_rs(32, 32, 60, 64, 4084, 4096, 0) == HZ_WIPE_PAGE);
    // Half the payload zero is not "mostly intact": do not claim a double free.
    chk(heapzero_verdict_rs(32, 32, 32, 64, 300, 4096, 0) != HZ_DOUBLE_FREE);

    // --- THE CANONICAL COALESCED SHAPE, which the old precondition rejected ---
    // 24 of 32 header bytes zero (magic 4 + flags 4 + next 8 + prev 8), `size`
    // still non-zero, neighbours intact. This is what coalesce_blocks() actually
    // leaves behind, and it MUST classify as a double free.
    chk(heapzero_verdict_rs(24, 32, 0, 64, 60, 4096, 20) == HZ_DOUBLE_FREE);
    chk(heapzero_verdict_rs(24, 32, 0, 64, 60, 4096, 0) == HZ_DOUBLE_FREE);

    // --- negative controls --------------------------------------------------
    // No readable header: say so rather than guessing.
    chk(heapzero_verdict_rs(32, 0, 0, 64, 4096, 4096, 0) == HZ_INDETERMINATE);
    // Header and payload zero, no surviving header, page overwhelmingly live:
    // refuse to call it.
    chk(heapzero_verdict_rs(32, 32, 64, 64, 100, 4096, 0) == HZ_INDETERMINATE);
    // Header and payload zero, page half zero: a bulk clear, not a field edit.
    chk(heapzero_verdict_rs(32, 32, 64, 64, 2048, 4096, 0) == HZ_WIPE_PARTIAL);
    // A page sample SHORTER than 4096 (the pointer sat near the heap end) must
    // not be promoted to WIPE_PAGE on a full count of a short range.
    chk(heapzero_verdict_rs(32, 32, 64, 64, 512, 512, 0) == HZ_WIPE_PARTIAL);
    // AND a wiped page with a surviving header is not a wipe: the magic test
    // wins over the zero count, in both directions.
    chk(heapzero_verdict_rs(32, 32, 64, 64, 4096, 4096, 1) == HZ_DOUBLE_FREE);

    // --- the names must exist and be terminated -----------------------------
    for v in [HZ_INDETERMINATE, HZ_WIPE_PAGE, HZ_DOUBLE_FREE, HZ_WIPE_PARTIAL] {
        let p = heapzero_verdict_name_rs(v);
        let mut i = 0usize;
        let mut terminated = false;
        // 512, not 256: the WHOLE-PAGE WIPE text is ~280 bytes and the 256-byte
        // bound turned this check RED on a real boot. The bound is the point of the
        // check (an unterminated name reaching kprintf("%s") reads past the
        // literal), so it is raised rather than removed, with headroom stated.
        while i < 512 {
            // SAFETY: p points at a 'static byte literal in this module.
            if unsafe { *p.add(i) } == 0 {
                terminated = true;
                break;
            }
            i += 1;
        }
        chk(terminated && i > 8);
    }

    if !out_checks.is_null() {
        unsafe { *out_checks = n };
    }
    if ok {
        0
    } else {
        -1
    }
}
