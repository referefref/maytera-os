#!/usr/bin/env bash
#
# sha1_hmac_kat.sh - host known-answer oracle for the kernel's SHA-1 and
# HMAC-SHA1 primitives (rustkern/sha1.rs + the HMAC-SHA1 arm of
# rustkern/hmac.rs).
#
# WHY THIS FILE EXISTS
# --------------------
# SHA-1 is new in this kernel and it exists for one reason: RFC 6238 TOTP
# specifies HMAC-SHA1, so a kernel-side secrets store needs it to verify the
# codes real authenticator apps generate. Because it is NEW code there is no C
# twin, so there is no [RUST-DIFF] differential to lean on: a passing kernel
# build proves the code COMPILES and LINKS and proves nothing whatever about
# whether the digest is SHA-1. The only thing that can prove that is a
# known-answer test against published vectors, and the only thing that proves
# the test is a test is watching it go RED.
#
# So this oracle compiles the REAL kernel sources (copied, not re-typed, so they
# cannot drift from what ships) for the host and:
#
#   GREEN : the unmodified rustkern/sha1.rs + rustkern/hmac.rs reproduce every
#           published vector, including the one-million-'a' streaming vector
#           that is too expensive to run on every boot, AND the boot self-test
#           sha1_selftest_rs() that main.c calls returns 0 with a non-zero
#           check count.
#   RED   : four mutations of a COPY of sha1.rs, each in a different part of the
#           algorithm, must every one of them make this oracle FAIL:
#             (a) a corrupted round constant   K(40..59)
#             (b) a rotate amount off by one   ROTL30 -> ROTL29
#             (c) a corrupted padding byte     0x80 -> 0x81
#             (d) a padding length calculation that is CORRECT for every message
#                 whose final block holds fewer than 56 bytes and wrong only at
#                 the boundary where the 8-byte length field needs a second
#                 block. (d) is the one that matters: (a) to (c) break nearly
#                 every digest, so any vector at all catches them, while (d) is
#                 invisible on "abc", on the empty string, on 55 or 64 or 65
#                 bytes and on the one-million-'a' vector. It is caught ONLY
#                 because the table deliberately covers 56, 57, 63 and 120
#                 bytes, and its smaller failing-vector count below is the
#                 evidence that those cases are load-bearing rather than
#                 decoration. A KAT that cannot see a boundary-only padding bug
#                 is not a SHA-1 KAT.
#
# WHY THE RED IS A MUTATED COPY RATHER THAN A *_KAT_FAULT DEFINE, which is how
# pbkdf2_shadow_kat.sh does it: a fault switch has to live in the shipping
# source, and for a brand-new primitive that means the kernel carries a
# deliberately-wrong code path forever to serve a host test. Mutating a copy
# proves the same property (the oracle detects a wrong digest) without that, and
# it is already an in-tree idiom: kernel/tools/mprotect-validate-test/selftest
# --prove-red does exactly this. Each mutation is VERIFIED to have changed the
# file, so a renamed constant can never silently turn the RED run into a second
# GREEN run that trivially "fails to fail".
#
# The expectations are published standards wherever one exists: FIPS 180-2 /
# RFC 3174 appendix vectors for SHA-1, RFC 2202 TC1/TC2/TC3/TC6/TC7 for
# HMAC-SHA1 (TC6/TC7 use a key longer than the 64-byte block, so they exercise
# the K' = H(key) branch of the shared hmac_construct). The six
# length-boundary digests have no published vector; they were reproduced with an
# independent implementation (Python hashlib) and are labelled as such below. A
# correct SHA-1 is the only thing that produces the published and the
# independently-derived values together.
#
# Picked up automatically by crypto/run-all-kats.sh (it globs *_kat.sh).
# Exit 0 iff GREEN is green AND all four REDs are red.
set -u

