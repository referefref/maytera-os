// rustkern/dosmcb.rs - #172: the DOS guest's conventional-memory answer to
// "how much is free?", and INT 21h AH=26h Create New PSP.
//
// New kernel logic with no C twin to strangle, so Rust per the 2026-07-16 rule.
// Both routines are load-time bookkeeping over at most DOS_MAX_MCB (512)
// records or one 256-byte copy; neither is on a per-frame path and neither
// touches float, so there is no measured performance reason for C.
//
// ===========================================================================
// WHY THIS EXISTS: Stunts, #172
// ---------------------------------------------------------------------------
// MEASURED, golden b1978, /DOS/STUNTS/LOAD.EXE, one run: the guest prints
// "Loading Stunts...Not enough memory to load program." and exits 1 after
// 11,100 instructions. The last thing it asks the kernel is
//
//     [int21:dos] AH=48 al=00 bx=ffff ... cs:ip=0525:1c06
//     [dos] 48h alloc req=ffff FAIL avail=0002 top=9ffe
//
// which is the standard MS C `_dos_allocmem(0xFFFF)` probe for "largest free
// block". It was told TWO PARAGRAPHS. The truth at that moment was a free run
// of 0x7651 paragraphs (475,216 bytes) sitting between the program's own block
// (0x0100 + 0x1425) and the block it had just placed high (0x8b76 + 0x1488).
//
// The old answer was `0xA000 - alloc_top_para`, i.e. only the space ABOVE the
// bump pointer. That is right until the guest performs the classic DOS
// allocate-high dance, which LOAD.EXE does (its own string for it is
// "HIGHLOAD"):
//
//     probe   = largest free                     ; 48h BX=FFFF, expected to fail
//     filler  = alloc(probe - (want + 2))        ; take everything but the top
//     block   = alloc(want)                      ; so THIS lands at the top
//     free(filler)                               ; hand the low part back
//
// After that, `block` pins the bump pointer at the ceiling, `0xA000 - top` is
// 2, and every later "how much is free?" answers 32 bytes forever, while a
// 464 KB hole sits directly below. The ALLOCATOR already coped - INT 21h 48h
// falls back to dos_mcb_first_fit() and would have satisfied the request - but
// the guest never got as far as asking, because the REPORT told it not to
// bother. A truthful allocator with a lying free-space report is a program
// that refuses to start.
//
// This is why Aladdin and Monkey Island are unaffected and were never
// evidence that DOS memory worked: neither does the high-load dance, so for
// them the bump pointer IS the true top of free memory and the old expression
// was accidentally correct.
//
// ===========================================================================
// AND WHY AH=26h IS IN THE SAME FILE
// ---------------------------------------------------------------------------
// The same trace shows the second half of the same guest operation:
//
//     [int21:dos] AH=26 (unimplemented) ax=2666 bx=1c58 cx=0008 dx=8b76
//
// AH=26h is Create New Program Segment Prefix, and 0x8b76 is exactly the
// high block from the dance above. LOAD.EXE relocates ITSELF into that block
// and re-enters, passing " HIGHLOAD " as the new instance's command tail, and
// the command tail lives in the PSP. Unimplemented, the function fell to the
// generic in-range default (CF=1, AX=1) - and LOAD.EXE's own error test is
// `or ax,ax / jnz`, i.e. it treats ZERO as the failure. AX=1 read as SUCCESS.
// So fixing only the free-space report would have moved the failure to a high
// copy entered with a 256-byte PSP of uninitialised guest RAM: no command
// tail, no memory-size word at +02h, no INT 22/23/24 save area. Both halves
// are needed to load one program.
// ===========================================================================

/// Mirrors `dos_mcb_t` in dos/dosexec.c. sizeof- and offsetof-locked there by
/// `_Static_assert`, because a silent layout drift here would make the free
/// space report wrong in a way that looks exactly like the bug being fixed.
#[repr(C)]
pub struct DosMcb {
    pub seg: u16,
    pub para: u16,
    pub live: i32,
}

