// tools/tls-frag-test/frag_test.rs - host harness over the OUTGOING TLS record
// fragmentation plan.
//
// It `include!`s the REAL kernel source (rustkern/tlsfrag.rs, or a mutant of it
// under --prove-red) so it cannot drift from what ships, then drives the SAME
// loop shape net/tls/tls.c tls_send() uses, through a fake transport that
// reproduces the wire format exactly, including the uint16_t record-header
// length field. A record body the header cannot express desyncs the parse, so
// the harness detects the truncation defect as well as the oversize one.
//
// The peer model is the part with teeth: it walks the stream using ONLY the
// declared lengths, the way a real server does, and refuses any record over
// 2^14 plaintext, which is the fatal record_overflow alert that killed every
// HTTPS POST whose whole request passed 16384 bytes.

include!(concat!(env!("TLSFRAG_RS")));

const CT_APP: u8 = 23;

struct Wire {
    bytes: Vec<u8>,
    bodies: Vec<usize>, // the REAL length written for each record
}

/// Fake transport: build the record exactly as tls_build_record_header() does,
/// with a 16-bit length field, and append the body. `enforce` mirrors the
/// tls_send_record() gate.
fn emit_record(w: &mut Wire, body: &[u8], enforce: bool) -> Result<(), String> {
    if enforce && tls_out_record_ok_rs(body.len() as u64) == 0 {
        return Err(format!("record gate refused a {}-byte body", body.len()));
    }
    let declared = (body.len() & 0xffff) as u16; // the uint16_t narrowing, verbatim
    w.bytes.push(CT_APP);
    w.bytes.push(0x03);
    w.bytes.push(0x03);
    w.bytes.push((declared >> 8) as u8);
    w.bytes.push((declared & 0xff) as u8);
    w.bytes.extend_from_slice(body);
    w.bodies.push(body.len());
    Ok(())
}

/// The peer. Uses only the declared lengths.
fn parse_stream(w: &Wire) -> Result<Vec<u8>, String> {
    let mut out = Vec::new();
    let mut i = 0usize;
    while i < w.bytes.len() {
        if i + 5 > w.bytes.len() {
            return Err(format!("truncated record header at offset {i}"));
        }
        let declared = ((w.bytes[i + 3] as usize) << 8) | (w.bytes[i + 4] as usize);
        if declared > TLS_MAX_PLAINTEXT_RS as usize {
            return Err(format!(
                "peer sends record_overflow: declared {declared} > {TLS_MAX_PLAINTEXT_RS}"
            ));
        }
        i += 5;
        if i + declared > w.bytes.len() {
            return Err(format!("record body runs past the stream at offset {i}"));
        }
        out.extend_from_slice(&w.bytes[i..i + declared]);
        i += declared;
    }
    Ok(out)
}

/// The tls_send() loop, transcribed. This is the thing under test.
fn tls_send_sim(payload: &[u8], enforce: bool) -> Result<Wire, String> {
    let mut w = Wire { bytes: Vec::new(), bodies: Vec::new() };
    let length = payload.len() as u64;
    if length == 0 {
        emit_record(&mut w, &[], enforce)?;
        return Ok(w);
    }
    let mut off: u64 = 0;
    let mut guard = 0u64;
    while off < length {
        let frag = tls_out_fragment_rs(length - off, TLS_MAX_PLAINTEXT_RS) as u64;
        if frag == 0 {
            return Err(format!("plan returned 0 with {} bytes left", length - off));
        }
        emit_record(&mut w, &payload[off as usize..(off + frag) as usize], enforce)?;
        off += frag;
        guard += 1;
        if guard > 1_000_000 {
            return Err("loop did not terminate".to_string());
        }
    }
    Ok(w)
}

fn payload_of(n: usize) -> Vec<u8> {
    // Non-repeating enough that a dropped or reordered fragment is visible.
    (0..n).map(|i| ((i * 31 + (i >> 8) * 7) & 0xff) as u8).collect()
}