HERE="$(cd "$(dirname "$0")" && pwd)"
KERNEL="$(cd "$HERE/.." && pwd)"
SHA1_RS="$KERNEL/rustkern/sha1.rs"
HMAC_RS="$KERNEL/rustkern/hmac.rs"
RUSTC="${RUSTC:-rustc}"

[ -r "$SHA1_RS" ] || { echo "FATAL: cannot read $SHA1_RS"; exit 2; }
[ -r "$HMAC_RS" ] || { echo "FATAL: cannot read $HMAC_RS"; exit 2; }
command -v "$RUSTC" >/dev/null 2>&1 || { echo "FATAL: no rustc on PATH (set RUSTC=)"; exit 2; }

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

# 1) Host copies of the REAL kernel Rust sources, byte-identical. They need no
#    edit at all: both are plain `mod` files with no crate attributes, and they
#    use only `core`, which is a subset of std. That is the whole reason this
#    oracle can compile what ships rather than a transcription of it.
cp "$SHA1_RS" "$WORK/sha1_host.rs"
cp "$HMAC_RS" "$WORK/hmac_host.rs"

# 2) Host driver. `#[path]` pulls in the two real modules; the SHA-256/384/MD5
#    hash triples that hmac.rs also declares are stubbed, because this oracle is
#    about SHA-1 and the host link still needs those symbols to exist. A stub
#    cannot hide a failure: if HMAC-SHA1 ever reached one, every RFC 2202 vector
#    below would be wrong.
cat > "$WORK/driver.rs" <<'EOF'
#![allow(dead_code)]

#[path = "sha1_host.rs"]
mod sha1m;
#[path = "hmac_host.rs"]
mod hmacm;

mod stubs {
    #[no_mangle] pub extern "C" fn sha256_init(_c: *mut u8) {}
    #[no_mangle] pub extern "C" fn sha256_update(_c: *mut u8, _d: *const u8, _l: usize) {}
    #[no_mangle] pub extern "C" fn sha256_final(_c: *mut u8, _o: *mut u8) {}
    #[no_mangle] pub extern "C" fn sha384_init(_c: *mut u8) {}
    #[no_mangle] pub extern "C" fn sha512_update(_c: *mut u8, _d: *const u8, _l: usize) {}
    #[no_mangle] pub extern "C" fn sha384_final(_c: *mut u8, _o: *mut u8) {}
    #[no_mangle] pub extern "C" fn md5_init(_c: *mut u8) {}
    #[no_mangle] pub extern "C" fn md5_update(_c: *mut u8, _d: *const u8, _l: usize) {}
    #[no_mangle] pub extern "C" fn md5_final(_c: *mut u8, _o: *mut u8) {}
}

// A RAW 96-byte, 8-aligned buffer, deliberately NOT the Rust struct: this is
// exactly what a C caller passes (sha1_ctx_t per crypto/sha1.h), so the whole
// FFI contract including the size in that header's _Static_assert is what gets
// exercised here.
#[repr(align(8))]
struct CtxBuf([u8; 96]);

fn hex(d: &[u8]) -> String {
    let mut s = String::new();
    for b in d { s.push_str(&format!("{:02x}", b)); }
    s
}

fn one_shot(msg: &[u8]) -> String {
    let mut out = [0u8; 20];
    sha1m::sha1(msg.as_ptr(), msg.len(), out.as_mut_ptr());
    hex(&out)
}

fn streamed(chunks: &[&[u8]]) -> String {
    let mut ctx = CtxBuf([0u8; 96]);
    let p = ctx.0.as_mut_ptr();
    sha1m::sha1_init(p);
    for c in chunks {
        sha1m::sha1_update(p, c.as_ptr(), c.len());
    }
    let mut out = [0u8; 20];
    sha1m::sha1_final(p, out.as_mut_ptr());
    hex(&out)
}