/// Largest contiguous run of free paragraphs in `[floor, ceiling)`, where
/// "free" means no LIVE record covers it. This is what INT 21h AH=48h must
/// report in BX when it cannot satisfy a request, and what AH=48h with
/// BX=FFFFh exists to ask.
///
/// Returns paragraphs, 0 if nothing is free. `ceiling` is the guest's memory
/// ceiling in paragraphs (0xA000, the VGA aperture, for the DOS task).
///
/// O(holes * records). Called only on the failure path of an allocation, so
/// the allocate-and-keep fast path is byte-identical to before.
#[no_mangle]
pub extern "C" fn dos_mcb_largest_free_rs(
    mcb: *const DosMcb,
    n: i32,
    floor: u16,
    ceiling: u16,
) -> u16 {
    let ceil = ceiling as u32;
    let mut pos = floor as u32;
    if pos >= ceil {
        return 0;
    }
    let count = if mcb.is_null() || n <= 0 { 0usize } else { n as usize };

    let mut best: u32 = 0;
    // Each iteration strictly increases `pos` (see the two advance sites), so
    // this cannot spin: bounded by the number of live records plus one.
    let mut guard = 2 * count + 2;
    while pos < ceil && guard > 0 {
        guard -= 1;

        // Highest end among live records that COVER pos. A straddling record
        // must push pos past its end, not merely past its start.
        let mut covered_end: u32 = 0;
        // Lowest start among live records strictly above pos.
        let mut next_start: u32 = ceil;

        for i in 0..count {
            // SAFETY: the caller guarantees `mcb` points to at least `n`
            // contiguous readable DosMcb records (dos_task_t::mcb, a fixed
            // 512-entry array, with n = mcb_n <= 512).
            let m = unsafe { &*mcb.add(i) };
            if m.live == 0 || m.para == 0 {
                continue;
            }
            let bs = m.seg as u32;
            let be = bs + m.para as u32;
            if bs <= pos && pos < be {
                if be > covered_end {
                    covered_end = be;
                }
            } else if bs > pos && bs < next_start {
                next_start = bs;
            }
        }

        if covered_end > pos {
            pos = covered_end;
            continue;
        }
        // [pos, next_start) is free.
        let hole = next_start - pos;
        if hole > best {
            best = hole;
        }
        pos = next_start;
    }

    if best > 0xFFFF {
        0xFFFF
    } else {
        best as u16
    }
}

/// INT 21h AH=26h, Create New Program Segment Prefix, at `new_psp`.
///
/// MS-DOS copies the CURRENT PSP verbatim to DX:0000, then updates the
/// memory-size word at +02h and re-snapshots the INT 22h/23h/24h vectors from
/// the live IVT into +0Ah/+0Eh/+12h. The parent-PSP word at +16h becomes the
/// creating PSP. Everything else, including the handle table at +18h and the
/// far pointer to it at +34h, is carried across UNCHANGED: that is exactly why
/// a program that moves its PSP has to patch the segment at +36h itself, and
/// LOAD.EXE does precisely that two instructions after this call. Setting it
/// here would be a divergence with no upside.
///
/// `mem` is the guest's 1 MiB image, `block_end_para` the paragraph one past
/// the end of the memory block the new PSP sits in (PSP:+02h, "top of memory
/// segment"). Returns 0 on success, -1 if either PSP would fall outside the
/// 1 MiB image.
#[no_mangle]
pub extern "C" fn dos_psp_create_rs(
    mem: *mut u8,
    cur_psp: u16,
    new_psp: u16,
    block_end_para: u16,
) -> i32 {
    if mem.is_null() {
        return -1;
    }
    let src = (cur_psp as u32) << 4;
    let dst = (new_psp as u32) << 4;
    if src + 0x100 > 0x100000 || dst + 0x100 > 0x100000 {
        return -1;
    }

    // Copy through a temporary so a source/destination overlap cannot shred
    // the PSP being copied. 256 bytes of stack, once per program load.
    let mut psp = [0u8; 0x100];
    for i in 0..0x100usize {
        // SAFETY: bounds checked above; `mem` is the caller's 1 MiB buffer.
        psp[i] = unsafe { *mem.add(src as usize + i) };
    }

    // +00h: INT 20h, so a program that RETs to PSP:0 terminates.
    psp[0x00] = 0xCD;
    psp[0x01] = 0x20;
    // +02h: first paragraph beyond this program's memory block.
    psp[0x02] = (block_end_para & 0xFF) as u8;
    psp[0x03] = (block_end_para >> 8) as u8;
    // +0Ah/+0Eh/+12h: terminate / Ctrl-Break / critical-error vectors, taken
    // from the IVT as it stands NOW, which is the documented behaviour and the
    // reason a child can restore them on exit.
    for (k, vec) in [(0x0Ausize, 0x22usize), (0x0E, 0x23), (0x12, 0x24)] {
        let lin = vec * 4;
        for b in 0..4usize {
            // SAFETY: lin+3 <= 0x93 < 1 MiB.
            psp[k + b] = unsafe { *mem.add(lin + b) };
        }
    }
    // +16h: parent PSP.
    psp[0x16] = (cur_psp & 0xFF) as u8;
    psp[0x17] = (cur_psp >> 8) as u8;

    for i in 0..0x100usize {
        // SAFETY: bounds checked above.
        unsafe { *mem.add(dst as usize + i) = psp[i] };
    }
    0
}

