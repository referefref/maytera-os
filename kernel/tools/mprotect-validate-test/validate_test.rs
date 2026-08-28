// Host-side unit proof of kernel/rustkern/mprotect.rs::mprotect_validate_rs.
// Includes the REAL kernel source file verbatim (no copy of the logic) so this
// cannot drift from what ships. Run on the build host, not in the kernel.
include!(concat!(env!("MPROTECT_RS")));

const PAGE: u64 = 4096;
const USTART: u64 = 0x0000_0000_0040_0000;
const UEND: u64 = 0x0000_7FFF_FFFF_FFFF;

fn main() {
    let base: u64 = 0x8000_0000; // a normal user address, page aligned
    let mut fails = 0;

    let cases: Vec<(&str, u64, u64, u32, i32)> = vec![
        // --- ACCEPT ---
        ("plain RW one page",        base, PAGE,     0x3, MP_OK),
        ("read only",                base, PAGE,     0x1, MP_OK),
        ("exec (no write)",          base, PAGE,     0x5, MP_OK),
        ("PROT_NONE",                base, PAGE,     0x0, MP_OK),
        ("unaligned LEN is legal",   base, 1,        0x1, MP_OK),
        ("at USER_SPACE_START",      USTART, PAGE,   0x1, MP_OK),

        // --- REFUSE, each for its own reason ---
        ("unknown prot bit 0x8",     base, PAGE,     0x8, MP_E_PROT_BITS),
        ("unknown prot bit 0x40",    base, PAGE,     0x40, MP_E_PROT_BITS),
        ("all high bits",            base, PAGE, 0xFFFF_FFFF, MP_E_PROT_BITS),
        ("zero length",              base, 0,        0x1, MP_E_LEN),
        ("addr off by one",          base + 1, PAGE, 0x1, MP_E_ALIGN),
        ("addr off by 4095",         base + 4095, PAGE, 0x1, MP_E_ALIGN),
        ("W|X",                      base, PAGE,     0x6, MP_E_WX),
        ("R|W|X",                    base, PAGE,     0x7, MP_E_WX),

        // Overflow: the class C wraps silently.
        ("len = u64::MAX",           base, u64::MAX, 0x1, MP_E_OVERFLOW),
        ("addr+len wraps exactly",   base, 0u64.wrapping_sub(base), 0x1, MP_E_OVERFLOW),
        ("rounding overflows",       base, u64::MAX - base - 2048, 0x1, MP_E_OVERFLOW),

        // Range: the privilege boundary.
        ("kernel high half",         0xFFFF_FFFF_8000_0000, PAGE, 0x1, MP_E_RANGE),
        ("canonical hole start",     0x0000_8000_0000_0000, PAGE, 0x1, MP_E_RANGE),
        ("below USER_SPACE_START",   0x1000, PAGE,          0x1, MP_E_RANGE),
        ("null page",                0x0,    PAGE,          0x1, MP_E_RANGE),
        ("straddles the user top",   (UEND & !(PAGE - 1)) - PAGE, 4 * PAGE, 0x1, MP_E_RANGE),
    ];

    // Ordering check: W^X must be reported even when the range is also bad,
    // because prot is validated before the range. Documents the precedence.
    println!("{:<28} {:>18} {:>10} {:>8}  {}", "case", "addr", "len", "prot", "got/want");
    for (what, addr, len, prot, want) in cases {
        let got = mprotect_validate_rs(addr, len, prot);
        let ok = got == want;
        if !ok {
            fails += 1;
        }
        println!(
            "{:<28} {:#18x} {:>10} {:>8}  {:>3}/{:<3} {}",
            what, addr, len, prot, got, want,
            if ok { "OK" } else { "<<< WRONG" }
        );
    }

    // Exhaustive sweep: no input may ever be accepted outside the user half.
    // 200k pseudo-random triples, checking the SAFETY property directly rather
    // than trusting the case list to be complete.
    let mut st: u64 = 0x243F_6A88_85A3_08D3;
    let mut accepted_outside = 0u64;
    let mut accepted = 0u64;
    for _ in 0..200_000 {
        st ^= st << 13; st ^= st >> 7; st ^= st << 17;
        // Bias 3/4 of addresses INTO the user half so the accept path is
        // actually exercised. An unbiased sweep over the full 64-bit space
        // almost never lands in user memory and therefore proves nothing:
        // the first version of this accepted 1 triple in 200000.
        let addr = if st % 4 != 0 {
            USTART + (st % (UEND - USTART)) & !(PAGE - 1)
        } else {
            st & !(PAGE - 1)
        };
        st ^= st << 13; st ^= st >> 7; st ^= st << 17;
        let len = if st % 3 != 0 { (st % (1 << 30)) + 1 } else { st };
        let prot = (st & 0x7) as u32;
        if mprotect_validate_rs(addr, len, prot) == MP_OK {
            accepted += 1;
            let end = addr
                .checked_add(len)
                .and_then(|e| e.checked_add(PAGE - 1))
                .map(|e| e & !(PAGE - 1));
            match end {
                None => accepted_outside += 1,
                Some(e) => {
                    if addr < USTART || e > UEND || e <= addr {
                        accepted_outside += 1;
                    }
                }
            }
        }
    }
    println!("\nsweep: 200000 random triples, {} accepted, {} accepted OUTSIDE the user half (want 0)",
             accepted, accepted_outside);
    if accepted_outside != 0 { fails += 1; }
    if accepted == 0 { println!("WARNING: sweep accepted nothing, it proves nothing"); fails += 1; }

    println!("\n{}", if fails == 0 { "ALL VALIDATOR CASES PASS" } else { "FAILURES PRESENT" });
    std::process::exit(if fails == 0 { 0 } else { 1 });
}