fn repeated(byte: u8, total: usize, chunk: usize) -> String {
    let buf = vec![byte; chunk];
    let mut ctx = CtxBuf([0u8; 96]);
    let p = ctx.0.as_mut_ptr();
    sha1m::sha1_init(p);
    let mut done = 0usize;
    while done < total {
        let take = std::cmp::min(chunk, total - done);
        sha1m::sha1_update(p, buf.as_ptr(), take);
        done += take;
    }
    let mut out = [0u8; 20];
    sha1m::sha1_final(p, out.as_mut_ptr());
    hex(&out)
}

fn hmac(key: &[u8], data: &[u8]) -> String {
    let mut mac = [0u8; 20];
    hmacm::hmac_sha1_rs(key.as_ptr(), key.len(), data.as_ptr(), data.len(), mac.as_mut_ptr());
    hex(&mac)
}

// The check ledger. A plain struct rather than a closure so the totals can
// still be read at the end (a closure capturing them keeps them borrowed).
struct Ck { checks: usize, fails: usize }
impl Ck {
    fn eq(&mut self, label: &str, got: String, want: &str) {
        self.checks += 1;
        if got != want {
            self.fails += 1;
            println!("  FAIL {:<34} got {} want {}", label, got, want);
        } else {
            println!("  ok   {:<34} {}", label, got);
        }
    }
}

