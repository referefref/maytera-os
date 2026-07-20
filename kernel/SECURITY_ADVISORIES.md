# MayteraOS Security Advisories - Rust-port memory-safety findings

This register tracks every **genuinely reachable** memory-safety defect discovered and closed
during the incremental Rust kernel port (task #404). Each entry has a stable, unique identifier
`MAYTERA-SEC-2026-NNNN`, assigned in discovery order.

Scope + rules:
- One advisory per reachable defect the Rust port removes **by construction** (bounds-checked
  slices + no unchecked pointer/index arithmetic). Latent / defense-in-depth ports (where the C
  was already correctly bounded, e.g. inflate, arp, dns, url, exfat, bmp) do NOT get an advisory.
- Every advisory is proven against the C on a crafted input, and the fix confines the identical
  input; both run in the offline differential harness. AddressSanitizer is the usual witness, but
  it is **not sufficient everywhere** and the register no longer claims it is: 0006's Huffman-index
  read lands past ASan's redzone in valid heap and needs a **guard page**; 0006's DHT write is
  intra-object and ASan is silent on it; 0001 cannot be witnessed at all because no artifact of the
  vulnerable code survives. The evidence basis is stated per advisory rather than blanket-claimed.
  See **Corrections**.
- Each advisory cross-references its **C-fallback hardening ticket** (the fix that keeps the
  `-DRUST_*` flag-off rollback path safe), the **build the fix was introduced in**, and the
  **golden fold** that shipped it to the live image users boot.
- When a new reachable defect is found: assign the next `MAYTERA-SEC-2026-NNNN`, append a row here
  AND to RUST_PORT_LEDGER.md, update the public security page on maytera.net, and keep this file
  md5-identical on both trees (local + the kernel build container).

Product line for all builds below: **MayteraOS v1.95.x**. The meaningful version identifier is the
**build number** (shown as "v1.95.0 (build NNN)" on the desktop).

Severity is an informal High/Medium scale weighted by: write-vs-read, reachability
(remote > network-LAN > local-file/disk), and pre-auth exposure.

CWE legend: CWE-125 out-of-bounds read; CWE-787 out-of-bounds write; CWE-190 integer overflow
(leading to an undersized allocation / wrapped bound).

| ID | Component (file) | Class (CWE) | Severity | Reachability / attack vector | Fixed by | C ticket | Affected builds | Patched in |
|---|---|---|---|---|---|---|---|---|
| MAYTERA-SEC-2026-0001 | ext2 directory parse (`fs/ext2.c` `ext2_lookup` / `ext2_dirblock_find`) | CWE-125 | Medium | Local: reading a **crafted ext2 image** with malformed directory `rec_len`/`name_len` heap over-reads on any lookup | **plain-C #476 guards (all four `fs/ext2.c` dir walkers)**; subsequently mirrored by `ext2_dirblock_find_rs` (b794), which makes the class impossible by construction. **The Rust is NOT load-bearing for this fix.** The pre-fix original is not recoverable and cannot be re-witnessed: see Corrections | #476 | <= build 793 | **build 794** |
| MAYTERA-SEC-2026-0002 | DHCP reply parse (`net/dhcp.c` `dhcp_parse`) | CWE-125 | Medium | Network (LAN): a **spoofed runt DHCP OFFER** drives the option TLV walk past the packet buffer | `dhcp_parse_rs` | #488 | <= build 805 | **build 806** |
| MAYTERA-SEC-2026-0003 | ELF loader validate (`exec/elf.c` `elf_validate` / `calculate_load_bounds`) | CWE-190 -> CWE-787 (+CWE-125) | High | Local file: a **crafted ELF** (oversized `p_filesz` underflow + undersized `e_phentsize`) yields an OOB heap **write** on load | `elf_validate_full_rs` | #489 | <= build 807 | **build 808** |
| MAYTERA-SEC-2026-0004 | FAT/VFAT LFN reassembly (`fs/fat.c` `fat_dir_step`) | CWE-787 | High | Local, **reachable from Ring-3 via `SYS_READDIR`**: a crafted FAT long-file-name overruns the name buffer (260->256) heap **write** | `fat_dir_step_rs` (caps at 255) | #490 | <= build 809 | **build 810** |
| MAYTERA-SEC-2026-0005 | PNG decoder (`gui/png.c` IHDR size math + BGRA convert loop) | CWE-190 -> CWE-787 (+CWE-125) | High | Untrusted image: a **crafted PNG** whose IHDR width/height wrap the uint32 size math into an undersized allocation, giving an OOB **write** (ASan: WRITE of size 4, plus OOB reads) in the **BGRA convert loop**, which is unseamed plain C. **Working PoC: `width=0x40000004`, RGBA, `h=1`** (see Corrections: the previously published `0x40000000` does NOT reproduce). Fed by browser `<img>`, Files previews and downloads. **NOT wallpapers** (`g_wallpapers[]` is entirely `.BMP`) | `png_parse_ihdr_rs` (load-bearing: its checked math rejects the crafted IHDR before the write site is reached) + `png_defilter_rs` | #500 | <= build 812 | **build 813** |
| MAYTERA-SEC-2026-0006 | JPEG decoder (`gui/jpeg.c` SOF0/SOS/DHT header parse) | CWE-787 (intra-object) + CWE-125 | High | Untrusted image: a **crafted JPEG** with three unvalidated header fields. (1) `comp_qt` quant-index OOB **read**: `quant[200][0]`, 12,800 bytes past a 256-byte object (ASan-proven). (2) `comp_dc`/`comp_ac` Huffman-index OOB **read**: `huff_fast[0][15][0]`, 22,528 bytes past an 8192-byte object. **This read is invisible to ASan** (it lands past the redzone in valid heap); it is witnessed with a **guard page**, and an ASan-only retest reports a false clean. (3) DHT count-sum with no clamp: a 4080-byte **write** past `huff_vals[]`. **In the pre-port original (3) is INTRA-OBJECT corruption, not an out-of-allocation write** (it ends at offset 5184 inside the 9760-byte `kzalloc`'d `jpeg_decoder_t`, absorbed by the 8192-byte `huff_fast` cushion; ASan silent, canary intact). It is real corruption (`huff_valid` is smashed, and it drives `decode_huff`), but it does not leave the allocation: see Corrections. Fed by album art, browser `<img>`, Files previews | `jpeg_parse_headers_rs` (confines all three) | #501 | <= build 813 | **build 814** |
| MAYTERA-SEC-2026-0007 | TLS handshake parse (`net/tls/tls.c` TLS 1.2 handshake-message loop) | CWE-125 | High (remote, pre-auth) | **Remote, pre-authentication**: a malicious or MITM server's first flight declares an oversized handshake length; the plaintext ServerHello loop over-reads (ASan: 4094 bytes past a 5-byte body). Runs on **every** HTTPS handshake | `tls_hs_next_rs` | #503 | <= build 815 | **build 816** |
| MAYTERA-SEC-2026-0008 | HTTP chunked decode (`net/https.c` `https_dechunk`) | CWE-190 -> CWE-787 (+CWE-125) | High (remote) | **Remote**: a malicious/compromised HTTPS origin the browser visits (or a compromised API/update endpoint) sends a chunked body whose hex chunk-size is near 2^32; the clamp `if (in + sz > len) sz = len - in;` is u32 and WRAPS, so the clamp is skipped and the following `memmove` over-copies ~4 GiB out of the `kmalloc`'d body (OOB read + **WRITE**). Reachable behind the `https_chunked_is_complete` gate (the crafted body `"FFFFFFFE\r\n0\r\n\r\n"` makes it return 1). Runs on **any** chunked HTTPS response (Kimi/LLM API, browser https, update, widget feeds) | `https_dechunk_rs` | #504 | <= build 816 | **build 817** |
| MAYTERA-SEC-2026-0009 | AAC/M4A ISO-BMFF sample-table parse (`media/aac.c` `mp4_parse`) | CWE-125 | Medium (local file) | Local, **reachable from Ring-3 via `SYS_PLAY_WAV`** (`sys_play_wav` -> `audio_play_file` -> `fat_read_file` -> `audio_decode_open` -> `aac_create` -> `mp4_parse`): a **crafted `.m4a`** (on disk or downloaded) whose `stsz`/`stco`(`co64`)/`stsc` declared sample-table counts (`nsamp`/`nchunks`/`nstsc`) exceed the tables actually present drives the chunk/sample walk `be32(d + stsz_tab/stco_tab/stsc_tab + i*stride)` far past the `kmalloc`'d file buffer -> heap OOB **read** (`nstsc` has NO clamp; `nsamp`/`nchunks` are clamped only to <= 10,000,000). A runt `stsz`/`stco`/`stsc` atom at end-of-buffer also over-reads its own count field | `mp4_parse_rs` | #505 | <= build 818 | **build 819** |
| MAYTERA-SEC-2026-0010 | HTTP/2 frame parse (`net/http2.c` `http2_get` PADDED branch) | CWE-476 (rooted in CWE-125) | High (remote, pre-auth) | **Remote, pre-authentication**: a malicious HTTPS site the browser visits negotiates ALPN `h2` and sends one crafted **zero-length PADDED** DATA or HEADERS frame (`flen==0`, flags `0x08`). The inline framing read the pad-length byte `payload[0]` / `pp[off]` WITHOUT first checking the frame carries any payload; `payload` is only `kmalloc`'d when `flen>0`, so it is **NULL** -> Ring-0 NULL-pointer dereference / OOB read -> kernel page fault -> whole-OS DoS. Runs on **any** h2 response frame the browser processes | `http2_frame_next_rs` | #506 | <= build 821 | **build 822** |
| MAYTERA-SEC-2026-0011 | on-disk xattr entry-walk (`fs/xattr.c` `xattr_get` / `xattr_list`) | CWE-125 | Medium | Local, **reachable from Ring-3 via `sys_getxattr` / `sys_listxattr`**: a **crafted/corrupt FAT image**'s `/.xattr/XXXXXXXX.xat` block declares per-entry `name_len` / `value_len` (and `attr_count`) past the buffer; the C get/list walk advances the entry pointer by those unchecked on-disk lengths, so `attr_name` / `attr_value` point past the `kmalloc`'d file buffer -> `strcmp` / `memcpy` heap over-read (info-leak / DoS). The Rust seam rejects (`rc != 1`) BEFORE any name/value is dereferenced | `xattr_entry_next_rs` | #508 | <= build 822 | **build 823** |
| MAYTERA-SEC-2026-0012 | NFS3 READ reply parse (`net/nfs.c` `nfs_read`) | CWE-787 | High (remote) | **Remote** (a malicious/compromised NFS server the client mounts + reads, `nfs://host/export` via `fs/netfs.c`): the server-declared `READ3resok` `data_len` is passed straight to `xdr_opaque(reply, buffer, data_len)` with NO clamp to the destination. `xdr_opaque` bounds only the SOURCE read against `reply->size`, so a `data_len` larger than the requested count (but present in the reply buffer) makes `memcpy` over-**write** the caller destination `buffer` (sized to the count, itself clamped to the server-supplied file size). ASan-proven (WRITE of size 2000, 0 bytes past a 512-byte region). The source-bounded XDR Rust seam does NOT confine this destination write; fixed directly in `nfs.c` by clamping `data_len` to `count` | (nfs.c `data_len` clamp; not a Rust seam) | #509 | <= build 823 | **build 824** |
| MAYTERA-SEC-2026-0013 | JPEG Huffman table build (`gui/jpeg.c` `build_huffman`) | CWE-787 | High | **Reachable from Ring-3 via `sys_decode_image`** (`proc/syscall.c:3983`, the userland browser's `<img>` path), Files previews (`gui/thumbnailer.c:474`), imageviewer, filebrowser, ipp: a **crafted JPEG** whose DHT declares a **non-canonical** code-length table (e.g. `bits[0]=255`, i.e. 255 one-bit codes where the code space allows at most 2) drives `idx_fast = (code << (10-len)) | f` past `huff_fast[dc][idx]` (`[1024]` of `int16_t`). Measured on an exact-size allocation: writes indices 0..130559 = **259,072 bytes past the array, 252,736 bytes past the 9760-byte `jpeg_decoder_t`**, with attacker-chosen `vals[k]` in the low byte of each entry. **LIVE under the shipped `-DRUST_JPEG`**: `jpeg_dht_rs`'s only gate was `total > 256`, and a count-sum of 255 passes it, so the seam ACCEPTED the table and handed it straight to the write. ASan-proven end-to-end through the real shipped seam (`jpeg_parse_headers_rs ret=0 ACCEPTED` -> `heap-buffer-overflow WRITE of size 2 ... 0 bytes to the right of 9760-byte region`). NOT extraction drift: `build_huffman` is byte-identical to the pre-extraction original (md5 `903798ad...`); b814 moved it just OUTSIDE the seam boundary so it was never ported, which NARROWED the reach (the original fired it during `parse_dht` with no SOS needed) but left it live | **plain-C canonicality + `k >= 256` bound in `build_huffman`** (flag-independent); `jpeg_dht_rs` tightened to match so both layers agree | #518 | <= build 825 | **build 826** |
| MAYTERA-SEC-2026-0014 | IPv4 receive path (`net/ip.c` `ip_handle`) | CWE-191 -> CWE-125 | High (remote, LAN) | **Remote, unauthenticated, from any host on the LAN** (no IP address required: `our_ip == 0` DHCP mode accepts any frame, and a broadcast destination is accepted regardless). `ip_handle` guarded `ihl <= length` and `total_length <= length` but **never `total_length >= ihl`**, so `uint16_t payload_length = total_length - ihl` UNDERFLOWED. A crafted **60-byte** frame (IHL=15 => `ihl=60`, `total_length=20`) yields `payload_length = 65496`, which is passed to **every** registered protocol handler. MEASURED by driving the real verbatim `ip_handle`: `icmp_handle`, `udp_handle` AND `tcp_handle` are each CALLED with `len=65496` from a 60-byte frame. ASan-witnessed over-reads in the real handlers on an exact-size 60-byte heap frame: `udp_handle` READ size 2 (4 bytes right of the region), `tcp_handle` prologue READ size 1 (12 bytes right). Propagation is real, not theoretical: with the bytes following the frame in the driver's fixed RX buffer under attacker influence (stale bytes from a previous packet), a bound UDP consumer (DHCP client port 68, DNS 53) is handed **`data_len=65488`** and the TCP prologue reaches **`payload_len=65476`**. This also widens MAYTERA-SEC-2026-0004's (dhcp) reach. The ICMP arm was confined only by accident of `-DRUST_ICMP`'s `ICMP_MAX_LEN`; the UDP/TCP arms were **not confined at all** (`net/ip.c` is unported) | **plain-C `if (total_length < ihl) return;` in `ip_handle`** (one line; fixes all three protocols at the root, flag-independent) | #517 | <= build 825 | **build 826** |

## Corrections (2026-07-16)

An exhaustive two-part **extraction-drift audit** re-witnessed **all 12** advisories against the
**pre-extraction original C** under a 3-way oracle (ORIGINAL == TWIN == RUST), covering all 36
shipped Rust seams over approximately 25 million differential vectors. See `DRIFT_AUDIT.md` (8
seams) and `DRIFT_AUDIT2.md` (28 seams).

**All 12 advisories remain VALID. None was invalidated.** Every one is still a real, reachable
defect, and the severities are unchanged. **Three carried wording errors**, corrected in the table
above and on the public page at maytera.net/security on 2026-07-16:

**1. 0001 (ext2): the fix was mis-attributed, and the original is not recoverable.**
The register credited the Rust seam `ext2_dirblock_find_rs`. That was wrong. The **plain-C #476
guards** on all four `fs/ext2.c` directory walkers are the actual fix, and they landed in the same
build **before** the Rust fold. Unlike elf/dhcp/xattr, **the Rust is not load-bearing here**; it
mirrors the C guards byte-for-byte and removes the class by construction. Separately, 0001 is the
one advisory that **cannot be re-witnessed**: the #476 fix predates every retained snapshot (all 27
tarballs across both backup trees were checked; the earliest already carries the guards), so **no
artifact of the vulnerable code survives**. The audit's witness is therefore a **reconstruction**,
produced by mechanically deleting the named guard from the recovered original, and it is labelled
as such rather than presented as a reproduction. The bug itself remains documented history
(#476, `version.h:177-189`), and the advisory stands on that record.

**2. 0005 (PNG): three errors, one of which defeats the reproducer.**
- **(a) The published PoC value does not work.** `width=0x40000000` wraps the pixel allocation to
  **exactly zero**; `kmalloc(0)` returns NULL, so every arm returns a clean `PNG_ERR_NOMEM` with no
  fault. It wraps to zero, not "to tiny", and zero is caught. Anyone reproducing 0005 from the
  documented example would wrongly conclude it is not exploitable. The working value is
  **`0x40000004`** (a wrap to tiny-but-nonzero), RGBA, `h=1`, into an exact-size heap buffer.
- **(b) "OOB write in defilter" was wrong for the original.** The original's write is in the **BGRA
  convert loop**, which was never seamed and is still plain C; the pre-extraction defilter step
  could not OOB-write at all. Practically: what fixes 0005 live is `png_parse_ihdr_rs`'s checked
  math rejecting the IHDR first, **not** `png_defilter_rs`'s bounds. The write site itself is
  unprotected and is safe only because the parse rejects before it is reached.
- **(c) "Fed by ... wallpapers" was false.** `gui/desktop.c` has zero PNG references; `g_wallpapers[]`
  is entirely `.BMP` and calls `image_load_bmp` directly. Browser `<img>`, Files previews and
  downloads are correct.

The CWE-190 -> CWE-787 chain, High severity, and browser/preview reach all stand, re-witnessed
against the original (`heap-buffer-overflow WRITE of size 4` plus OOB reads, then SEGV).

**3. 0006 (JPEG): claim 3 overstated the original, and claim 2's evidence basis is restated.**
- **Claim 3 shrinks.** "DHT count-sum with no clamp (OOB write)" described an **out-of-allocation**
  write. Against the pre-port original it is not one: the 4080-byte write ends at offset 5184
  inside the 9760-byte `kzalloc`'d `jpeg_decoder_t`, absorbed by the 8192-byte `huff_fast` cushion,
  with ASan silent and the canary untouched. It is real memory corruption (it smashes `huff_valid`,
  which drives `decode_huff`), but it is **intra-object**. The genuine **out-of-object** write we
  originally cited exists **only in our own extracted C twin** (a 1512-byte stack `jpeg_hdr_t`,
  where the same DHT lands 3824 bytes past the end). We published an artifact of our own extraction
  as if it were the shipping bug. That is our error, and this is the correction.
- **Claim 2's read is invisible to AddressSanitizer.** It lands 22,528 bytes past an 8192-byte
  object, past the redzone and inside valid heap, so **an ASan-only retest reports a false clean**.
  It is witnessed with a guard page.

0006 is **not invalidated**: claims 1 and 2 are re-witnessed reachable OOB reads, claim 3 is
re-witnessed as genuine corruption, and the Rust seam confines all three, so the High severity is
unchanged. The class is now stated as CWE-787 (intra-object) + CWE-125.

The audit re-witnessed the other nine advisories as accurate, including **0007**'s exact "4094
bytes past a 5-byte body" figure and **0008**'s OOB **write** claim, which needed its own witness
(a mapping where only writes fault, attested by page-fault error code `REG_ERR=0x7`) because ASan
reports only the read range.

**4. 0006 (JPEG) has a sibling the audit found LIVE, filed above as its own advisory.**
`build_huffman`'s unbounded `code` is worse than everything 0006 lists and, unlike all three of
0006's claims, the Rust seam did **not** confine it: a non-canonical DHT (`bits[0]=255`) sums to
255, passes `jpeg_dht_rs`'s `total > 256` gate, and is handed straight to a ~259 KB out-of-bounds
**write**. It is the only finding in either audit that is both **live under a shipped flag** and a
**write primitive**. It is filed as its own row (id pending) rather than folded into 0006 because
its root cause is different: 0006's three OOBs are all inside the seamed header parse, whereas this
one is in plain C that the b814 extraction moved just **outside** the seam boundary and therefore
never ported. That is the structural lesson of the whole audit: the seams are sound, and the code
left adjacent to them is where the bugs now are. Note also that `build_huffman` is **byte-identical**
to the pre-extraction original, so this is a pre-existing bug the port neither introduced nor fixed,
only narrowed.

Why this is recorded here rather than quietly edited: the register's value is that it can be
checked. A published PoC that does not reproduce, a fix credited to the wrong code, and a claim
that describes our own extraction artifact instead of the shipping bug are all defects in the
report, and they are logged like any other defect.

## Notes / assessed-clean (no advisory)

- **inflate / DEFLATE** (`gui/png.c` `inflate`, ticket #502): assessed under ASan over 3M+ hostile
  vectors (including distance-before-window and length-past-output); the C back-reference copy is
  already bounded on both ends. **No reachable OOB** -> no advisory. #502 filed then closed as
  not-needed. Rust (`inflate_rs`) still removes the class by construction (defense-in-depth).
- Other latent/defense-in-depth ports with no reachable defect: ip/tcp/udp checksums, sha256/512,
  md4/5, chacha20, aes, hmac, arp, dns, url, pe, exfat, bmp, inflate. **`icmp` was REMOVED from this
  list on 2026-07-16**: its `ICMP_MAX_LEN` bound was rated defense-in-depth on the reasoning that "IP
  delivers ICMP payloads bounded by total_length <= frame len <= IP_MTU", which is FALSE. The
  `net/ip.c` `total_length - ihl` underflow (new advisory above) delivered `len=65496` to `icmp_parse`
  from a 60-byte frame, so that bound was confining a **reachable** OOB and a `-DRUST_ICMP` rollback was
  **not** safe. Fixed at the root in plain C; the bound is genuinely defense-in-depth again. The HTTP header-block
  framing (`find_header_end`), the size_t chunked decoder (`http_decode_chunked`, used by plain
  HTTP + https_post), and the Content-Length digit parse were ported alongside 0008 but are already
  correctly bounded in C (the size_t decoder rejects an oversized chunk before any copy; a
  Content-Length overflow only bounds a separately-bounded receive loop) -> no advisory.
- **JPEG dequant + inverse-DCT** (`gui/jpeg.c` `jpeg_dequant_idct`, seam `jpeg_dequant_idct_rs`,
  build 822): assessed under ASan over 3.2M offline vectors - both C and Rust confine every write to
  the fixed 64-entry block (loop-constant/zigzag indices), so **no reachable memory OOB** -> no
  advisory. It DOES carry a genuinely reachable **CWE-190 signed-integer-overflow (undefined
  behavior)** in the C integer IDCT on large coefficient products (a crafted JPEG can carry
  `ac_val` up to +-32767 * quant up to 255; UBSan-proven offline, reachable even in the realistic
  coefficient band). On the current gcc -O2 x86-64 build it wraps two's-complement (benign garbage
  pixels, 0 differential mismatch) but is UB the compiler is licensed to miscompile. The Rust
  `wrapping_*` ops make it **well-defined** and byte-identical to the observed wrap. Because it is
  not memory corruption, this is recorded as defense-in-depth, NOT an advisory; the plain-C
  hardening that makes the overflow defined (e.g. i64 intermediates), so a `-DRUST_JPEG_ENTROPY=off`
  rollback stays UB-free, is **ticket #507**.
- **theme-file line tokenizer** (`gui/theme_parser.c` `theme_parse_ini`, seam `theme_parse_line_rs`,
  build 822): the verbatim C is already fully bounded (ASan-clean over 32.6M malformed vectors; every
  fixed output field is cap-checked) so there is **no reachable OOB** -> no advisory. Defense-in-depth
  (removes the raw-scan class by construction). One rs/c divergence (an embedded interior NUL that the
  NUL-terminated C classifiers truncate at) was found and fixed offline so the Rust mirrors C exactly.
- **WAV/RIFF header parse** (`media/wav.c` `wav_parse_header`, seam `wav_parse_header_rs`, build 823):
  the verbatim C `wav_create` RIFF walk is already bounded (every `fmt`/`data` field read is guarded by
  `body+N<=size`, and the `data` chunk size is clamped to the buffer) -> **no reachable OOB** -> no
  advisory. Defense-in-depth (slice of exactly `len`, `checked_add` chunk advance removes the raw-
  pointer-walk class by construction).
- **PEM base64 decode** (`net/tls/cert_store.c` `base64_decode`, seam `cert_base64_decode_rs`, build
  823): the verbatim C decoder is already output-bounded (its `written < out_len` loop guard) -> **no
  reachable OOB** -> no advisory. Defense-in-depth (structural slice bound). It DOES carry a minor
  benign **CWE-190-class signed-int left-shift UB** (`acc = (acc << 6) | val` on a signed `int`, fires
  on every real certificate); only the low <=7 bits are ever read, so on the current build it is
  harmless, but the Rust wrapping-`u32` accumulator makes it **well-defined** and byte-identical. Not
  memory corruption, so not an advisory (minor hardening only).
- **SSH binary-packet framing** (`net/ssh/ssh_transport.c` `ssh_recv_packet`, build 824): a batch-3 seam
  was prepared for the `size_t payload_len = packet_len - 1 - pad_len` underflow (pad_len an unchecked
  attacker byte -> huge size_t -> clamped memcpy over-read of the 35000-byte stack buffer). It is
  ASan-provable **in isolation**, BUT on integration `ssh_transport.c` was found to be **dead
  scaffolding**: the Makefile compiles only `net/ssh/ssh2.c` + `ssh2_server.c` (the live SSH), and
  `ssh_recv_packet` has **zero callers** (only a declaration in `ssh.h`). The LIVE `ssh2.c`
  `ssh2_extract()` already rejects the pad underflow in BOTH branches via a **signed-int**
  `int payload_len = (int)packet_length - pad - 1; if (payload_len < 1) return -1;`. So the underflow
  is **NOT reachable** in the shipping kernel -> **no advisory, no seam integrated** (assessed-clean,
  dead code). Retargeting the seam to `ssh2.c` would be a new unverified port and batch-3 stops the
  port; not done. Honest correction of the batch-3 seam agent's stale-code assumption.
- **ed25519 point-decode** (`crypto/ed25519.c` `unpack25519`, seam `unpack25519_rs`, build 824): the
  32-byte compressed-point y-decode is a fixed-size read (`n[0..32]` -> `gf[16]`); the C is already
  bounded (no reachable OOB) -> **no advisory**. Defense-in-depth (Rust slices of exactly 32/16 remove
  the raw-index class). rs==c over edge + 20000 PRNG vectors at boot.
- **XDR decode primitives** (`net/rpc.c` `xdr_uint32`/`uint64`/`opaque`/`bytes`/`string`/`string_len`/
  `skip`/`nfs_fh3` decode branches, seams `xdr_decode_*_rs`, build 824): the untrusted input is
  XDR-encoded RPC/NFS server replies. On this 64-bit target every opaque/string length is a u32, so
  `(len+3)&~3` cannot wrap and `pos+aligned>size` correctly rejects -> the C decode is **source-bounded,
  NO reachable OOB** -> **no advisory**. Defense-in-depth (Rust `checked_add` removes the align round-up
  overflow class by construction). NOTE: the source-bounded XDR seam does NOT confine the SEPARATE
  destination over-write in `nfs.c` (MAYTERA-SEC-2026-0012), which is fixed directly in `nfs.c`.

## Summary

- **12 reachable memory-safety vulnerabilities found and closed** during the Rust port. Precisely:
  all 12 were **found** by the port (its differential harness or the reading it forced), and **10
  are closed by a Rust seam**. **Two are closed by a plain-C fix** and the Rust is not load-bearing
  for either: **0001** (the #476 ext2 guards, which landed before the fold; the seam mirrors them)
  and **0012** (the `nfs.c` `data_len` clamp, which the source-bounded XDR seam does not cover).
  "Closed by the Rust port" was previously stated for all 12; that over-credited the Rust on those
  two. See **Corrections**.
- By class: **6 out-of-bounds writes** (0003 ELF, 0004 FAT, 0005 PNG, 0006 JPEG, 0008 HTTP-chunked,
  0012 NFS). NOTE: 0006's DHT write is out of bounds of `huff_vals[]` but stays **inside** the
  enclosing allocation in the pre-port original (intra-object corruption, ASan-silent); the other
  five leave their allocation. **5 out-of-bounds reads** (0001 ext2, 0002 DHCP, 0007 TLS, 0009
  AAC/M4A, 0011 xattr), and
  **1 NULL-pointer dereference / OOB read** (0010 HTTP/2, CWE-476 rooted in CWE-125). 0003/0005/0006/0008
  pair an integer overflow (CWE-190) with the write.
- By reachability: **4 remote** (0007 TLS pre-auth, 0008 HTTP-chunked, 0010 HTTP/2 pre-auth, 0012 NFS -
  a malicious NFS server the client mounts + reads), 1 network-LAN (0002 DHCP), 2 untrusted-image
  (0005 PNG, 0006 JPEG), 5 local file/disk (0001 ext2, 0003 ELF, 0004 FAT, 0009 AAC/M4A, 0011 xattr -
  0004/0009/0011 reachable from Ring-3 via a syscall: readdir, play-audio, and get/listxattr).
- Every one is witnessed against the C reference and confined by the fix; each has a C hardening
  ticket. The witness is ASan for most, a **guard page** for 0006 claim 2 (ASan cannot see it), a
  **page-fault error code** for 0008's write claim (ASan reports only the read range), a NULL-deref
  for 0010 (the live `payload` is NULL when `flen==0`), and, for **0001 only**, a **labelled
  reconstruction**, because no artifact of the pre-#476 code survives. See **Corrections**.
  NOTE: 0012 is NOT a Rust-seam confinement - it is a DESTINATION over-write in `nfs.c` that the
  (source-bounded) XDR Rust seam does not cover, so it is fixed directly in C (clamp `data_len` to
  the destination `count`).
- 0001-0008 are fixed in the current golden (build 817); **0009 is fixed in build 819**,
  **0010 in build 822**, **0011 in build 823**, and **0012 in build 824** (folded to golden in the
  build-824 fold). Users on builds below the "Patched in" column for a given advisory should update.
- Defense-in-depth ports that are NOT advisories but still remove a bug class by construction now
  include the JPEG dequant+IDCT seam (build 822, carries a reachable CWE-190 signed-overflow UB the
  Rust well-defines; plain-C hardening = #507), the theme-file line tokenizer (build 822), the
  WAV/RIFF header parse (build 823), the PEM base64 decoder (build 823, whose Rust wrapping-u32
  accumulator additionally well-defines a benign CWE-190-class signed-shift UB), and the batch-3
  ed25519 point-decode + XDR decode primitives (build 824). Batch-3 also assessed an SSH framing
  underflow as **not reachable** (dead scaffolding `ssh_transport.c`; the live `ssh2.c` already rejects
  it) -> no advisory, no seam integrated. **Build 824 is the last parser-tier Rust batch;** the kernel
  Rust port stops at the parser tier after this fold.
</content>
