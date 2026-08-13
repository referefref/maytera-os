// pwpolicy-hosttest.rs - run the EXACT kernel password policy on the host,
// against the full 50,000-word source list.
//
// WHY THIS EXISTS. The in-kernel self-test (pwpolicy_selftest_rs) proves the
// policy is live on a booted build, but it can only check a handful of vectors:
// the kernel does not ship the wordlist, so it cannot assert the property that
// actually matters, which is that EVERY ONE of the 50,000 breached passwords is
// refused. This harness compiles the same rustkern/pwpolicy.rs module (by
// #[path], not by copy, so it cannot drift) against std and runs the whole list
// through pw_check().
//
// Build and run:
//   rustc -O -o /tmp/pwhosttest kernel/tools/pwbreach-gen/pwpolicy-hosttest.rs
//   /tmp/pwhosttest rockyou-top50k.txt
//
// Exit 0 = every breached password refused and every strong password accepted.
// Exit 1 = at least one was not, with the offending strings printed.

#[path = "../../rustkern/pwpolicy.rs"]
mod pwpolicy;

use pwpolicy::*;

fn main() {
    let args: Vec<String> = std::env::args().collect();
    if args.len() != 2 {
        eprintln!("usage: pwpolicy-hosttest <wordlist.txt>");
        std::process::exit(2);
    }
    // BYTES, not text. Decoding would silently rewrite the non-UTF-8 lines,
    // and those were exactly the two the first version of this harness let
    // through: a 7-character word that is 8 BYTES of UTF-8 passes the kernel
    // length rule, so it has to be in the table.
    let data = std::fs::read(&args[1]).expect("read wordlist");
    let words: Vec<&[u8]> = data
        .split(|b| *b == b'\n')
        .map(|l| if l.last() == Some(&b'\r') { &l[..l.len() - 1] } else { l })
        .filter(|l| !l.is_empty())
        .collect();

    // 1. The in-kernel self-test, run here too.
    let mask = pwpolicy_selftest_rs();
    println!("selftest mask = 0x{:x} ({})", mask, if mask == 0 { "PASS" } else { "FAIL" });

    // 2. EVERY breached password must be refused, by SOME rule. Which rule is
    //    reported does not matter; being accepted does.
    let mut accepted: Vec<&[u8]> = Vec::new();
    let mut by_code = [0usize; 16];
    for w in &words {
        let r = pw_check(w, b"");
        by_code[(r as usize).min(15)] += 1;
        if r == PW_OK {
            accepted.push(w);
        }
    }
    println!("source words: {}", words.len());
    println!("  accepted (MUST be 0): {}", by_code[PW_OK as usize]);
    println!("  refused too-short:    {}", by_code[PW_ERR_TOO_SHORT as usize]);
    println!("  refused low-variety:  {}", by_code[PW_ERR_LOW_VARIETY as usize]);
    println!("  refused sequence:     {}", by_code[PW_ERR_SEQUENCE as usize]);
    println!("  refused breached:     {}", by_code[PW_ERR_BREACHED as usize]);
    println!("  refused bad char:     {}", by_code[PW_ERR_BADCHAR as usize]);
    for w in accepted.iter().take(20) {
        println!("  ACCEPTED BREACHED PASSWORD: {:?}", String::from_utf8_lossy(w));
    }

    // 3. Strong passwords must still be accepted, or the policy is just a
    //    refuse-everything stub. None of these are on the list.
    let good: [&str; 8] = [
        "Kx7#vqLm2Zt",
        "correct horse battery staple",
        "tr0ub4dour-and-3-more",
        "Zq!8xNv2Wm4",
        "maytera-os-build-1234",
        "j'ai perdu mon parapluie",
        "Th3-Quick-Br0wn-F0x-Jumps",
        "9d2f-4a7c-11ee-be56",
    ];
    let mut bad_reject = 0;
    for g in good.iter() {
        let r = pw_check(g.as_bytes(), b"alice");
        if r != PW_OK {
            println!("  STRONG PASSWORD REFUSED: {:?} -> code {}", g, r);
            bad_reject += 1;
        }
    }
    println!("strong passwords refused (MUST be 0): {}", bad_reject);

    // 4. False-positive sample: random 12-char passwords must almost never be
    //    reported as breached. Deterministic LCG so the run is reproducible.
    let alphabet: &[u8] = b"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    let mut state: u64 = 0x2026_0809_DEAD_BEEF;
    let mut fp = 0usize;
    let trials = 200_000;
    for _ in 0..trials {
        let mut buf = [0u8; 12];
        for b in buf.iter_mut() {
            state = state.wrapping_mul(6364136223846793005).wrapping_add(1442695040888963407);
            *b = alphabet[((state >> 33) as usize) % alphabet.len()];
        }
        if pw_check(&buf, b"") == PW_ERR_BREACHED {
            fp += 1;
        }
    }
    println!("false positives: {} / {} random 12-char passwords", fp, trials);

    let ok = mask == 0 && accepted.is_empty() && bad_reject == 0;
    println!("{}", if ok { "RESULT: PASS" } else { "RESULT: FAIL" });
    std::process::exit(if ok { 0 } else { 1 });
}