// The sizes a real caller produces.
//   11500  ~ the 7701-byte JPEG flow that SUCCEEDED on golden 2474
//   23000  ~ the 15639-byte JPEG flow that FAILED with "POST returned -1"
//   67584  ~ aiclient.c BODY_MAX (65536) plus the request line and headers
const SIZES: &[usize] = &[
    0, 1, 5, 1024, 11500, 16383, 16384, 16385, 20000, 23000, 32768, 65535, 65536, 67584,
    131072,
];

fn main() {
    let mut failures = 0usize;
    let mut checks = 0usize;
    let mut fail = |msg: String, failures: &mut usize| {
        println!("FAIL: {msg}");
        *failures += 1;
    };

    // Case 1: the send loop must produce a stream a conforming peer accepts and
    // reassembles byte-for-byte. The record gate is OFF here on purpose, so what
    // fires on a bad plan is the peer's record_overflow, i.e. the real defect.
    for &n in SIZES {
        checks += 1;
        let payload = payload_of(n);
        match tls_send_sim(&payload, false) {
            Err(e) => fail(format!("size {n}: send loop failed: {e}"), &mut failures),
            Ok(w) => {
                if let Some(&big) = w.bodies.iter().find(|&&b| b > TLS_MAX_PLAINTEXT_RS as usize) {
                    fail(
                        format!("size {n}: emitted a {big}-byte record, over the {TLS_MAX_PLAINTEXT_RS} plaintext limit"),
                        &mut failures,
                    );
                    continue;
                }
                match parse_stream(&w) {
                    Err(e) => fail(format!("size {n}: peer rejected the stream: {e}"), &mut failures),
                    Ok(got) => {
                        if got != payload {
                            fail(
                                format!("size {n}: peer reassembled {} bytes, expected {n}", got.len()),
                                &mut failures,
                            );
                        }
                    }
                }
            }
        }
    }

    // Case 1b: with the shipped tls_send_record() gate ON, a legal plan must
    // still go through untouched. A gate that refuses correct traffic is its own
    // outage.
    for &n in SIZES {
        checks += 1;
        if let Err(e) = tls_send_sim(&payload_of(n), true) {
            fail(format!("size {n}: the record gate refused a legal plan: {e}"), &mut failures);
        }
    }

    // Case 2: the fragment plan itself, at the edges.
    for &(remaining, cap, want) in &[
        (0u64, 16384u32, 0u32),
        (1, 16384, 1),
        (16383, 16384, 16383),
        (16384, 16384, 16384),
        (16385, 16384, 16384),
        (u64::MAX, 16384, 16384),
        (100000, 99999, 16384), // a caller may not widen the protocol limit
        (100, 0, 100),          // nor pass 0 and make the loop spin
    ] {
        checks += 1;
        let got = tls_out_fragment_rs(remaining, cap);
        if got != want {
            fail(format!("tls_out_fragment_rs({remaining}, {cap}) = {got}, want {want}"), &mut failures);
        }
    }

    // Case 3: the outgoing record gate, including the case that used to wrap the
    // uint16_t header field (a BODY_MAX-sized body: 8 + 66000 + 16 = 66024).
    for &(body, want) in &[
        (0u64, 1i32),
        (1, 1),
        (16401, 1), // 2^14 plaintext + content type + GCM tag (TLS 1.3)
        (16408, 1), // 8-byte explicit nonce + 2^14 + tag (TLS 1.2)
        (TLS_MAX_RECORD_RS as u64, 1),
        (TLS_MAX_RECORD_RS as u64 + 1, 0),
        (65535, 0),
        (65536, 0),
        (66024, 0),
    ] {
        checks += 1;
        let got = tls_out_record_ok_rs(body);
        if got != want {
            fail(format!("tls_out_record_ok_rs({body}) = {got}, want {want}"), &mut failures);
        }
    }

    // Case 4: the constants must match net/tls/tls.h.
    checks += 1;
    if tls_out_max_plaintext_rs() != 16384 {
        fail(
            format!(
                "tls_out_max_plaintext_rs() = {}, want 16384 (tls.h TLS_MAX_PLAINTEXT_SIZE)",
                tls_out_max_plaintext_rs()
            ),
            &mut failures,
        );
    }

    println!("tls-frag-test: {checks} checks, {failures} FAILURES");
    if failures > 0 {
        std::process::exit(1);
    }
}