fn main() {
    let mut ck = Ck { checks: 0, fails: 0 };

    // ---- SHA-1, published vectors (FIPS 180-2 appendix A / RFC 3174). ----
    ck.eq("sha1(\"abc\")", one_shot(b"abc"), "a9993e364706816aba3e25717850c26c9cd0d89d");
    ck.eq("sha1(\"\")", one_shot(b""), "da39a3ee5e6b4b0d3255bfef95601890afd80709");
    ck.eq("sha1(448-bit vector)",
       one_shot(b"abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"),
       "84983e441c3bd26ebaae4aa1f95129e5e54670f1");

    // The one-million-'a' vector, the published multi-block streaming case.
    // Run twice with different chunk sizes: 1000 (an exact divisor, so every
    // update lands on a block boundary) and 7 (so every single update leaves a
    // partial block behind, which is the path the boot self-test cannot afford
    // to run 142858 times).
    ck.eq("sha1(1e6 x 'a'), 1000B chunks", repeated(b'a', 1_000_000, 1000),
       "34aa973cd4c4daa4f61eeb2bdbad27316534016f");
    ck.eq("sha1(1e6 x 'a'), 7B chunks", repeated(b'a', 1_000_000, 7),
       "34aa973cd4c4daa4f61eeb2bdbad27316534016f");

    // ---- Length-padding boundary. No published vector exists for these;
    // reproduced with Python hashlib (an independent implementation). 56 and 63
    // are the cases that force the 8-byte length field into a SECOND block. ----
    let a = |n: usize| -> Vec<u8> { vec![b'a'; n] };
    ck.eq("sha1(55 x 'a')",  one_shot(&a(55)),  "c1c8bbdc22796e28c0e15163d20899b65621d65a");
    ck.eq("sha1(56 x 'a')",  one_shot(&a(56)),  "c2db330f6083854c99d4b5bfb6e8f29f201be699");
    ck.eq("sha1(57 x 'a')",  one_shot(&a(57)),  "f08f24908d682555111be7ff6f004e78283d989a");
    ck.eq("sha1(63 x 'a')",  one_shot(&a(63)),  "03f09f5b158a7a8cdad920bddc29b81c18a551f5");
    ck.eq("sha1(64 x 'a')",  one_shot(&a(64)),  "0098ba824b5c16427bd7a1122a5a442a25ec644d");
    ck.eq("sha1(65 x 'a')",  one_shot(&a(65)),  "11655326c708d70319be2610e8a57d9a5b959d3b");
    ck.eq("sha1(119 x 'a')", one_shot(&a(119)), "ee971065aaa017e0632a8ca6c77bb3bf8b1dfc56");
    ck.eq("sha1(120 x 'a')", one_shot(&a(120)), "f34c1488385346a55709ba056ddd08280dd4c6d6");
    ck.eq("sha1(128 x 'a')", one_shot(&a(128)), "ad5b3fdbcb526778c2839d2f151ea753995e26a0");

    // ---- Streaming: the published 56-byte message split every awkward way
    // must equal its one-shot digest. Exercises the ctx tail buffer, which is
    // where a streaming hash goes wrong independently of its compressor. ----
    let m: &[u8] = b"abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    for step in [1usize, 2, 3, 7, 13, 31, 55, 56, 64] {
        let mut parts: Vec<&[u8]> = Vec::new();
        let mut off = 0usize;
        while off < m.len() {
            let take = std::cmp::min(step, m.len() - off);
            parts.push(&m[off..off + take]);
            off += take;
        }
        ck.eq(&format!("streamed 448-bit, {}B chunks", step), streamed(&parts),
           "84983e441c3bd26ebaae4aa1f95129e5e54670f1");
    }
    // Empty updates and a zero-length final update must change nothing.
    ck.eq("streamed with empty updates", streamed(&[b"", b"abc", b"", b""]),
       "a9993e364706816aba3e25717850c26c9cd0d89d");

    // ---- The FFI layout contract that crypto/sha1.h pins with a
    // _Static_assert. Checked here too so a reordering is caught even by a
    // build that does not compile the C header. ----
    ck.eq("sizeof(Sha1Ctx)", format!("{}", std::mem::size_of::<sha1m::Sha1Ctx>()), "96");
    ck.eq("alignof(Sha1Ctx)", format!("{}", std::mem::align_of::<sha1m::Sha1Ctx>()), "8");

    // ---- HMAC-SHA1, RFC 2202. TC6/TC7 keys are 80 bytes (> the 64-byte
    // block), which is the K' = H(key) branch of the shared construction. ----
    ck.eq("HMAC-SHA1 RFC2202 TC1", hmac(&[0x0bu8; 20], b"Hi There"),
       "b617318655057264e28bc0b6fb378c8ef146be00");
    ck.eq("HMAC-SHA1 RFC2202 TC2", hmac(b"Jefe", b"what do ya want for nothing?"),
       "effcdf6ae5eb2fa2d27416d5f184df9c259a7c79");
    ck.eq("HMAC-SHA1 RFC2202 TC3", hmac(&[0xaau8; 20], &[0xddu8; 50]),
       "125d7342b9ac11cd91a39af48aa17b4f63f175d3");
    ck.eq("HMAC-SHA1 RFC2202 TC6", hmac(&[0xaau8; 80],
       b"Test Using Larger Than Block-Size Key - Hash Key First"),
       "aa4ae5e15272d00e95705637ce8a3b55ed402112");
    ck.eq("HMAC-SHA1 RFC2202 TC7", hmac(&[0xaau8; 80],
       b"Test Using Larger Than Block-Size Key and Larger Than One Block-Size Data"),
       "e8e99d0f45237d786d6bbaa7965c7808bbff1a91");

    // ---- The BOOT self-test itself, exactly as main.c calls it. A KAT that
    // passes while the on-boot harness is broken would leave every real machine
    // unverified, so the harness is part of what is under test. ----
    let mut bchecks: u32 = 0;
    let rc = sha1m::sha1_selftest_rs(&mut bchecks as *mut u32);
    // Both halves are asserted on purpose: rc == 0 with ZERO checks is a
    // vacuous pass, which is the exact failure mode a self-test nobody has
    // watched go red actually has.
    ck.eq("sha1_selftest_rs (boot harness)",
          format!("rc={} any_checks={}", rc, bchecks > 0), "rc=0 any_checks=true");
    println!("  ...  boot harness ran {} checks", bchecks);

    println!("HOST-KAT: sha1_hmac checks={} fails={}", ck.checks, ck.fails);
    std::process::exit(if ck.fails == 0 { 0 } else { 1 });
}
EOF