/// Self-test. Returns the number of FAILING checks, so 0 is the pass. Run at
/// DOS task start next to the existing XMS/EMS and CRTC self-tests.
///
/// Case 2 is the measured Stunts geometry from the #172 trace, and it is the
/// one that makes this a regression test rather than decoration: the old
/// expression `0xA000 - alloc_top_para` returns 2 for it.
#[no_mangle]
pub extern "C" fn dos_mcb_selftest_rs() -> i32 {
    let mut fail = 0;

    // 1. Nothing allocated: the whole span above the floor is free.
    let empty: [DosMcb; 1] = [DosMcb { seg: 0, para: 0, live: 0 }];
    if dos_mcb_largest_free_rs(empty.as_ptr(), 1, 0x0719, 0xA000) != 0xA000 - 0x0719 {
        fail += 1;
    }

    // 2. THE STUNTS CASE. PSP block 0x0100+0x1425 (ends 0x1525) and the
    //    high-loaded block 0x8b76+0x1488 (ends 0x9FFE). The bump pointer is
    //    0x9FFE, so the old answer was 0xA000-0x9FFE = 2. The truth is the
    //    hole between them: 0x8b76 - 0x1525 = 0x7651.
    let stunts: [DosMcb; 2] = [
        DosMcb { seg: 0x0100, para: 0x1425, live: 1 },
        DosMcb { seg: 0x8b76, para: 0x1488, live: 1 },
    ];
    if dos_mcb_largest_free_rs(stunts.as_ptr(), 2, 0x0719, 0xA000) != 0x7651 {
        fail += 1;
    }

    // 3. The tail above the topmost block is the largest run.
    let tail: [DosMcb; 2] = [
        DosMcb { seg: 0x0100, para: 0x0100, live: 1 },
        DosMcb { seg: 0x0400, para: 0x0100, live: 1 },
    ];
    if dos_mcb_largest_free_rs(tail.as_ptr(), 2, 0x0100, 0xA000) != 0xA000 - 0x0500 {
        fail += 1;
    }

    // 4. A dead record is not an obstacle; a zero-length live one is not
    //    either. Both together must still see one unbroken span.
    let dead: [DosMcb; 2] = [
        DosMcb { seg: 0x1000, para: 0x1000, live: 0 },
        DosMcb { seg: 0x2000, para: 0x0000, live: 1 },
    ];
    if dos_mcb_largest_free_rs(dead.as_ptr(), 2, 0x0100, 0xA000) != 0xA000 - 0x0100 {
        fail += 1;
    }

    // 5. A record that STRADDLES the scan position must advance it past its
    //    END. Advancing only past its start would report a hole that is
    //    inside a live block, which is worse than under-reporting.
    let straddle: [DosMcb; 2] = [
        DosMcb { seg: 0x0100, para: 0x2000, live: 1 },
        DosMcb { seg: 0x0800, para: 0x2000, live: 1 },
    ];
    // Live coverage runs 0x0100..0x2800; free is 0x2800..0xA000.
    if dos_mcb_largest_free_rs(straddle.as_ptr(), 2, 0x0100, 0xA000) != 0xA000 - 0x2800 {
        fail += 1;
    }

    // 6. Floor at or above the ceiling is zero free, not an underflow.
    if dos_mcb_largest_free_rs(empty.as_ptr(), 1, 0xA000, 0xA000) != 0 {
        fail += 1;
    }
    if dos_mcb_largest_free_rs(core::ptr::null(), 0, 0xB000, 0xA000) != 0 {
        fail += 1;
    }

    // 7. A null table with a sane span is the whole span (no records = all
    //    free), which is the degenerate case the guard must not turn into 0.
    if dos_mcb_largest_free_rs(core::ptr::null(), 0, 0x0100, 0xA000) != 0xA000 - 0x0100 {
        fail += 1;
    }

    // 8. dos_psp_create_rs over a synthetic 1 MiB is not testable without a
    //    megabyte, so test the two rejections that need no buffer.
    if dos_psp_create_rs(core::ptr::null_mut(), 0x0100, 0x8b76, 0x9FFE) != -1 {
        fail += 1;
    }

    fail
}
