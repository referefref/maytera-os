#!/usr/bin/env python3
"""One-shot splitter: rustkern.rs (9,566 lines) -> rustkern/<subsystem>.rs modules.

#404 / #526. This is a PURE REFACTOR: it slices the original file at its own
section-banner boundaries and emits one module per subsystem, so two agents
working on different subsystems no longer edit the same file. Not a rewrite:
every byte of logic is carried across verbatim. The proof is that `nm` reports
an IDENTICAL symbol list before and after.

Run once from kernel/. Kept in-tree as the record of exactly how the split was
performed (and so the boundaries can be re-derived if it is ever questioned).
"""
import os
import re
import sys

SRC = "rustkern.rs"
OUTDIR = "rustkern"

# (first_line, last_line, module) - 1-based, inclusive. Derived from the file's
# own banner lines; asserted against the live file below so a drifted source
# cannot be silently mis-sliced.
PLAN = [
    (62,   296,  "mono"),       # #525 shared TSC monotonic clock
    (298,  339,  "checksum"),   # ip_checksum_rs, which physically sat inside the
                                # mono block. It belongs with tcp/udp below.
    (340,  448,  "ext2"),
    (449,  553,  "checksum"),   # tcp/udp checksums (same module as ip above)
    (554,  670,  "sha256"),
    (671,  797,  "sha512"),
    (798,  922,  "md5"),
    (923,  1037, "md4"),
    (1038, 1373, "aes"),
    (1374, 1460, "chacha20"),
    (1461, 1677, "hmac"),
    (1678, 1866, "icmp"),
    (1867, 1975, "arp"),
    (1976, 2164, "dns"),
    (2165, 2425, "dhcp"),
    (2426, 2813, "url"),
    (2814, 3091, "elf"),
    (3092, 3306, "pe"),
    (3307, 3479, "fat"),
    (3480, 3684, "exfat"),
    (3685, 3885, "bmp"),
    (3886, 4168, "png"),
    (4169, 4573, "jpeg"),       # header parse
    (4574, 4975, "inflate"),
    (4976, 5266, "tls_parse"),
    (5267, 5385, "certverify"),
    (5386, 5771, "http"),
    (5772, 6144, "mp4"),
    (6145, 6304, "jpeg"),       # dequant + IDCT (same gui/jpeg.c subsystem)
    (6305, 6463, "http2"),
    (6464, 6707, "theme"),
    (6708, 6881, "wav"),
    (6882, 6961, "cert_b64"),
    (6962, 7066, "xattr"),
    (7067, 7087, "ed25519"),
    (7088, 7377, "xdr"),
    (7378, 7460, "taskmgr"),
    (7461, 7705, "parttbl"),
    (7706, 7850, "usb_desc"),
    (7851, 8027, "proc_mem"),
    (8028, 8080, "vfs_path"),
    (8081, 8147, "conn"),
    (8148, 8305, "procinfo"),
    (8306, 8532, "argtab"),     # the argument-descriptor helpers ...
    (8533, 8962, "argtab"),     # ... and THE TABLE that uses them: one module
    (8963, None, "tls12"),      # to EOF
]

# Anchors: a line that MUST appear at each range start, proving the plan still
# matches the file. A drifted source fails loudly instead of slicing garbage.
ANCHORS = {
    63:   "// #525: THE SHARED MONOTONIC CLOCK",
    296:  "}",                                                  # end of mono_tsc_khz_rs
    298:  "/// Faithful Rust port of net/ip.c : ip_checksum()",  # ip_checksum starts here
    450:  "// Phase D port (#404 / #486)",
    2815: "// #404 / #499 Phase Q",
    8307: "// #503 / MAYTERA-SEC-2026-0016",
    8534: "// THE TABLE. One line per syscall",
    8964: "// #502: TLS 1.2 (RFC 5246) client core",
}

HEADER = """// rust/{mod}.rs - {desc}
//
// Split out of the 9,566-line rustkern.rs (#404 / #526). PURE REFACTOR: the
// code below is carried across verbatim; the exported symbol list is unchanged
// and enforced by ../rust-symbols.manifest via tools/rust-symbol-gate.
//
// `#[no_mangle]` exports keep their exact C names regardless of the module they
// live in, so the FFI surface and every `extern` declaration on the C side are
// untouched.
"""