build_and_run() {   # $1 = sha1 source to compile, $2 = label
    local src="$1" label="$2"
    cp "$src" "$WORK/sha1_host.rs"
    if ! "$RUSTC" --edition 2021 -O -A warnings \
            -o "$WORK/kat_$label" "$WORK/driver.rs" 2> "$WORK/cc_$label.log"; then
        echo "FATAL: compile failed for $label"
        cat "$WORK/cc_$label.log"
        exit 2
    fi
    "$WORK/kat_$label"
    return $?
}

mutate() {   # $1 = sed expression, $2 = output file, $3 = description
    sed "$1" "$WORK/sha1_shipped.rs" > "$2"
    if cmp -s "$WORK/sha1_shipped.rs" "$2"; then
        echo "FATAL: mutation '$3' changed NOTHING ($1 matched no line)."
        echo "       The RED run would then be a second GREEN run and would prove nothing."
        exit 2
    fi
}

cp "$SHA1_RS" "$WORK/sha1_shipped.rs"

echo "== GREEN: rustkern/sha1.rs + rustkern/hmac.rs exactly as they ship =="
build_and_run "$WORK/sha1_shipped.rs" green
GREEN=$?

echo
echo "== RED 1: round constant K(40..59) corrupted by one bit =="
mutate 's/0x8f1bbcdcu32/0x8f1bbcddu32/' "$WORK/sha1_red1.rs" "K(40..59) constant"
build_and_run "$WORK/sha1_red1.rs" red1 > "$WORK/red1.out" 2>&1
RED1=$?

echo "== RED 2: b ROTL 30 changed to ROTL 29 =="
mutate 's/b\.rotate_left(30)/b.rotate_left(29)/' "$WORK/sha1_red2.rs" "ROTL30 rotate amount"
build_and_run "$WORK/sha1_red2.rs" red2 > "$WORK/red2.out" 2>&1
RED2=$?

echo "== RED 3: padding byte 0x80 corrupted to 0x81 =="
mutate 's/pad\[0\] = 0x80;/pad[0] = 0x81;/' "$WORK/sha1_red3.rs" "padding 0x80 byte"
build_and_run "$WORK/sha1_red3.rs" red3 > "$WORK/red3.out" 2>&1
RED3=$?

echo "== RED 4: pad length correct below the boundary, wrong at it (the bug only"
echo "          the 56/57/63/120-byte vectors can see) =="
mutate 's|let zeros = .*|let zeros = if bl < 56 { 55 - bl } else { 0 };|' \
       "$WORK/sha1_red4.rs" "boundary-only pad length"
build_and_run "$WORK/sha1_red4.rs" red4 > "$WORK/red4.out" 2>&1
RED4=$?

echo "------------------------------------------------------------"
rc=0
if [ "$GREEN" -eq 0 ]; then
    echo "GREEN ok  : every published SHA-1 + RFC 2202 HMAC-SHA1 vector matches (exit 0)"
else
    echo "GREEN FAIL: the shipped code did not pass its own KAT (exit $GREEN)"; rc=1
fi
for n in 1 2 3 4; do
    eval "r=\$RED$n"
    case $n in
        1) what="corrupted round constant";;
        2) what="off-by-one rotate";;
        3) what="corrupted padding byte";;
        4) what="boundary-only pad length";;
    esac
    if [ "$r" -ne 0 ]; then
        echo "RED $n ok  : $what is CAUGHT (exit $r, $(grep -c '  FAIL' "$WORK/red$n.out") failing vectors)"
    else
        echo "RED $n FAIL: $what was NOT caught -> this oracle is a no-op"; rc=1
    fi
done
echo "------------------------------------------------------------"
[ "$rc" -eq 0 ] && echo "sha1_hmac_kat.sh: PASS" || echo "sha1_hmac_kat.sh: FAIL"
exit $rc