DESCS = {
    "mono": "#525 the shared TSC monotonic clock (C API in cpu/mono.h)",
    "checksum": "#404 IP / TCP / UDP ones-complement checksums (RFC 1071)",
    "ext2": "#404 Phase C / #485 ext2 directory-block entry scan",
    "sha256": "#404 Phase E / #487 SHA-256 block compression core",
    "sha512": "#404 Phase F / #488 SHA-512 block compression core",
    "md5": "#404 Phase G / #489 MD5 block compression core",
    "md4": "#404 Phase H / #490 MD4 block compression core",
    "aes": "#404 Phase J / #492 AES block encrypt + decrypt cores",
    "chacha20": "#404 Phase I / #491 ChaCha20 block (keystream) core",
    "hmac": "#404 Phase K / #493 HMAC construction (RFC 2104 / RFC 4231)",
    "icmp": "#404 Phase L / #494 incoming ICMP parse/validate",
    "arp": "#404 Phase M / #495 incoming ARP frame parse/validate",
    "dns": "#404 Phase N / #496 incoming DNS response parse/validate",
    "dhcp": "#404 Phase O / #497 incoming DHCP reply parse/validate",
    "url": "#404 Phase P / #498 URL-string parse (net/url.c)",
    "elf": "#404 Phase Q / #499 ELF64 header + program-header validation",
    "pe": "#404 Phase R PE32 pre-map validation (exec/pe.c)",
    "fat": "#404 Phase S VFAT directory-entry + LFN parse (fs/fat.c)",
    "exfat": "#404 Phase T / #501 exFAT directory entry-set decode",
    "bmp": "#404 Phase U BMP image decoder (Tier-2 untrusted input)",
    "png": "#404 Phase V PNG parse seams (gui/png.c)",
    "jpeg": "#404 Phase W + batch-1 gui/jpeg.c seams: header parse, dequant + IDCT",
    "inflate": "#404 Phase X / #502 DEFLATE/INFLATE decompression core",
    "tls_parse": "#404 Phase Y / #502 TLS record / handshake / certificate walkers",
    "certverify": "#510 / MAYTERA-SEC-2026-0017 TLS 1.3 CertificateVerify (RFC 8446 4.4.3)",
    "http": "#404 Phase Y HTTP response length-parse seam (net/https.c, net/wget.c)",
    "mp4": "#404 Phase Z / #505 ISO-BMFF / MP4 (M4A) sample-table parse",
    "http2": "#404 batch-1 HTTP/2 frame framing (net/http2.c)",
    "theme": "#404 batch-1 theme-file line tokenizer (gui/theme_parser.c)",
    "wav": "#404 batch-2 RIFF/WAVE header parse (media/wav.c)",
    "cert_b64": "#404 batch-2 PEM base64 -> DER decode (net/tls/cert_store.c)",
    "xattr": "#404 batch-2 / MAYTERA-SEC-2026-0011 on-disk xattr parse (fs/xattr.c)",
    "ed25519": "#404 batch-3 ed25519 point decode (crypto/ed25519.c unpack25519)",
    "xdr": "#404 batch-3 XDR decode primitives (net/rpc.c)",
    "taskmgr": "#404 Task Manager data core",
    "parttbl": "#404 driver/block tier: MBR / GPT partition-table parse",
    "usb_desc": "#404 driver tier: USB Audio Class configuration-descriptor parse",
    "proc_mem": "#487/#349 Task Manager accessor: per-process memory accounting",
    "vfs_path": "#487/#349 Task Manager accessor: open-handle paths",
    "conn": "#487/#349 Task Manager accessor: per-process network connections",
    "procinfo": "#487/#349 Ring-3 introspection builders (procinfo)",
    "argtab": "#503 / MAYTERA-SEC-2026-0016 THE SYSCALL POINTER CHOKE POINT",
    "tls12": "#502 TLS 1.2 (RFC 5246) client core - NEW code written in Rust",
}


def main():
    if not os.path.exists(SRC):
        sys.exit(f"split: {SRC} not found (run from kernel/)")
    lines = open(SRC).read().split("\n")
    n = len(lines)

    for ln, text in ANCHORS.items():
        got = lines[ln - 1]
        if text not in got:
            sys.exit(f"split: ANCHOR FAIL at line {ln}: expected {text!r}, got {got!r}\n"
                     f"       The source has drifted from the plan. Refusing to slice.")
    print(f"split: all {len(ANCHORS)} anchors matched; source is the expected {n}-line file")

    os.makedirs(OUTDIR, exist_ok=True)
    chunks = {}
    covered = set()
    for start, end, mod in PLAN:
        end = end if end is not None else n
        body = "\n".join(lines[start - 1:end]).rstrip("\n")
        chunks.setdefault(mod, []).append(body)
        covered.update(range(start, end + 1))

    # Every line 62..EOF must land in exactly one module (1..61 is the crate
    # root, handled by hand). A gap would silently DROP code.
    gaps = [i for i in range(62, n + 1) if i not in covered and lines[i - 1].strip()]
    if gaps:
        sys.exit(f"split: {len(gaps)} non-blank line(s) fall in NO module, first at "
                 f"{gaps[0]}: {lines[gaps[0]-1]!r}. Refusing to drop code.")
    print(f"split: every non-blank line from 62..{n} is covered by exactly one module")

    for mod, bodies in sorted(chunks.items()):
        path = os.path.join(OUTDIR, f"{mod}.rs")
        with open(path, "w") as fh:
            fh.write(HEADER.format(mod=mod, desc=DESCS.get(mod, "#404 Rust port")))
            fh.write("\n")
            fh.write("\n\n".join(bodies))
            fh.write("\n")
        print(f"  wrote {path:28s} {len(bodies)} chunk(s)")
    print(f"split: {len(chunks)} modules written to {OUTDIR}/")


if __name__ == "__main__":
    main()
