# MayteraOS Security Advisories

> **ID COLLISION RESOLVED 2026-10-02, AND THE RULE THAT CAUSED IT IS NOW GATED.** `0015`, `0016` and
> `0017` each named TWO different findings for a month. Each now names exactly one: the JULY meaning,
> which has been published on maytera.net since July. The September capability batch, which had taken
> `0015`-`0023` and had never left the project, is renumbered to `0030`-`0038`.

WHAT HAPPENED, stated because the fix has to address the cause and not only the symptom. The rule in
force was "never number from the website, always take max+1 from THIS file". That rule is right in
form and it was followed. It still minted three colliding IDs, because three IDs had already been
issued in July, in `CHANGELOG.md` and in code comments, and were never added here: this file's max
read `0014` when the real max was `0017`. **Taking max+1 from a ledger that is not the issuing point
mints a colliding ID, and obeying the rule is no protection.** The rule was insufficient, not
disobeyed.

| ID | Kept meaning (July 2026, PUBLISHED) | September 2026 meaning, RENUMBERED TO |
|---|---|---|
| 0015 | predictable DHCP transaction ID, CWE-330 (#495/#521; `net/dhcp.c`) | `SYS_GET_KEYBOARD` ungated keystroke theft -> **0030** |
| 0016 | syscall pointer-validation chokepoint, CWE-284 (#500/#503; `security/validate.c`, `rustkern/argtab.rs`) | `SYS_INJECT_KEY` ungated input injection -> **0031** |
| 0017 | TLS 1.3 CertificateVerify live adversarial verification, **not a defect** (#510; `net/tls/tls.c`, `rustkern/certverify.rs`) | legacy raw-index TCP cross-process break -> **0032** |

Renumbering the September batch rather than the July IDs is the lower-damage direction: a public
identifier cannot be withdrawn, and B's numbers had never left the project. `0018`-`0023` moved with
it to `0033`-`0038` so the batch stays one contiguous run. The old `0018`-`0023` are recorded below
as WITHDRAWN and must never be re-issued. The renumber was applied OCCURRENCE BY OCCURRENCE across 36
files, each one classified by reading its surrounding comment, because a blind substitution over the
ID strings would have rewritten the public July identifiers, which is the one outcome that cannot be
undone.

THE MECHANISM FIX, so a recurrence fails loudly instead of shipping:

1. **This file is now the COMPLETE issuing ledger.** July's `0015`-`0017` and the six PUBLISHED
   availability advisories `0024`-`0029` have entries below, so `max+1` from this file is finally
   safe. Nothing is issued anywhere else, and an ID that appears anywhere else without an entry here
   is now a build failure rather than a latent collision.
2. **`tools/advisory-id-gate/advisory-id-gate.py` enforces it**, wired into `build/repo-guard.sh` and
   listed in `build/selftest-census.policy`. It fails if any `MAYTERA-SEC-2026-NNNN` cited anywhere
   in the tree is absent from the ledger block below, if the ledger has a duplicate ID or a gap in
   the sequence, if a WITHDRAWN ID is still cited, or if a citation sits nowhere near any of that
   ID's registered anchor terms. That last check is the collision detector: in September it would
   have fired on the first `0015` comment written next to `SYS_GET_KEYBOARD`, because `0015`'s
   anchors are the DHCP ones.
3. The one-command check is still worth running by hand before minting an ID:
   `grep -rho "MAYTERA-SEC-2026-[0-9]\{4\}" . | sort -u | tail -3`.

**THE NEXT FREE ID IS `0052`.** (It read `0051` until 2026-10-07, when that day's review issued
`0051` for the SMB pre-authentication finding and moved this sentence IN THE SAME COMMIT, which is
the only discipline that keeps it true; re-derived that day two ways, `max+1` over the ledger block
below and `tools/advisory-id-gate/advisory-id-gate.py`, which before the issue reported "ledger: 50
IDs (44 LIVE, 6 WITHDRAWN); 219 citations in the tree" and exit 0. It read `0050` until 2026-10-06. That sentence was correct when
written and went stale on 2026-10-05, when that day's review issued `0050` and did not move it, which
is the one sentence in this file that must never be stale. Re-derived on 2026-10-06 three independent
ways, all three landing on `0051`: `max+1` over the ledger block below, an independent sweep of every
`MAYTERA-SEC-2026-NNNN` string in the whole tree, and `tools/advisory-id-gate/advisory-id-gate.py`,
which reports in one line "ledger: 50 IDs (44 LIVE, 6 WITHDRAWN); 219 citations in the tree; NEXT
FREE ID = ... `0051`" and exits 0. (The ID is written in short form here ON PURPOSE. Writing the
next free ID out in full, even in a quotation of the gate's own output, makes this sentence a
CITATION of an unissued ID, and the gate fails the build for it: "CITED BUT NOT ISSUED ... An ID
minted outside the register is exactly how the 0015/0016/0017 collision happened". That refusal was
observed on 2026-10-06 while writing this very paragraph, so the mechanism is working, and the house
style for an unissued ID is the short form.) **Run the gate rather than reading this sentence**: the
gate cannot go stale, and a reader who trusts the prose inherits whatever the last pass forgot. The
public page was NOT used, and must never be: on 2026-10-02 numbering from the page's highest row
produced a six-way collision.)

**PUBLISHED 2026-10-02: `0030`-`0037` are now on maytera.net/security.** The renumber unblocked them
and the eight rows went up the same day, as **Local** access-control rows. They deliberately do NOT
carry the page's `adv-row-remote` styling, which every one of `0024`-`0029` does, because none of the
eight is remote: each needs a program already running on the machine. The page's gap note, which said
eight closed findings were recorded internally and not published, was deleted in the same edit,
because publishing them made it false.

**ISSUED AND PUBLISHED 2026-10-02, later the same day: `0039`-`0041`**, three AVAILABILITY
advisories out of the #245 browser work. All three are closed and shipped, all three are **remote
and unauthenticated from an ordinary web page**, and all three therefore DO carry the page's
`adv-row-remote` styling, unlike `0030`-`0037` directly above. They also close a gap the page had
been carrying in writing: its Notes list ended with an admission that parsing deeply nested HTML,
and building a deep tree through the DOM API, each saturated the CPU on its own, that these were
separate defects from `0029`, and that no advisory was filed because nothing was fixed. Both halves
are now fixed, so that note is gone and three rows stand where it was. **Taking the gap note down
was not optional**: leaving a published note saying a defect is open, after it is closed, is the
same class of inaccuracy as the drift this register exists to prevent, pointing the other way.

**THE PAGE WAS REWRITTEN ON 2026-10-05 AT THE OWNER'S INSTRUCTION, AND THE TWO THINGS THIS
PARAGRAPH USED TO TELL YOU TO RECONCILE NO LONGER EXIST.** The page is now ONE searchable, filterable
table sorted newest-fix-first, plus a short "Report a bug" section and a short "How to update"
section. **The summary box and the Notes section were deliberately DELETED**, along with the
hardening essay, the methodology paragraph and the per-row narrative about how each defect was found.
They are not to be reinstated. The previous version of this paragraph instructed the next reviewer to
recount "its summary box, its by-reachability breakdown and its how-to-update build list" and to read
an arithmetic note "in its Notes"; every one of those is gone, and an instruction to reconcile a
deleted element is the same drift this register exists to prevent, pointing the other way. What is
left to reconcile is exactly two things: **the row count stated in the page's own prose, and the
table**. New rows are ONE LINE in the style of those already present.

MEASURED 2026-10-07 by parsing both files rather than by counting `<tr>` lines or trusting this
paragraph:

- the page's table holds **43 rows** (`0001`-`0016`, `0024`-`0037`, `0039`-`0041`, `0042`-`0051`),
  43 of them carrying distinct IDs, and the page's prose says "43 advisories to date" in both places
  it states a count. The table is also sorted strictly newest-fixed-in first, checked by parsing the
  `Fixed in` column rather than by eye, and `0051` is now the top row at build 2581;
- the ledger block below holds **51 IDs**, contiguous `0001`-`0051` with no duplicate, of which
  **45 are LIVE** and **6 are WITHDRAWN** (`0018`-`0023`);
- **no page row lacks a LIVE ledger entry**, and the only two LIVE IDs that are not rows are the two
  documented note-not-row exceptions, `0017` (not a defect) and `0038` (a capability the project
  added, not a defect). 45 LIVE minus those 2 is 43, which is the table's row count. Both are
  explained in prose on the page, as is the `0018`-`0023` hole, so the public sequence has no
  unexplained gap;
- severity split on the page is 31 High, 10 Medium, 2 Low = 43;
- `adv-row-remote` **is still present and was not replaced by anything**: the class appears on 18
  rows, the `data-remote` attribute that drives the filter control says `yes` on 18 rows, and the two
  agree row for row on all 43. Both mechanisms are live; use both on a new remote row. `0051` was
  given both, and the served bytes were re-parsed afterwards to confirm they still agree row for row.

Also MEASURED, because the rewrite is the kind of edit that loses data quietly: every one of the 41
advisory IDs present on the pre-rewrite page is present on the rewritten one. **No advisory data was
lost in the rewrite.**

**The next daily review should re-derive all of the above rather than inherit it from this
paragraph**, which is the mistake that produced both halves of the original drift, and which produced
the stale sentences this 2026-10-06 pass had to correct.

FIVE CLASSES OF FINDING LIVE IN THIS FILE, and they are not interchangeable.

1. **Rust-port memory-safety findings** (`MAYTERA-SEC-2026-0001` to `0014`), the original scope:
   reachable bounds defects found and closed during the incremental Rust kernel port (#404). The
   rules for that class are immediately below and are unchanged.
2. **Access-control and privilege-boundary findings** (`0030` onward), added 2026-09-04 with the
   capability API's Stage 0. These corrupt nothing; each is a missing authorization check on a
   correctly-written syscall. They have their own section, their own severity basis and their own
   evidence rules, because a memory-safety rulebook does not describe them: there is no crafted
   input, no ASan witness and no crash, only a caller doing something it should never have been
   able to ask for.

3. **July 2026 findings that were minted OUTSIDE this file** (`0015`, `0016`, `0017`), added here
   2026-10-02 as part of closing the collision. They are neither Rust-port bounds defects nor
   capability-API gaps: `0015` is a randomness defect (CWE-330), `0016` is the syscall
   pointer-validation chokepoint (CWE-284), and `0017` is **not a defect at all**. They are recorded
   because this file is the issuing ledger, and a ledger missing three issued IDs is what produced
   the September collision.
4. **Availability / resource-exhaustion findings** (`0024`-`0029`, then `0039`-`0041`), the #245
   class: a crafted input causes unbounded or grossly disproportionate work. Nothing is corrupted
   and nothing is disclosed; the machine or the process stops doing useful work. `0024`-`0029` were
   PUBLISHED on maytera.net before they were recorded here, which was the gap in the other
   direction. `0039`-`0041` were issued HERE first, which is the order the gate now enforces, and
   they are the first three in this class whose fix commit is bracketed against the golden archive
   rather than taken from the published page.

5. **Data-protection, integrity and authentication findings** (`0042`-`0044`), added 2026-10-03. The
   common thread is that **a protection the system asserts did not hold, or data the user never asked
   to lose was destroyed.** No memory is corrupted, no authorization check is missing from an
   otherwise-correct syscall, and nothing is amplified. The evidence bar is a MEASURED
   before-and-after on a running system with the quantity that matters counted: bytes kept or lost,
   or the identity actually negotiated. AddressSanitizer is meaningless for this class and is not
   claimed. Two of the three carry an acknowledged open residual.

Numbering is one shared sequence in discovery order across all five classes. `0018`-`0023` are
WITHDRAWN (see their section below) and must never be re-issued.


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
  md5-identical on both trees (local + the build container).
- **ASSIGNING AN ID: max+1 FROM THE LEDGER BLOCK BELOW, AND NOWHERE ELSE** (2026-10-02, after the
  `0015`/`0016`/`0017` collision). An ID is ISSUED by adding a row to the ledger block in this file,
  in the SAME change that first cites it. Minting one in a CHANGELOG entry or a code comment and
  "adding it to the register later" is exactly what broke the sequence: later never came, this
  file's max read `0014` when the real max was `0017`, and the September batch collided while
  correctly obeying the max+1 rule. `tools/advisory-id-gate/advisory-id-gate.py` now fails the build
  on any ID cited in the tree that has no ledger row, so the order is enforced rather than asked for.
- **Every ledger row carries ANCHOR TERMS**, and a citation must sit within a few lines of one of
  them. That is what makes "one ID, one meaning" machine-checkable: two findings sharing a number
  cannot both match the same anchors.
- **A WITHDRAWN ID is never re-issued.** It keeps its row, marked WITHDRAWN with the ID it moved to,
  and the gate fails if it is still cited anywhere.

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
| MAYTERA-SEC-2026-0013 | JPEG Huffman table build (`gui/jpeg.c` `build_huffman`) | CWE-787 | High | **Reachable from Ring-3 via `sys_decode_image`** (`proc/syscall.c:3983`, the userland browser's `<img>` path), Files previews (`gui/thumbnailer.c:474`), imageviewer, filebrowser, ipp: a **crafted JPEG** whose DHT declares a **non-canonical** code-length table (e.g. `bits[0]=255`, i.e. 255 one-bit codes where the code space allows at most 2) drives `idx_fast = (code << (10-len)) \| f` past `huff_fast[dc][idx]` (`[1024]` of `int16_t`). Measured on an exact-size allocation: writes indices 0..130559 = **259,072 bytes past the array, 252,736 bytes past the 9760-byte `jpeg_decoder_t`**, with attacker-chosen `vals[k]` in the low byte of each entry. **LIVE under the shipped `-DRUST_JPEG`**: `jpeg_dht_rs`'s only gate was `total > 256`, and a count-sum of 255 passes it, so the seam ACCEPTED the table and handed it straight to the write. ASan-proven end-to-end through the real shipped seam (`jpeg_parse_headers_rs ret=0 ACCEPTED` -> `heap-buffer-overflow WRITE of size 2 ... 0 bytes to the right of 9760-byte region`). NOT extraction drift: `build_huffman` is byte-identical to the pre-extraction original (md5 `903798ad...`); b814 moved it just OUTSIDE the seam boundary so it was never ported, which NARROWED the reach (the original fired it during `parse_dht` with no SOS needed) but left it live | **plain-C canonicality + `k >= 256` bound in `build_huffman`** (flag-independent); `jpeg_dht_rs` tightened to match so both layers agree | #518 | <= build 825 | **build 826** |
| MAYTERA-SEC-2026-0014 | IPv4 receive path (`net/ip.c` `ip_handle`) | CWE-191 -> CWE-125 | High (remote, LAN) | **Remote, unauthenticated, from any host on the LAN** (no IP address required: `our_ip == 0` DHCP mode accepts any frame, and a broadcast destination is accepted regardless). `ip_handle` guarded `ihl <= length` and `total_length <= length` but **never `total_length >= ihl`**, so `uint16_t payload_length = total_length - ihl` UNDERFLOWED. A crafted **60-byte** frame (IHL=15 => `ihl=60`, `total_length=20`) yields `payload_length = 65496`, which is passed to **every** registered protocol handler. MEASURED by driving the real verbatim `ip_handle`: `icmp_handle`, `udp_handle` AND `tcp_handle` are each CALLED with `len=65496` from a 60-byte frame. ASan-witnessed over-reads in the real handlers on an exact-size 60-byte heap frame: `udp_handle` READ size 2 (4 bytes right of the region), `tcp_handle` prologue READ size 1 (12 bytes right). Propagation is real, not theoretical: with the bytes following the frame in the driver's fixed RX buffer under attacker influence (stale bytes from a previous packet), a bound UDP consumer (DHCP client port 68, DNS 53) is handed **`data_len=65488`** and the TCP prologue reaches **`payload_len=65476`**. This also widens MAYTERA-SEC-2026-0004's (dhcp) reach. The ICMP arm was confined only by accident of `-DRUST_ICMP`'s `ICMP_MAX_LEN`; the UDP/TCP arms were **not confined at all** (`net/ip.c` is unported) | **plain-C `if (total_length < ihl) return;` in `ip_handle`** (one line; fixes all three protocols at the root, flag-independent) | #517 | <= build 825 | **build 826** |

## Access-control and privilege-boundary advisories (capability API Stage 0)

**This section is a DIFFERENT CLASS from the table above, and the distinction matters when reading
it.** Everything above is a memory-safety defect found during the Rust port: a bounds bug that
corrupts or over-reads memory. Nothing below corrupts anything. Every one of the eight is a
**missing access-control check on a correctly-written syscall**, and each was reachable by an
ordinary unprivileged Ring-3 application with **no capability, no consent, no audit record and no
visible indicator**. A fuzzer would never find them, because nothing crashes: the kernel does
exactly what it was asked, for a caller that should never have been able to ask.

The FIVE Stage 0 findings (`0030`-`0034`) were discovered by the inventory in
`docs/SYSTEM_CAPABILITY_API.md` section 1 (code reading, 2026-09-04, dev @ `bbebc5a0`), re-verified
against `dev` @ `e860a883` before the fix, and **demonstrated on a booted VM** by
`userland/apps/caphole` (see "Evidence" below). Closed by that document's Stage 0. `0035`, `0036` and
`0037` were found and closed by Stages 1, 2 and 3 of the same document and arrived in this table
later, each with its own evidence block. **So where this section says "these five" it means Stage 0,
and the table's eight rows span four stages;** the stage table near the end of the section says which
is which. That drift is worth naming rather than silently editing: the prose was written when the
table held five rows and was not revisited when three more were appended to it.

Severity here is weighted by what the defect lets one application do to **another application or to
the user**, not by memory corruption. "Local" means a Ring-3 process already running on the machine:
that is the whole precondition, and on a desktop OS it is a low bar (any app the user installs, any
app the AI layer is asked to run).

| ID | Component (file) | Class (CWE) | Severity | Reachability / attack vector | Fixed by | Affected builds | Patched in |
|---|---|---|---|---|---|---|---|
| MAYTERA-SEC-2026-0030 | `SYS_GET_KEYBOARD` (195) dispatch (`proc/syscall.c`) | CWE-862 missing authorization -> CWE-522 credential exposure | **High** | **Local, and it defeats the only kernel-enforced consent gate the system has.** The syscall is a **destructive drain** of the global cooked key ring and was gated on nothing but `g_win16_owns_screen`. Any Ring-3 process could both **read and steal every keystroke in the system**. #745's entire trust story (`proc/elevate.h`) is that a requesting app "never draws anything, never receives a keystroke and never learns the password", because the **compositor** draws the elevation prompt; but the compositor reads that prompt's keys through this syscall (`compositor/main.c:825` -> `elevate_handle_key`), so **the password was readable by the very app that raised the prompt**. The lock screen uses the same loop and was exposed identically. `compositor/main.c:854` already said "a trusted prompt whose keystrokes are also delivered to the app that raised it is worth nothing"; that check was in the compositor, and an app simply does not go through the compositor | Dispatcher gate to the compositor principal via `capgate_is_compositor_rs()` (`rustkern/capgate.rs`). Refuses with **-13 (EACCES)**, deliberately distinct from the ordinary "ring empty" -1, so the refusal is observable from Ring 3 | <= build 2391 | **build 2392** |
| MAYTERA-SEC-2026-0031 | `SYS_INJECT_KEY` (197) dispatch (`proc/syscall.c`) | CWE-862 missing authorization -> CWE-1021 UI redress | **High** | **Local.** Posts a synthetic `EVENT_KEY_DOWN` to the focused window with no privilege, ownership or capability check, while its **matched pair** `SYS_INJECT_MOUSE` (214) has been gated with `is_compositor()` since it was written (`gui/fb_syscall.c:1064`) and the header describes **both** as compositor-only. Two consequences: (a) it **forges elevation input credit**, because `sys_elev_request()` refuses with `ELEV_ENOINPUT` unless the WM recently delivered a **real** input event to a window the requester owns, and that stamp is written for `EVENT_KEY_DOWN`; an app that focuses its own window could inject a key, stamp its own credit, and **raise a password prompt the user never asked for**; (b) the event lands on whatever window has focus, so it drives **other applications' UI** | Same dispatcher gate. Refuses with -1, which is unambiguous here because the pre-fix case had **no failure path at all** and always returned 0 | <= build 2391 | **build 2392** |
| MAYTERA-SEC-2026-0032 | legacy raw-index TCP family (`net/tcp.c` `tcp_get_conn`, dispatched via `proc/syscall.c` `tcp_*_kcr3`) | CWE-863 incorrect authorization | **High** | **Local, cross-process confidentiality AND integrity break.** `tcp_get_conn()` checked only `0 <= sock < 64` and `conn->active`. `owner_pid` was stamped on every connection (`tcp.c:447`) and read by **nothing except the Task Manager listing**. So a Ring-3 process could pass any index 0..63 to `SYS_SEND` (62), `SYS_RECV` (63), `SYS_TCP_CLOSE` (64), `SYS_CONNECT` (61), `SYS_TCP_STATE` (65), `SYS_LISTEN` (303) or `SYS_ACCEPT` (304) and **read, write or tear down another process's TCP connection, the in-kernel sshd's included**. A 64-entry table is a loop, not a guess. The #524 BSD family (`SYS_SOCK_*`) is per-process by fd, but wraps the same 64-slot table, so the raw-index family **bypassed that fd table entirely** | Ownership guard at the **Ring-3 chokepoint only** (the `static` `tcp_*_kcr3` wrappers, which the dispatcher alone reaches), using `capgate_owner_ok_rs()`. Deliberately **not** inside `tcp_get_conn()`, whose 13 callers include in-kernel consumers (`net/https.c`, `net/ftp.c`, sshd) that run where `proc_current()` is an unrelated victim process. New `owner_tgid` field so a sibling pthread of the opener is admitted. Refuses with a bare **-1**, deliberately NOT a distinct code, because a distinct code would be an oracle ("slot N is live but not yours") | <= build 2391 | **build 2392** |
| MAYTERA-SEC-2026-0033 | `sys_shm_map()` (`ipc/shm.c`) | CWE-862 missing authorization | **High** | **Local, cross-process memory disclosure.** The map path checked range, not-free, not-already-mapped-by-me and `SHM_FLAG_EXCLUSIVE`, and **no credential of any kind**. Any Ring-3 process could map any allocated region 0..63 and **read another process's memory**, and write it too unless `SHM_FLAG_READONLY` was set (which only downgrades non-creators to `VMM_USER_RO`). Again a 64-entry table is a loop | Creator-thread-group check via the same `capgate_owner_ok_rs()`, plus a `creator_tgid` stamp. **No share list was added**: a complete census found the only `shm_map` callers in the tree (`libc/compositor_client.c`, `apps/ipc_test`) each map a region they created themselves, so there is no cross-process consumer to serve, and principle 7 of the design ("a capability with no consumer becomes fiction") applies | <= build 2391 | **build 2392** |
| MAYTERA-SEC-2026-0034 | `/dev/` node open ordering (`proc/fdlayer.c` `sys_open_k`) | CWE-696 incorrect behavior order -> CWE-862 | **Medium** | **Local.** The `/dev/<name>` prefix was handled **before** the permission block and **returned**, so opening any device node **bypassed `perms_check()` entirely**. This was not a missing rule but a **structural exemption**, and it applied to every node the system registers now or later, not only to the USB serial adapter where it was noticed: `/dev/ttyACM0` (real read and write fops), `/dev/ptmx`, `/dev/pts/0..7`, `/dev/console`, `/dev/tty`, `/dev/null`, `/dev/zero`, `/dev/random`, `/dev/urandom`. An operator entry in `/CONFIG/PERMS.DB` for a device node parsed, stored, listed, and was **never consulted** | The `/dev` branch moved **after** `perms_check()`, with a signpost comment where it used to be. Paired with `perms_dev_node_seed[]` (`fs/perms.c`), a third seed table whose modes **preserve today's effective access** so no shipping app regresses | <= build 2391 | **build 2392** |
| MAYTERA-SEC-2026-0035 | `/SCREENSHOT.REQ` file-drop trigger (`userland/apps/compositor/screenshot.c` `screenshot_poll`) | CWE-862 missing authorization -> CWE-200 information exposure | **High** | **Local, whole-screen capture with no consent.** `screenshot_poll()` ran once per compositor frame, opened `/SCREENSHOT.REQ`, read an **attacker-supplied absolute output path** from the body, and captured the compositor's whole composited backbuffer, which contains **every window on screen** (any open document, any password field mid-entry), to that path. There was **no uid check, no capability, no consent, no audit record and no toast**. The trigger was a file any Ring-3 app could create in `/`, so any app could screenshot the entire desktop to a path of its choosing. The file's own header even noted the identical risk for `testhook.c` and gated `testhook.c` at **compile time** (`compositor/Makefile`, `ifdef TESTHOOK`); the screenshot trigger had **no equivalent gate and shipped in every build**, including the golden | Stage 1 of the capability API. The `/SCREENSHOT.REQ` file poll is **deleted** (not merely checked): `screenshot_poll()` now dequeues from a **kernel-mediated queue** (`SYS_SCREENSHOT_POLL`, compositor-only via `fb_owner_is`). The only Ring-3 way into that queue is `SYS_SCREENSHOT_REQUEST`, which the **dispatcher capability chokepoint** (`syscall_cap_check`) gates on a live `screen.capture` grant, and whose handler (`gui/shotq.c`) then binds the request to the **exact granted path** and re-applies `perms_check(W_OK)`. A `screen.capture` grant is obtained only through the compositor-drawn consent prompt (`sys_cap_resolve`, compositor-only) and is journalled as a `GFSJ_OP_EDGE_ADD` edge. Removing the ambient path, not just adding a check, is design rule 4: a check can be forgotten, a deleted trigger cannot | <= build 2392 | **build 2394** |
| MAYTERA-SEC-2026-0036 | Ambient USB-serial access via `/dev/ttyACM0` (`kernel/fs/perms.c` `perms_dev_node_seed[]`, seed `0666`) | CWE-862 missing authorization -> CWE-668 exposure of resource to wrong sphere | **Medium** | **Local, unmediated serial-adapter access.** After Stage 0 gave `/dev/` nodes a consulted permission policy, `/DEV/TTYACM0` was deliberately left seeded `0666` so no shipping app regressed, which means **any Ring-3 app could open and drive an attached USB CDC-ACM serial adapter** (a 3D printer, a microcontroller, a modem) with no capability, no consent and no audit. Serial is a bidirectional physical channel: read is data exfiltration from whatever is attached, write is arbitrary command of it. The kernel exposed the node with real read/write fops (`drivers/usb_cdc_acm.c`) and no ownership or capability check gated it | Stage 2 of the capability API. The seed is tightened to root-owned **`0600`** so the desktop uid (1000) is refused the raw open, and serial is re-issued only through a **mediated gateway** (`drivers/serialport.c`): `SYS_SERIAL_LIST` names published ports (a name + class, never a base address or `/dev` path), and `SYS_SERIAL_OPEN` opens one, gated by a **`serial.port`** grant at the dispatcher chokepoint (`syscall_cap_check`) and bound to the exact granted port name. A `serial.port` grant is obtained only through the compositor-drawn consent prompt and journalled as a `GFSJ_OP_EDGE_ADD` edge. Removing the ambient path (tightening the seed), not just adding a check, is design rule 4 | <= build 2396 | **build 2397** |
| MAYTERA-SEC-2026-0037 | Unasserted `/TESTINPUT.TXT` marker (`drivers/testinput.c`; `build/invariant-gate.sh`) | CWE-489 active debug code / CWE-1188 insecure default -> CWE-862 missing authorization | **Low** (build-time defense-in-depth, not a Ring-3 hole in a normal golden) | **The host->guest debug injection channel could be silently armable in a shipped image.** `drivers/testinput.c` injects synthetic keyboard/mouse directly into the hardware input paths (so injected input counts as REAL, stamping the elevation input credit), and it arms whenever `/TESTINPUT.TXT` is present on the ESP. That marker is absent from a normal golden, so the channel is off; but NOTHING asserted its absence (docs/SYSTEM_CAPABILITY_API.md 1.2), so a golden that accidentally carried the marker (a stray file on the asset base, a bad overlay) would ship an ungated injection channel with hypervisor-serial reach, sitting beside the new capability-gated input.inject. The channel is not reachable by an in-guest Ring-3 app (the kernel owns COM1; apps have no port I/O), which is why this is a build-integrity gap rather than a direct Ring-3 escalation | Stage 3 of the capability API. `build/invariant-gate.sh` now FAILS (RED) any image whose ESP carries `/TESTINPUT.TXT` (factored `check_no_testinput`, with `--self-test-testinput` proving it goes RED on a marker image and GREEN without). The golden cannot be produced without the gate passing, so the marker's absence is now enforced by construction, not by hope. Independently, Stage 3's input provenance (`INPUT_SRC_HW`/`INPUT_SRC_SYNTHETIC`) ensures even the LEGITIMATE input.inject path cannot manufacture input credit: a synthetic event never stamps `elev_last_input_ms`, so an app holding input.inject cannot inject the keystroke that approves its own next grant or elevation | <= build 2397 | **build 2398** |

**MAYTERA-SEC-2026-0035 evidence (Stage 1, measured on a booted VM, build 2394 / commit `e4d2c03e`).** `userland/apps/captest` (unshipped) demonstrates both arms on the running kernel. RED (ordinary app, no window, no user intent): `sys_cap_request` -> `CAP_ENOINPUT`, `sys_screenshot_request` -> `CAP_EDENIED`, `sys_cap_resolve(APPROVE)` from the app itself -> `CAP_EPERM` (an app cannot grant itself), `sys_cap_view` -> `CAP_EPERM`, query `held=0` throughout: "RED SUITE PASS: a grant cannot be obtained without consent." GREEN (window with real input credit -> the compositor's consent prompt approved with a real Enter delivered over the testinput channel): the kernel logged `[CAP] GRANTED: pid=34 uid=1000 screen.capture scope=/CAPTEST.BMP expires=171849ms edge=17` (grant issued on the uid-1000 requester, time-bounded, recorded as GraphFS journal edge 17 = `GFSJ_OP_EDGE_ADD`); the app's `sys_cap_query` returned `held=1`; the post-grant `sys_screenshot_request` passed the capability chokepoint (it reached `perms_check`, returning `-13 EACCES` on the write path, NOT `-3 CAP_EDENIED`); then `[CAP] REVOKED: pid=34 screen.capture edge=17` (`GFSJ_OP_EDGE_REVOKE`) and the next `sys_screenshot_request` was refused `CAP_EDENIED` at the chokepoint (revocation bit in flight). The boot self-test `[CAP] selftest OK` asserts the whole grant model + consent state machine on every boot; `[CAPGATE] selftest OK`; `[STAGE] DESKTOP_READY`; zero `[WQBLOCK]`; no kernel panic. The scope resolved to `/CAPTEST.BMP` (the harness's session home resolved to `/`), which is why the write hit `perms_check`; that is a filesystem-permission detail orthogonal to the capability gate, which opened correctly.

**CORRECTION (#permcreate / #shotwriter, 2026-09-24): that `-13` was NOT an orthogonal detail, and reading it as one cost two more months.** `perms_check(path, W_OK)` on a path that does not exist yet was answered from `perms_check_leaf()`'s no-entry default (root-owned 0755), which denies `W_OK` to **every** non-root uid. So `screen.capture` could never produce a file for a normal user, **whatever** capability the person granted: the gate opened onto a door that was nailed shut. The same `-13` reappeared verbatim on golden 2472 as `visiongame: FAIL screenshot request refused (-13 ...)` for uid 1000 writing into its own home. Two defects, both now fixed: (a) `rustkern/permpath.rs` decides a `W_OK` on a row-less name from the **parent directory**, which is what POSIX requires to create a file, strictly as an additional allow after the ordinary check has already denied; (b) `gui/shotq.c` additionally checks the **compositor's** identity, because the compositor is what actually writes the file, and returns `-14` (compositor cannot write there) or `-15` (no framebuffer owner) instead of letting a permission refusal surface as a 12-second timeout. An error code recorded in an evidence block as "orthogonal" is a finding nobody owns.

**MAYTERA-SEC-2026-0036 evidence (Stage 2, measured on a booted VM, build 2397 / commit `b083cd73`).** `userland/apps/sertest` (unshipped) demonstrates both arms and the ambient closure. **Ambient closed:** as the uid-1000 desktop principal, `open("/dev/ttyACM0", O_RDWR)` returned `-13` (EACCES) where the `0666` seed had let it succeed; the `/dev/` perms gate fired. **RED** (ordinary app, no window, no intent): `sys_serial_list` named 2 ports (`ttyACM0` class 2, `ttyS1` class 1 = COM2, a name + class only); `sys_serial_open("ttyS1")` with no grant -> `-3 CAP_EDENIED` at the dispatcher chokepoint; a spontaneous `sys_cap_request(serial.port,ttyS1)` -> `-4 CAP_ENOINPUT`; `sys_cap_request(serial.port,"nosuchport")` -> `-6 CAP_ESCOPE` (the kernel-owned enumeration check refuses a port it does not publish); a PATH-kind scope on a serial cap -> `-6 CAP_ESCOPE`; `sys_cap_resolve(APPROVE)` from the app itself -> `-10 CAP_EPERM`; query `held=0` throughout: "RED SUITE PASS". **GREEN** (window with real input credit -> the compositor's consent prompt approved with a real Enter over the testinput channel): the kernel logged `[CAP] GRANTED: pid=36 uid=1000 serial.port scope=ttyS1 ... edge=20` (issued on the uid-1000 requester, time-bounded, GraphFS journal edge 20 = `GFSJ_OP_EDGE_ADD`); `sys_cap_query` -> `held=1 scope=ttyS1`; `sys_serial_open("ttyS1")` -> fd `3` (the gate OPENED and installed a real fd); `sys_write(fd, 23 bytes)` -> **the 23 bytes `MAYTERA-SERIAL-CAP-OK\r\n` arrived on the VM's SECOND serial (COM2/ttyS1), captured on that socket** = positive proof the open reached hardware, not merely a return code; then revoke and the next `sys_serial_open("ttyS1")` -> `-3 CAP_EDENIED` (revocation bit in flight). The publish itself is measured: `[SERIALPORT] published 'ttyS1' (class 1, 16550 @ 0x2f8)` and `... 'ttyACM0' (class 2, USB CDC-ACM)` on every boot. **Boot regression: clean at 1, 2, 4 and 8 cores** - each reached `[STAGE] DESKTOP_READY` and `[CAP] selftest OK` (now asserting the serial cases too), with 0 `[WQBLOCK]` and 0 kernel panics. Stage 1's `captest` (screen.capture) still passes red-then-green on the same image (no regression): grant edge 31 issued, `sys_screenshot_request` reached `perms_check` (not the chokepoint), revoke refused it again.

**MAYTERA-SEC-2026-0037 evidence (Stage 3, input.inject, measured on a booted VM, golden build 2398 kernel / commit `6de46840`, VM 2599 booting the golden over USB, driven by the testinput channel armed only on this throwaway image).** `userland/apps/injtest` (unshipped) demonstrates all three properties, and captest/sertest are re-run for regression. **RED** (ordinary uid-1000 app): `sys_cap_inject_key`/`sys_cap_inject_mouse` with no grant -> `-3 CAP_EDENIED` at the dispatcher chokepoint; the raw compositor-only `SYS_INJECT_KEY` (197) -> `-1` (no ambient inject path for a non-compositor); `sys_cap_request(input.inject, PATH kind)` and `(input.inject, WINDOW "other")` -> `-6 CAP_ESCOPE`; spontaneous `sys_cap_request(input.inject, self)` -> `-4 CAP_ENOINPUT` (with the durable `[CAP] REFUSED spontaneous request: pid=35 input.inject (no input in 10000ms)`); `sys_cap_resolve(APPROVE)` from the app -> `-10 CAP_EPERM`: "RED SUITE PASS". **GREEN** (window with real input credit -> compositor consent approved with a real Enter over testinput): `[CAP] GRANTED: pid=35 uid=1000 input.inject` (scope=self, journalled edge); `sys_cap_query -> held=1 scope=self`; a synthetic key injected into the app's OWN window was read back as `EVENT_KEY_DOWN` keycode 0x5A (`gotk=1`, injection WORKS); after `sys_cap_revoke` the next `sys_cap_inject_key` -> `-3 CAP_EDENIED` (revocation bit in flight). **THE SELF-ESCALATION PROOF** (the point of the stage): after the grant, the app waits a TSC-wall-clock-guaranteed >=18s so the real input credit expires (a BASELINE `sys_cap_request(screen.capture)` with NO injection then returns `-4 CAP_ENOINPUT`, confirming the credit is gone), then injects 5 synthetic keys into its own window and immediately requests another capability: `sys_cap_request(screen.capture) = -4 CAP_ENOINPUT` and `sys_elev_request = -8 ELEV_ENOINPUT` -> "SELF-ESCALATION PASS". A synthetic event does not stamp `elev_last_input_ms` (the one stamp site, `syscall.c` `user_window_queue_event`, is gated on `INPUT_SRC_HW`), so an app holding input.inject cannot inject the keystroke that credits its own next grant or elevation. Separately, while the BASELINE prompt was open the follow-on injection returned `-2 CAP_EBUSY` (consent-surface guard: an app cannot inject while a prompt is up), and the app cannot inject the Allow for its OWN input.inject prompt (it holds no grant yet). **Boot regression: clean at 1, 2, 4 and 8 cores** - each reached `[STAGE] DESKTOP_READY`, `[CAP] selftest OK`, `[CAPGATE] selftest OK`, `N of N cores online`, with 0 `[WQBLOCK]` and no kernel panic (the 6 `PANIC` log lines are the benign `PANIC_LOG_ARM` pre-allocation of `/boot/PANIC.TXT`, not a crash). **No regression:** captest (screen.capture) passes RED+GREEN at 1 core (grant edge issued, `sys_screenshot_request` reached `perms_check` returning `-13`, revoke refused it `-3`); its R5 was updated from input.inject (now issuable in Stage 3) to net.connect (still `-7 CAP_EPOLICY`). sertest (serial.port) passes RED+GREEN at 2 cores (ambient `/dev/ttyACM0` -> `-13`; grant edge issued; `sys_serial_open(ttyS1) -> fd 3`; 23 bytes `MAYTERA-SERIAL-CAP-OK` written; revoke refused it `-3`). The `/TESTINPUT.TXT` invariant-gate check is proven with `build/invariant-gate.sh --self-test-testinput` (RED on an ESP carrying the marker, GREEN without), and the shipped golden carries no marker.


**MAYTERA-SEC-2026-0038 (Stage 4, CAP_SCOPE_WINDOW_TARGET, consented CROSS-APP input.inject).** Extends
input.inject (Stage 3, self-scoped) with a scope that lets a consented holder inject synthetic keyboard/
mouse into ONE specific window it does NOT own, named by the human at consent time. The grant binds to the
target window's STABLE, monotonic, never-reused id (`user_window_t.win_id`), not the reusable slot handle,
so a closed-then-reused slot can never be driven by a stale grant; `cap_covers_window_target()` is an EXACT
id match (the cross-app twin of `cap_covers_port`). Consent names the target by its kernel-authored title
(`userwin_target_resolve()` reads the window's own title; the app supplies only the handle). Every Stage 3
protection is preserved across the app boundary: an injected event is `INPUT_SRC_SYNTHETIC` for ANY target
and so never stamps `elev_last_input_ms` (the one stamp site, `user_window_queue_event`, is gated on
`INPUT_SRC_HW`), so a holder driving app B cannot manufacture a consent/elevation for itself; the temporal
guard (`cap_inject_guard`) refuses all synthetic input while any consent/elevation prompt is open or the
session is locked; the compositor's own surface is refused as a target at bind and at inject; the grant is
time-bounded and revoked in flight. EVIDENCE (measured on a booted VM, golden build 2399, kernel commit 8dceace9 = the capstage4 kernel; the injtest2 harness is a kernel-identical userland follow-up rebuilt from eb685963 and overlaid onto build 2399, so the kernel under test is exactly the landed one). VM 2777 boots the golden over USB with the testinput channel armed only on the throwaway image; uid-1000 session. `userland/apps/injtest2` (unshipped) spawns itself as a second app (argv "target") which publishes its window handle and echoes every key its per-window queue delivers; the injector runs the red-then-green. **RED** (uid-1000 app, no grant): cross-app `sys_cap_inject_key` into app B's window -> `-3 CAP_EDENIED` at the dispatcher chokepoint; `sys_cap_request(input.inject, WINDOW_TARGET, "self")` -> `-6 CAP_ESCOPE` (non-decimal noun); `sys_cap_request(input.inject, WINDOW_TARGET, "99")` -> `-6 CAP_ESCOPE` (not a live targetable window): RED PASS. **GREEN** (window A credited by a real key over testinput, then the compositor consent approved with a real Enter): the prompt is named from the KERNEL-authored title, `[CAP] prompt raised: pid=33 input.inject scope=1:InjTarget` (the app supplied only the target HANDLE; the kernel resolved it to the window's stable id and its own title "InjTarget"); verdict GRANTED, `edge_seq` journalled. **THE HEADLINE PROOF:** five synthetic 'Z' (0x5A) keys injected from process A into app B's window were each received and echoed BY APP B - `[INJTGT] RECEIVED KEY 0x5A in MY window (cross-app injection delivered)` (22 such echoes across the run) - a keystroke that originated in process A appearing as input in a second app's window. Cross-app mouse-down into B also accepted (=0). **REFUSALS, each demonstrated:** (a) cannot inject OUTSIDE the granted target - injecting into the injector's OWN window A while holding only the WINDOW_TARGET(B) grant -> `-6 CAP_ESCOPE`; (b) cannot inject the Allow - with its own consent prompt open and no grant held, its attempt to inject the Allow into B -> `-3 CAP_EDENIED` at the chokepoint (strictly stronger than the temporal guard: no grant, no inject at all); (c) TEMPORAL GUARD across the app boundary - while HOLDING the B grant and with a second consent prompt open, injecting into the granted target B -> `-2 CAP_EBUSY`, so a holder can never race an Allow or drive the machine during consent. The compositor's consent surface and the lock screen are drawn by the compositor and are NOT entries in `user_windows[]`, so no WINDOW_TARGET handle can name them; `userwin_is_compositor_owned()` additionally refuses a compositor-owned target at bind and at inject. **THE SELF-ESCALATION PROOF, across the app boundary** (the point): after A's real input credit expired (a >=18s TSC-wall-clock wait; a BASELINE `request(screen.capture)` with NO injection then returned `-4 CAP_ENOINPUT`, confirming the credit was gone), the injector drove app B with 5 synthetic keys (B echoed all 5) and immediately requested another capability: `request(screen.capture) = -4 CAP_ENOINPUT` and `sys_elev_request = -8 ELEV_ENOINPUT`. Driving another app's window CANNOT manufacture the input credit A's own next consent/elevation requires, because a synthetic event is `INPUT_SRC_SYNTHETIC` for ANY target and never stamps `elev_last_input_ms` (the one stamp site, `user_window_queue_event`, is gated on `INPUT_SRC_HW`). **REVOKE in flight:** `sys_cap_revoke(input.inject)` -> held=0; the next cross-app inject into B -> `-3 CAP_EDENIED`. The grant binds to the window's STABLE monotonic id (`user_window_t.win_id`), not the reusable slot handle, so a closed-then-reused slot can never be driven by a stale grant. **Boot: clean at 1, 2, 4 and 8 cores** - each reached `DESKTOP_READY`, `[CAP] selftest OK` (now including the Stage 4 scope-shape / exact-id-cover / self-vs-target-scope-kind self-test cases 80-96), `[CAPGATE] selftest OK`, N of N cores online, with 0 `[WQBLOCK]` and no kernel panic (the 5 `PANIC` log lines are the benign `PANIC_LOG_ARM` pre-allocation of `/boot/PANIC.TXT` + `/boot/STAGE.TXT`, not a crash). **No regression:** the caps boot self-test (requirement table, policy, scope validation, grant issue/find/cover/consume/revoke, table-full, consent SM) passes; injtest (Stage 3 self-scope) still passes red-then-green (RED: query held=0, no-grant inject `-3`, spontaneous request `-4`; GREEN: a self-scope grant issued via consent, a synthetic key injected into the app's OWN window read back as `EVENT_KEY_DOWN` 0x5A -> "injection WORKS", self-escalation holds, revoke refuses `-3`, `gotk=1`), proving the `cap_inject_guard` rewrite preserved the Stage 3 self path; captest (screen.capture, `cap_covers_path`) and sertest (serial.port, `cap_covers_port`) use helper paths this inject-only change does not touch.

### What Stage 0 does NOT claim

Stated here because the honest limits are part of the advisory, and because 0034 in particular is
easy to over-read. These are **Stage 0's** limits (`0030`-`0034`); two of them have since been taken
by later stages and say so inline.

- **0034 does not take serial (or any device node) out of ambient reach.** The seeded modes are
  permissive on purpose: landing the ordering fix with no seed rows would have broken the **shipping
  Terminal** (`/dev/ptmx` and `/dev/pts/N` opened `O_RDWR`, EACCES at uid 1000) and **`/APPS/PRINT3D`**
  (`/dev/ttyACM0` `O_RDWR`), and a security change whose first effect is that the Terminal stops
  working is a change that gets reverted, taking the hole with it. What 0034 delivers is the
  **mechanism**: a permission entry on a device node is now consulted, where before it was
  structurally impossible for it to matter. Tightening the policy is Stage 4's `serial.port`
  capability.
- **0030/0031 gate to the compositor; they do not introduce a capability.** An app that legitimately
  wants to inject input has no path at all until Stage 3. That is the intended posture
  (`docs/SYSTEM_CAPABILITY_API.md` 11.3), not an oversight.
- **A compromised compositor still holds everything.** It owns the framebuffer, draws every prompt,
  and is the principal these gates trust. That is the trust assumption #745 already makes and Stage 0
  does not change it.
- **Synthetic input is still indistinguishable from real input inside the kernel.** `SYS_INJECT_KEY`
  is now compositor-only, which removes the *unprivileged* forgery of `elev_last_input_ms`, but the
  kernel still has no provenance bit, so the compositor's own relay cannot be told apart from
  hardware. That is Stage 3 and it is not fixed here.
- The desktop session runs as **uid 1000** (measured on the RED rig: `[PROC] Created user process
  'CAPHOLE' (PID 35) ... uid=1000 gid=1000`), which corrects several in-tree comments that assert the
  session is uid 0. 0034 only bites below root, so this measurement is load-bearing for it.

### Evidence

**Re-substantiated against the code on 2026-10-02**, independently of the evidence blocks, before
the batch was prepared for publication. Every claim below holds: the `SYS_GET_KEYBOARD` and
`SYS_INJECT_KEY` dispatch arms both gate on the compositor principal (`capgate_caller_is_compositor`,
a thin C wrapper over `capgate_is_compositor_rs`), and `SYS_INJECT_MOUSE`'s pre-existing
`if (!is_compositor()) return -1;` is where the asymmetry was; `tcp_sock_owned_by_caller()` guards
exactly the seven Ring-3 TCP wrappers named and `tcp_get_conn()` itself is untouched, as the entry
says it deliberately is; `shm` carries `creator_tgid` and checks `capgate_caller_owns`; `fdlayer.c`
has the `/dev` branch AFTER `perms_check()` with the signpost comment standing where it used to be;
the `/SCREENSHOT.REQ` poll is gone and `screenshot_poll()` dequeues from `SYS_SCREENSHOT_POLL`;
`perms_dev_node_seed[]` carries `{ "/DEV/TTYACM0", 0600 }`; and `check_no_testinput()` is wired into
the gate's main path with its own `--self-test-testinput`. Two naming notes, neither a substantiation
failure: the entries call the ownership helper `capgate_owner_ok_rs()`, which is the Rust symbol the
C wrapper `capgate_caller_owns()` calls, and `tcp_get_conn()` now has 14 call sites to the entry's 13.

Unusually for this register, the Stage 0 five were **demonstrated on a booted VM, not only read from code**,
because `docs/SYSTEM_CAPABILITY_API.md` section 14 flagged that its own inventory was "read from code,
not demonstrated on a VM ... one grade weaker than a run".

`userland/apps/caphole` is the demonstrator: one unprivileged Ring-3 binary, unchanged between arms,
that prints `OPEN` per defect on a pre-fix kernel and `CLOSED` on a post-fix one. The cross-process
probes (0032, 0033) are genuinely two processes with distinct pids **and** tgids: the owner creates
the socket and the shared-memory region, and a spawned child reaches for them by raw table index. It
carries **positive controls** as well as the attacks, because, as `security/validate_test.c:352`
already notes, "a reject-everything validator 'passes' all negative tests": a run where the owner
cannot reach its **own** socket or its **own** region is reported as a REGRESSION, not as a pass.

RED arm, measured on VM 2390 against the unmodified golden **build 2391** (commit `e860a883`),
`/root/caphole-red2.log`:

```
[PROC] Created user process 'CAPHOLE' (PID 35) CR3=0x41e8a000 uid=1000 gid=1000
[CAPHOLE] D1   OPEN    injkey  (sys_inject_key returned 0 from a NON-compositor: synthetic key dispatched)
[CAPHOLE] D2   OPEN    getkbd  (reached the key ring from a NON-compositor (rc=-1: ring empty but body ran))
[CAPHOLE] D5   OPEN    devwr   (non-root opened /dev/zero for WRITE (fd=3) despite a 0644 root-owned
                                entry: /dev bypassed perms_check)
```

0034's RED arm depends on the rig planting `/DEV/ZERO:0:0:0644` into `/CONFIG/PERMS.DB` before boot;
without that the seeded 0666 default makes a non-root write open succeed either way and the probe
could never go green. The kernel's seed pass skips any path that already has an entry, so the planted
line survives.

Both arms ran the SAME probe binary against the SAME image with ONLY `kernel.elf` swapped (all four
ESP copies md5-identical, same `/APPS/CAPHOLE`, same planted `PERMS.DB` row).

RED, `/root/red-F.log`, ungated kernel:

```
[CAPHOLE] D1   OPEN    injkey  (sys_inject_key returned 0 from a NON-compositor: key dispatched)
[CAPHOLE] D2   OPEN    getkbd  (reached the key ring from a NON-compositor (rc=-1: ring empty but body ran))
[CAPHOLE] D3   OPEN    tcphij  (a foreign process tore down our socket (state 1 -> 0))
[CAPHOLE] D4   OPEN    shmmap  (a foreign process mapped our region and wrote into it (word0=0x0badc0de))
[CAPHOLE] D5   OPEN    devperm  (/dev open bypassed perms_check (rc=-1; expected -13 once consulted))
[CAPHOLE] RESULT: RED - 5 Stage-0 hole(s) OPEN
```

0032 and 0033 report IMPACT there, not a return code: a separate process with a distinct pid AND
tgid tore down the owner's listener socket, and mapped the owner's region to overwrite its
`0xc0deface` secret with `0x0badc0de`.

GREEN, `/root/green-F.log`, Stage 0 kernel:

```
[CAPGATE] selftest OK (compositor principal, handle ownership, unclaimed-latch refusal, pthread widening)
[CAPHOLE] D1   CLOSED  injkey  (sys_inject_key refused (rc=-1))
[CAPHOLE] D2   CLOSED  getkbd  (sys_get_keyboard refused (rc=-13 EACCES) before the ring)
[CAPHOLE] D3   CLOSED  tcphij  (foreign close refused; our socket survived (state stayed 1))
[CAPHOLE] D4   CLOSED  shmmap  (foreign map refused: creator-only enforced (word0 still 0xc0deface))
[CAPHOLE] D5   CLOSED  devperm  (perms_check consulted for /dev (non-root WRITE -> -13 EACCES))
[CAPHOLE] RESULT: GREEN - every Stage-0 hole is CLOSED
```

`[STAGE] DESKTOP_READY` reached, zero `[WQBLOCK]`, no panic.

**THE POSITIVE CONTROL, which matters more than the refusals.** A gate that refuses everything
"passes" every negative test (`security/validate_test.c:352` makes the same point). The kernel's own
running ledger, on the durable heartbeat, `/root/green-hb.log`:

```
[CAPGATE] fbowner=27 obs=1/1364 inj=1/0 tcp=2/44 shm=1/1 dev=1/8 (refused/allowed)
```

Every gate shows BOTH arms firing. `obs=1/1364` is one refused attacker against 1,364 admitted
compositor reads, i.e. the riskiest change in Stage 0 did **not** break the subsystem it guards.
`dev=1/8` is one refusal against eight legitimate `/dev` opens now passing THROUGH `perms_check`,
which the pre-fix kernel never did at all.

**0034's decisive evidence is the kernel's, not the probe's.** The probe initially reported 0034
OPEN on a kernel where the fix demonstrably worked, because libc's `open()` (`stdlib.c:991`)
collapses every kernel error to `-1` and stashes the code in `errno`, making `-13` (EACCES)
indistinguishable from `-1` (no such device). The independent artifact is the kernel's own A/B:
`grep 'PERMS-DENY.*path=/dev'` returns **zero** across the RED logs, which is structurally impossible
pre-fix because `/dev` never reached `perms_check`, and on GREEN returns
`[PERMS-DENY] proc=CAPHOLE uid=1000 want=-w- path=/dev/ttyACM0`. The probe now calls raw `sys_open()`;
any Ring-3 test whose verdict depends on WHICH error the kernel returned must do the same.

**A LATENT PANIC THE RUN FOUND.** `sys_shm_create()` zeroed each new region through
`phys_addr + 0xFFFF800000000000`, a Linux higher-half direct-map offset that does not exist in this
identity-mapped kernel. That address is unmapped, so the `memset` took a Ring-0 page fault and
panicked on the first `sys_shm_create()` ever made in anger. It was latent only because neither
`shm_create` caller in the tree is exercised on a normal boot, so SHM was unusable and nobody knew.
Fixed to zero through the identity map. It is unrelated to the capability gate and is recorded here
because 0033's demonstration is what tripped it.

**"Patched in" is now MEASURED, and the paragraph it replaces named two commits that do not
exist** (corrected 2026-10-02). This read: the work "landed on `dev` as `f1f3dc02` (the gates) and
`7fc087f5` (the demonstration and the ledger)", with the first golden left as future work.
**`git rev-parse --verify` fails on both of those names.** The Stage 0 commits are `58ba9417` (the
gates) and `f2230772` (the measured red-then-green). A hash that resolves to nothing is worse than
no hash: it reads as provenance and cannot be followed.

The first golden to carry each stage is now established with
`git merge-base --is-ancestor <fix-commit> <golden-commit>` against the golden archive, never guessed
from dates, and the "Patched in" cells above carry the result:

| Stage | Advisories | Fix commit | Last golden WITHOUT it | First golden WITH it |
|---|---|---|---|---|
| 0 | 0030-0034 | `58ba9417` + `f2230772` | 2391 (`e860a883`) | **2392** (`9c50d5d4`) |
| 1 | 0035 | `1cc435b4` | 2392 (`9c50d5d4`) | **2394** (`e4d2c03e`) |
| 2 | 0036 | `03cdac18` | 2396 (`2e53c220`) | **2397** (`03cdac18`) |
| 3 | 0037 | `dc0ecd05` | 2397 (`b083cd73`) | **2398** (`57795bd4`) |
| 4 | 0038 | `6eeba54a` | 2398 (`6de46840`) | **2399** (`6eeba54a`) |

Each row is an is-ancestor result in both directions, so the boundary is bracketed rather than
asserted. Affected is then "<= (first patched - 1)", which agrees with the affected bounds already in
the table for every stage.

**HONEST LIMIT on Stage 1, and the earlier wording of it was itself imprecise** (sharpened
2026-10-02). It read "Build 2393 is not in the golden archive, so it cannot be tested. 2394
(`e4d2c03e`, the image the 0035 evidence block was measured on) is the earliest golden PROVEN to
contain `1cc435b4`." **NEITHER 2393 NOR 2394 is in the golden archive**; the archive jumps 2392 ->
2396. So the sentence implied 2394 had been tested as a retained image when it had not, which is the
same shape of error as citing a hash that resolves to nothing. **The bracket is still sound, because
it never needed the image: it needs the COMMIT.** `e4d2c03e` is recorded by the Stage 1 measurement
run, and re-derived 2026-10-02: `git merge-base --is-ancestor 1cc435b4 9c50d5d4` FAILS (build 2392
provably lacks the fix) and `... 1cc435b4 e4d2c03e` SUCCEEDS (build 2394 provably carries it), so the
boundary is bracketed in both directions. The affected bound `<= build 2392` is therefore MEASURED.
Whether a 2393 existed and already carried the fix cannot be established either way, so 2394 is the
first build **proven** patched, not necessarily the first patched. Stated rather than rounded down
to 2393.

**Also re-derived 2026-10-02: the currently published release carries every one of these fixes.**
`git merge-base --is-ancestor` against build 2544's own commit (`1c4953643eac`, from the archived
image) succeeds for `58ba9417`, `f2230772` (Stage 0), `1cc435b4` (Stage 1), `03cdac18` (Stage 2),
`dc0ecd05` (Stage 3) and `6eeba54a` (Stage 4). The public page says so in those terms rather than
inferring it from 2544 being a larger number than 2398.

**0038 IS NOT A DEFECT AND MUST NOT BE PUBLISHED AS AN ADVISORY** (noted 2026-10-02). Read its entry
further down: it describes a capability the project deliberately ADDED, consented cross-app
`input.inject`, together with the properties that keep it safe. There is no missing check, no
affected build range, and no caller doing something it should never have been able to ask for. It
belongs in this file as a security record, the way the public page keeps one number as "not a table
entry", but giving it an advisory row would pad the count with a feature. Its absence from the table
above is correct, and is now deliberate rather than an oversight. Its "first golden with it" row
above is therefore an introduced-in, not a patched-in.


## July 2026 advisories minted outside this register (0015-0017)

**These three were issued in July 2026 in `CHANGELOG.md` and in code comments and were never added
here, which is the whole cause of the September ID collision** (see the block at the top of this
file). They are recorded now so that max+1 from this file is finally safe. They are NOT re-dated and
NOT re-graded: the entries below describe what the code and the public page have said since July.

| ID | Component (file) | Class (CWE) | Severity | Reachability / attack vector | Fixed by | Affected builds | Patched in |
|---|---|---|---|---|---|---|---|
| MAYTERA-SEC-2026-0015 | DHCP client transaction ID (`net/dhcp.c` `dhcp_init` / `dhcp_discover`) | CWE-330 use of insufficiently random values | **Medium** | **Network (LAN), and it was the same on every machine.** `dhcp_init()` ran `dhcp_xid = timer_ticks ^ 0xDEADBEEF` BEFORE `sti()`, when `timer_ticks` is always 0, so every MayteraOS box on earth started its DHCP transaction sequence at `0xDEADBEEF` and sent its first DISCOVER as xid `0xDEADBEF0`. MEASURED on the the build host LAN: two independent VMs broadcasting DISCOVERs with the identical xid `0xdeadbef0` at the same moment. Two consequences: box A's broadcast OFFER passes box B's `pr.xid != dhcp_xid` check, so B can take a lease meant for A; and an off-path attacker who need not see the DISCOVER can forge an OFFER/ACK with a guessed xid, which is a gateway/DNS hijack. | #495 / #521: a fresh `rng_get_u32()` xid per DORA transaction, plus a permanent `[DHCP] DORA xid=%08x` line so a regression to a fixed seed is visible in the serial log | builds before the #521 fix | see `CHANGELOG.md` 2026-07; public page records build 924 |
| MAYTERA-SEC-2026-0016 | System-call argument validation (`proc/syscall.c` dispatcher; `security/validate.c`; `rustkern/argtab.rs`) | CWE-284 improper access control -> arbitrary kernel read/write | **High** | **Local, from any Ring-3 process.** #500 found 171 user-pointer arguments across 239 dispatcher cases and exactly FIVE validated call sites tree-wide. The checks that did exist were address-range tests that could not express the intent on this OS: the kernel is identity-mapped at `0x400000` and `vmm_create_user_space()` copies PML4[0] into every user address space, so `info = 0x400000` passed "is it below `0x800000000000`?" and the kernel wrote over its own text. Unvalidated pointers reached read paths (info leak), write paths (`sys_fb_info`, four argtab syscalls), a futex compare used as a read ORACLE, and a DEFERRED write at thread exit via `set_tid_address`. | #500 / #503 / #565: one chokepoint in `syscall_dispatch()` driven by `rustkern/argtab.rs`, `validate_user_ptr()` walking the live CR3 for the U/S and R/W bits at every level, and `tools/syscall-ptr-lint` failing the build on a pointer syscall that is in neither the table nor the manifest | builds before the #503/#500 chokepoint landed | see `CHANGELOG.md` 2026-07-17 / 2026-07-28; public page records the dispatcher fix |
| MAYTERA-SEC-2026-0017 | **NOT A DEFECT.** TLS 1.3 CertificateVerify (`net/tls/tls.c`, `rustkern/certverify.rs`) | n/a | n/a | **The number is reserved for record-keeping continuity, not for a vulnerability.** It documents the #510 LIVE ADVERSARIAL VERIFICATION that the TLS 1.3 CertificateVerify signature check, already correct in the tree, is actually load-bearing: a `CVBASELINE` build (the literal pre-fix parse-and-discard code) and a `CVTAMPER` build disagree on the IDENTICAL tampered input, which is the independent oracle that an on-path attacker replaying a real chain with its own ECDHE is rejected. No code change, no affected range, no patched-in. The public page carries it in exactly these terms as "not a table entry". | #510 (verification only) | none | n/a |

**WHY 0017 IS HERE AT ALL.** A reserved, documented non-defect is cheap; a hole in the sequence is
not. The number was cited in `net/tls/tls.c`, `net/tls/tls.h`, `rustkern/certverify.rs`,
`rust-symbols.manifest`, `CHANGELOG.md` and on the public page long before this file knew about it,
and that silence is precisely what let September take `0017` for a real defect. Recording it honestly
as "not a defect" is the entry; inventing a vulnerability to fill the row would be worse than the gap.


## WITHDRAWN IDs: 0018-0023

`0018` through `0023` were issued on 2026-09-04/05 for the capability-API batch and were renumbered
to `0033`-`0038` on 2026-10-02 when the `0015`/`0016`/`0017` collision was resolved by moving the
whole September run clear of the July IDs. They were never published. **They must never be
re-issued**, and `tools/advisory-id-gate/advisory-id-gate.py` fails the build if any of them is still
cited anywhere in the tree, which is also the proof that the renumber was complete.

| Withdrawn ID | Renumbered to | Finding |
|---|---|---|
| 0018 | **0033** | `sys_shm_map()` missing authorization (`ipc/shm.c`) |
| 0019 | **0034** | `/dev/` node open ordering bypassed `perms_check()` (`proc/fdlayer.c`) |
| 0020 | **0035** | `/SCREENSHOT.REQ` ambient whole-screen capture (`userland/apps/compositor/screenshot.c`) |
| 0021 | **0036** | ambient `/dev/ttyACM0` access (`kernel/fs/perms.c`) |
| 0022 | **0037** | unasserted `/TESTINPUT.TXT` debug-injection marker (`drivers/testinput.c`) |
| 0023 | **0038** | consented cross-app `input.inject`, **not a defect** (Stage 4 capability) |


## Availability / resource-exhaustion advisories (0024-0029, 0039-0041)

**These six were PUBLISHED on maytera.net before they were recorded here.** That is the second half of
the same ledger failure: a register that is the source of truth and is missing six published
advisories is not the source of truth for that class. The entries below are reconstructed from the
published page (`/var/www/html/security.html` on the build container, read 2026-10-02) and from the matching
`CHANGELOG.md` and `blame.md` entries for the #245 sweep, which agree with it.

The class is distinct from the three above: nothing is corrupted and nothing is disclosed. A crafted
input causes unbounded or grossly disproportionate work, so the machine or the process stops doing
anything useful. The recurring shape, named in `blame.md`, is **a limit that resets faster than the
attacker has to work**: a deadline re-armed inside its own loop, or a scanner that advances only on a
byte it recognises.

| ID | Component (file) | Class (CWE) | Severity | Reachability / attack vector | Fixed by | Affected builds | Patched in |
|---|---|---|---|---|---|---|---|
| MAYTERA-SEC-2026-0024 | CSS colour parse (`browser/port/netsurf/cssvar.c` `parse_color`) | CWE-835 unbounded loop (infinite) | **High** | **Remote, unauthenticated, any web page.** The `rgb()`/`rgba()` component scanner advanced its cursor only on a byte it recognised, so a component written with a leading-dot alpha (the `.18` form every CSS minifier emits, as opposed to `0.18`) advanced nothing while the loop condition stayed true. This runs in `mcs_add_author_css`, before layout, on every stylesheet of every page, so roughly eleven characters of CSS froze the browser permanently at 100% of a core with no crash and no error. | Consume a fractional part, and record the cursor at the top of the loop body and advance unconditionally if the body consumed nothing, so **termination is a property of the loop rather than of its input** | <= build 2539 | **2540** |
| MAYTERA-SEC-2026-0025 | JavaScript math (`browser/port/duktape/duk_support.c` `log`) | CWE-835 unbounded loop (infinite) | **High** | **Remote, unauthenticated, any web page.** The logarithm normalisation loop iterated while the mantissa was at least 1.0, halving it each pass; halving infinity yields infinity, so the body made no progress while the condition stayed true. `DUK_LOG` maps onto this function with no special-casing and inline scripts run before CSS and before layout, so `Math.log(Infinity)` in a script tag froze the browser permanently. `Math.log2`, `Math.log10` and friends route through the same code. | The infinite case returns the ECMAScript-required value rather than being clamped, AND both normalisation loops carry an iteration bound (two independent layers). `Math.log`/`log2`/`log10`/`cbrt`/`pow(Infinity,0.5)` reach the same two loops, so one guard closed five entry points | <= build 2541 | **2542** |
| MAYTERA-SEC-2026-0026 | Progressive JPEG decoder (`kernel/gui/jpeg.c`, SOF2 scan loop) | CWE-834 excessive iteration (amplification) | **High** | **Remote, unauthenticated, untrusted image, and it burns Ring 0 time rather than a user process.** The progressive decoder ran a whole-image coefficient pass per start-of-scan marker with no cap on the attacker-controlled scan count. A DC-refinement scan carrying no entropy payload costs about ten bytes and still forces a full block pass, so a single file inside the browser's 1536 KB image cap carries roughly 157,000 scans. Measured, not calculated, on a 1,572,856-byte 4096x4096 grayscale fixture. | A scan-count bound of **256**, over six times what the most aggressive real encoders emit; the identical fixture is then rejected in 0.30 s (about 450x faster) and legitimate progressive JPEGs still decode. A boundary self-test asserts accept-at-256 and reject-at-257 | <= build 2541 | **2542** |
| MAYTERA-SEC-2026-0027 | TLS record receive (`kernel/net/tls/tls.c` `tls_recv`) | CWE-400 (+CWE-674) uncontrolled resource consumption; unbounded recursion | **High** | **Remote, unauthenticated, any TLS server the client connects to.** The receive deadline was re-armed at the top of each call and the function RECURSED on the TLS 1.3 skip paths (ChangeCipherSpec, post-handshake messages), so a peer streaming skippable records reset the timeout indefinitely and the call never returned. It was not also a stack overflow only because the compiler happened to turn the recursion into a loop, which is an accident of optimisation rather than a design. | De-recursed to a loop with the deadline set **ONCE** plus a skip-count bound (`TLS_MAX_SKIP_RECORDS`); the data-return paths are byte-identical | <= build 2541 | **2542** |
| MAYTERA-SEC-2026-0028 | HTTP/2 fetch loop (`kernel/net/http2.c` `http2_get`) | CWE-400 uncontrolled resource consumption | **Low** | **Remote in principle, but NOT reachable on a shipping image**, and ranked Low for that reason rather than for its mechanism (`g_tls_force_h1_alpn` defaults to 1, so ALPN never negotiates h2 on a stock kernel). The frame loop had only a no-progress cap, re-armed on every byte received, so a peer dribbling one 9-byte frame every few seconds could hold a worker and one of only SIX global async-fetch slots, shared by every process on the machine, for an unbounded time. | A whole-fetch deadline set once and checked each iteration, mirroring the pattern already proven in `https.c` | <= build 2541 (dormant) | **2542** |
| MAYTERA-SEC-2026-0029 | DOM lookup (`browser/port/duktape/duk_dom.c` `find_by_id`, behind `document.getElementById`) | CWE-674 uncontrolled recursion (stack exhaustion) | **High** | **Remote, unauthenticated, any web page.** The lookup recursed once per DOM nesting level with no depth cap, over the full document tree rather than the depth-capped layout tree, and the HTML tree builder applies no nesting limit of its own: its open-element stack simply grows. A page that nests elements deeply and then calls `getElementById` drives the recursion past the 2 MB Ring-3 stack and the kernel terminates the browser. Blast radius is the browser process; the machine survives. Attribution needed a three-way control, because BOTH ways of building a deep DOM are themselves a CPU-peg: build-only (no crash), build+find (page-fault at `CR2 == RSP - 8`), fixed-build+find (no crash). | A depth cap of **256**, mirroring the existing layout-walk limit; content deeper than that is never rendered anyway, so the cap turns a crash into a correct not-found | <= build 2542 | **2543** |
| MAYTERA-SEC-2026-0039 | HTML tree construction and DOM insertion (vendored `browser/port/netsurf/` libdom `_dom_node_insert_before`, `_dom_node_dispatch_event`, `_dom_node_readonly`; hubbub `element_in_scope`) | CWE-407 inefficient algorithmic complexity (+CWE-674 uncontrolled recursion on tree teardown) | **High** | **Remote, unauthenticated, any web page.** Every node insertion walked the new node's ancestor chain **four separate times**, so building a deeply nested document costs time proportional to the SQUARE of its depth. Both ways in are affected and neither needs anything unusual: the HTML parser on ordinary nested markup, and `createElement`/`appendChild` from script with no parser involved. Measured on the shipped engine sources at nesting depth 32,000: parse 28.77 s, script 26.28 s, growing about 4x for each doubling of depth, with 1,024,192,000 / 512,080,000 / 512,080,000 / 512,048,001 ancestor visits counted exactly at the four sites. In the OS, on a 440 KB fixture, the browser held 66-87% of a core across 116 heartbeat samples over about 128 s. **Separately and not a complexity bug at all:** the engine destroys a tree by RECURSING once per node, and nothing in tree construction capped depth, so a 32,000-deep tree faulted the 2 MB Ring-3 stack on teardown where a 16,000-deep one did not. The first suspect was NOT the cost: the parser's own quadratic `element_in_scope` is called once rather than repeatedly for some tags, and nesting such a tag took the same 28.7 s | All four walks now answer in O(1), and each is proven to give the SAME answer rather than an approximation: with no event listener registered anywhere the dispatch walk's list is empty, a childless new child cannot be an ancestor of anything, with no entity node in existence the read-only walk cannot succeed, and zero scope occupancy means not-in-scope. Result 0.0280 s and 0.0135 s at depth 32,000, exactly 2x per doubling, measured with the cap RAISED so the algorithms and not the cap are what is being measured. **Plus a construction depth cap of 256**, now taken from one shared header together with the layout walk's limit and `0029`'s `getElementById` cap, so the three limits are one number instead of three literals. Over the cap the start tag becomes a parse error and content inside it is still inserted at the deepest node allowed, so nothing truncates | <= build 2545 | **build 2546** |
| MAYTERA-SEC-2026-0040 | Shared userland allocator (`userland/libc/stdlib.c` `find_free_block`, reached by every `malloc()` in every Ring-3 process) | CWE-407 inefficient algorithmic complexity | **High** | **Remote, unauthenticated, any web page, and the blast radius is EVERY Ring-3 application, not only the browser.** `find_free_block()` walked an address-ordered list of ALL blocks, free and in use alike, so one `malloc()` cost time proportional to the number of live blocks and any workload that RETAINS what it allocates is quadratic overall. A page whose script creates DOM nodes in a loop is exactly that workload. At 10,000 nodes the list held 30,659 blocks, a single `malloc()` averaged **6,044 list steps**, the run spent 496,501,543 steps in the walk, and gprof put **98.58%** of wall clock inside the allocator with the DOM library and the JavaScript engine both at 0.00%. Creating 20,000 nodes from script took **23.952 s**. The existing `g_free_hint` short-cut did not contain it because it is ONE scalar across all sizes and every `free()` raises it, so a 32-byte request still had to scan the whole list to prove that no large block fitted, and the next `free()` raised the hint again. **The obvious suspect was wrong, and was refuted three ways**, which is worth recording because it is why this took as long as it did: the standing hypothesis was the JavaScript engine's mark-and-sweep collector, and (a) the collector's own live-object counters came out **bit-identical** between a fast arm and a slow arm whose script times differed 72x, (b) a stressor written to grow the live object count linearly stayed linear, and (c) `createElement` with no insertion at all was still quadratic, so it was neither the GC nor insertion | A size-binned free list: 16-byte size classes up to 1 KB plus a catch-all, so the walk visits FREE blocks of a fitting size only, with a bounded scan within a bin. The walk is then **83 to 87 steps regardless of heap size**, the cost is exactly 2x per doubling out to 320,000 nodes, and 20,000 nodes take 0.0330 s (726x). It is **not** a memory-for-speed trade: over 3,000,000 random operations it uses 12,564 KB of arena against the old 14,558 KB, **14% LESS** | <= build 2548 | **build 2549** |
| MAYTERA-SEC-2026-0041 | Kernel address-space insert (`kernel/mm/demand.c` `do_mmap`, `vma_merge`) | CWE-407 inefficient algorithmic complexity | **High** | **Remote, unauthenticated, any web page, and the disproportionate work is in Ring 0.** The shared userland allocator grows the heap by mapping only the page-aligned gap it needs, at an explicit address immediately after the previous mapping, so a heap reaching S bytes issues S/4096 adjacent, same-protection map requests. `do_mmap()` created a separate region record for every single one, because `vma_merge()` **had ZERO CALLERS**; its own comment opened "DANGER, and the reason this has no callers". Three walks then start from the head of that list, including the lookup on **every first-touch page fault**. Measured on the real kernel functions with a deterministic node-visit counter, never wall clock: a 96 MB heap is **24,576 regions and 905,932,800 node visits**, and four times the heap costs sixteen times the work. The kernel's own instrument agrees from the other side: with coalescing off, one process reported reaching 256, 512, 1024, 2048, 4096, 8192 and then 16,384 regions, and 16,384 is exactly 64 MB / 4 KB, one region per page. End to end in the shipping browser, four equal batches of 100,000 retained `createElement` ran 2000 / 7000 / 12000 / 11000 ms with the browser at 97-99% of a core | `do_mmap()` calls `vma_merge()` after a successful insert, which is work that was already written and had never been invoked. Same rig, same workload: **1 region at every heap size, 1 visit per page, 18,432x fewer visits** at 96 MB, with the slope exactly linear. The same four browser batches become a flat 1000 / 1000 / 2000 / 1000 ms. The merge predicate now also requires `prot` and `ref_count` to match rather than only `flags`, because two ranges can carry identical flags with different protection words and the survivor would otherwise have reported one range's protection for both. Because the fix makes the defect INVISIBLE, a boot-time reporter announces the first time any address space's region list reaches a power of two from 256 up, and its decision function is self-tested at boot so the silence is a result rather than a dead instrument | <= build 2550 | **build 2551** |

**HONEST LIMIT on this section.** These six were written up for the public page first, so they do not
carry the internal evidence blocks the other classes do. The build boundaries above are the published
ones. The **"Fixed by" column was added 2026-10-02**, so this table finally has the same shape as the
other two advisory tables; it did not before, and a reader comparing the three could not tell whether
the column was missing or the fix was unrecorded. Its contents are what the public page already
states as the fix for each of the six, moved here rather than newly derived, and for `0027`/`0028`
moved out of the Reachability cell where the same sentence had been sitting. **It names the fix
MECHANISM, not a commit.** Unlike the capability stages, these six still have no fix commit bracketed
against the golden archive with `git merge-base --is-ancestor`, so the build boundary cannot be
followed back to a hash the way it can for `0030`-`0037`. Bringing them up to the evidence standard of
the rest of this file is outstanding work, stated here rather than implied by the table's presence.


### 0039-0041: evidence, build bracketing, and why these are advisories at all

**WHY THESE ARE ADVISORIES AND NOT PERFORMANCE BUGS, argued rather than assumed.** All three were
found as performance work on the browser, and that is a fair reason to be suspicious of them. The
test this register applies is REACHABILITY plus DISPROPORTION, and all three pass it the same way:
an unauthenticated remote input of ordinary size, an HTML page of a few hundred kilobytes or a short
script, causes work that grows faster than the input, with no bound anywhere in the path, and the
measured outcome is a machine that stops doing anything useful rather than one that is merely slower.
A linear input buying quadratic work is the amplification that defines this class; it is what
`0026`'s scan count and `0024`'s cursor already are, in a different shape. For `0041` the work is
additionally in Ring 0. If any of the three had been a constant-factor slowdown, it would belong in
`CHANGELOG.md` and nowhere else, and this section would say so.

**BUILD BRACKETING IS MEASURED, NOT DATED.** Each fix commit was tested for ancestry against the
BUILDINFO commit of every passing golden in the archive with
`git merge-base --is-ancestor <fix> <golden-commit>`, and the patched build is the lowest-numbered
golden that answered yes:

| Advisory | Fix commit | First golden containing it | Affected |
|---|---|---|---|
| 0039 | `74fa7e6e` | build 2546 (`f54e922117b4`) | <= 2545 |
| 0040 | `b93cab6d` | build 2549 (`b30d04479245`) | <= 2548 |
| 0041 | `ad00bf9f`, gate and end-to-end arm through `cd46b6dc` | build 2551 (`d19f5b059c31`) | <= 2550 |

`0039`'s boundary is independently corroborated: its own before-arm measurement was taken against
the stock build-2545 browser, which is the last affected build the ancestry test names.

**A WART IN THE "AFFECTED" COLUMN, named rather than smoothed over.** Builds **2547 and 2548 are
burned numbers**: both were produced by builds that FAILED their gates, so neither is a golden
anyone can run. The last golden actually PRODUCED before `0040`'s fix is therefore 2546, not 2548.
The column still reads `<= build 2548` because the convention in this file and on the page is
"first patched minus one", and a reader comparing their own build number against it must not be
told a number lower than the true boundary. The gap is a property of the build numbering, not of
the defect.

**`0041` IS WIDER THAN ONE PROCESS, and this is INFERRED from the code rather than measured.** The
region list is per-address-space, so the quadratic belongs to the process that grew its heap. But
the kernel is still entered under a single global lock: `kernel/proc/syscall.asm` calls
`bkl_acquire` on the SYSCALL path and the interrupt dispatcher wraps every vector in the same
acquire, with the page-fault handler running inside it. So while one address space holds 24,576
regions, each of its map calls and each of its first-touch faults walks that list with the global
lock held, and no other core can enter the kernel meanwhile. The per-walk visit counts are MEASURED;
the lock coupling is read from the two call sites named above and is INFERRED. Nobody measured
cross-core stall for this, and the entry does not claim it.

**NOT CLOSED, carried forward from the fixes themselves.**
- `0041`: a process with many genuinely NON-adjacent mappings still pays a walk proportional to
  their number, on every map call and every first-touch fault. Coalescing removed the case that
  hurt, not the data structure. The boot-time reporter exists precisely because that case remains
  expressible, and it fires once at 256 for a deliberately non-adjacent test workload.
- `0040`: the amplification needs the workload to RETAIN its allocations. Of five measured shapes
  only one was quadratic; pure-retention shapes of records, dictionaries and strings moved under
  10%, and a small-object shape did not move at all because the Python layer's own allocator
  absorbs requests at or below 512 bytes before `malloc()` is reached. The advisory is the
  retention case, which is the browser case; it is not a claim about every workload.
- `0039`: the depth cap changes behaviour past depth 256, by design. A differential corpus of 45
  tree-construction fixtures serialises byte-identically with the cap raised, and exactly the 8
  at-or-past-cap fixtures differ with the shipping cap, with their text-node counts unchanged.
- `0039`'s fix is in the VENDORED browser engine port, not in code this project wrote. The four
  O(1) replacements are each justified by a property of this port's configuration, not of the
  upstream library in general, which is why each one is stated as its own argument above.

**ONE CLAIM IN THE SOURCE RECORD THAT DID NOT SURVIVE CHECKING, recorded because a register that
only repeats the commit messages is not a check.** `0040`'s fix commit closed with a warning that
the Python interpreter would NOT get the fix, because it carried a forked private copy of the
allocator at its own path. The file facts were right and the conclusion was wrong: that copy had no
build consumer, the interpreter is relinked against the SHARED libc on every golden build, and a
later commit the same day (`3a6a40e4`) verified by relinking that the shipped interpreter carries
the binned allocator's symbols and not the old scalar hint, deleted the fork, and added a gate
against the next one. The advisory above therefore states the blast radius as every Ring-3
application WITHOUT that exception. Anyone reading the fix commit alone would carry away an
exception that no longer exists.


## Data-protection, integrity and authentication advisories (0042-0051)

**A FIFTH CLASS, and it is not interchangeable with the four above.** None of these corrupts
memory, none is a missing authorization check on an otherwise-correct syscall, and none is a
resource-exhaustion amplification. **EXTENDED 2026-10-04 with `0045`, `0046`, `0047` and `0049`.**
The four additions widen the class in one way worth naming: `0042`-`0044` were all local, and three of
the four new ones are reachable across the network, so the class is no longer a local-only class. What
still holds the class together is the evidence bar, not the attack vector. What they share is that **a protection the system asserts did not
hold, or data the user never asked to lose was destroyed.** They are recorded as their own class
rather than squeezed into the nearest existing one, because the evidence bar is different again and a
reader comparing rows is entitled to know which bar each row cleared.

The bar for this class is a **MEASURED before-and-after on a running system**, with the quantity that
matters counted: bytes kept or lost, or the identity actually negotiated. AddressSanitizer is
meaningless here and is not claimed. For `0044` the independent oracle is the **remote server's own
log**, because the client's report is the thing under suspicion and cannot be its own witness.

Two of the three carry an **acknowledged open residual**, stated in the entry and named on the public
page WITHOUT a reproduction, per the standing rule that a live defect is escalated rather than
published with a recipe.

| ID | Component (file) | Class (CWE) | Severity | Reachability / attack vector | Fixed by | Affected builds | Patched in |
|---|---|---|---|---|---|---|---|
| MAYTERA-SEC-2026-0042 | AI file-protection chokepoint did not cover every content-destroying route: `SYS_PKG_WRITE` (301) dispatch and partial overwrite (`proc/syscall.c`, `proc/fdlayer.c` `sys_write_inner`; `fs/escrow_guard.c`) | CWE-693 protection mechanism failure -> CWE-184 incomplete denylist (+CWE-392 missing report of error for the silent half) | **Medium** | **Local, and it is an escape from a restriction the restricted program cannot decline.** The kernel binds the shipping AI assistant programs (`/APPS/AICHAT`, `/APPS/AIDEMO`) into a **Recycle-only** mode at launch, before their first instruction. That binding is mandatory: there is deliberately no syscall that clears it, so unlike the pre-existing voluntary mode the program cannot opt out. A delete by such a process is redirected into the user's Recycle Bin, the bytes stay recoverable through the Files app, and the user is TOLD on screen ("AI moved to Recycle Bin", or the distinct "AI overwrote, old copy in Recycle Bin"). **Two routes that destroy file contents were outside that chokepoint.** (1) `SYS_PKG_WRITE` **replaces a whole named file in one call** and had **no escrow call of any kind** (MEASURED: zero occurrences of `escrow` in its authorization helper or its dispatcher case), so a bound process could irrecoverably overwrite any file the invoking user may write. (2) A **partial overwrite** (open an existing file for writing WITHOUT `O_TRUNC`, seek into it, write over existing bytes) kept no pre-image either, and it is the stealthiest of the set because **the file keeps its length**, so it is invisible to every route watching for a file getting shorter or disappearing. **Neither route fired the on-screen notice, while the sibling routes in the same build did**, so a user saw "AI overwrote, old copy in Recycle Bin" for some destruction and **silence** for these two. Silence reads to a person as "the AI did not touch that file", which is the misleading part and is worse than a plain gap. **The project's own coverage table claimed to enumerate "every FS-mutating syscall in the dispatcher" and was missing the package-write route**, so for that route the guarantee was stated and false. It is bounded by two real controls, which is why this is Medium: boot paths are refused for every Ring-3 caller, and ordinary POSIX permissions apply, so **no access is gained that the invoking user did not already have**. What is defeated is recoverability and notification, not a permission. **MEASURED DESTRUCTIVELY, red then green, twice.** Package write, on two copies of golden 2559 differing only in `kernel.elf` with the same prover binary injected into both: on 2559's own kernel the write SUCCEEDED and the pre-image was **absent from the bin**, and on the patched kernel all 17 arms pass, the two serial transcripts differing in exactly those five lines with the seven unrelated arms passing in both as a control. Partial overwrite, on golden 2560's own kernel: a bound actor opened an existing 53-byte file, seeked to byte 8 and overwrote 8 bytes; the write took effect, the length and tail survived, and **the pre-image was not in the bin** | (1) `escrow_fs_guard` then the authorization helper then `escrow_replace_victim()`, in that order, so there is ONE authorization decision before either writer, and deliberately a **copy** of the pre-image rather than a move, because a package write can still fail afterwards and moving the pre-image out would turn a failed install into a destroyed file. (2) `escrow_overwrite_victim()` from `sys_write_inner`, keyed on two new per-descriptor fields and gated on a per-write range test (`position < original length`), so a pure appender pays nothing. The cost was **measured, not assumed**: roughly 7 ms fixed plus the existing linear term, paid once per file per editing session (4 KB: 2.1 ms -> 9.5 ms; 64 KB: 9.6 ms -> 25.0 ms; 512 KB: 75.8 ms -> 171.5 ms), against a measured frequency of **16** qualifying writes across an entire boot to the desktop with everything running, **zero** of them from the compositor, shell, Files, login or any app. The coverage table was narrowed to what it can honestly claim and given its missing row; the three raw-device calls were given a written exemption that does NOT rest on "it needs root", but on the fact that none of them names a file, so there is no honest per-file pre-image to keep | **builds 2559 to 2561** (the package-write route in 2559 only; the partial-overwrite route in 2559, 2560 and 2561). Before 2559 no shipping image bound ANY program to the mandatory mode, so there was no restriction to escape; see the limits below | **build 2562** (the package-write route in **build 2560**) |
| MAYTERA-SEC-2026-0043 | whole-file open policy for network and mounted filesystems (`proc/fdlayer.c` `sys_open_k`: the SMB, NFS and `/MNT` auxiliary branches) | CWE-665 improper initialization -> unintended destruction of user data (integrity). NOT memory safety and NOT disclosure | **High** | **Local, ANY unprivileged Ring-3 program, on any already-mounted SMB share, NFS export or `/MNT` volume, and no attacker is required or postulated.** Opening an existing file for writing **WITHOUT `O_TRUNC`** returned a descriptor whose write buffer began EMPTY (`wlen = 0`), and whose only commit path on `close()` is a **whole-file replace**. So the bytes written in that session became the file's ENTIRE new contents and every other byte was destroyed. The write-intent test was a raw-bit expression that never distinguished "replace this file" from "write part of it". Two further arms of the same defect: `read()` on such a descriptor returned **-1** unconditionally, because the read buffer is never allocated on a write open, so a program could not read back the file it had just opened; and `ftruncate()` returned -1 for both families. The `/MNT` auxiliary branch was the worst case, marking the buffer dirty **at open**, so opening an existing file for writing and closing it again **with no write at all** committed a zero-length file. **MEASURED on golden 2560, the same image both ways with only `kernel.elf` swapped, running as the ordinary desktop user (uid 1000):** a **65,536-byte** file, four bytes written, came back **4 bytes** on SMB, on NFS and on `/MNT`; `read()` returned -1 on all three; on `/MNT`, open-and-close with no write came back **0**. Reachability is ordinary rather than adversarial, which is what makes it High: no crafted input, no privilege, and an ordinary editor saving an ordinary file is enough. **Creating** the mount is privileged; **destroying a file on an already-mounted share is not.** NOT affected, verified rather than assumed: local FAT (the boot partition and removable volumes, which already honoured `O_TRUNC` and `O_APPEND` against a real file position), the ext2 root (already correct, fixed earlier under #746), and the `/MNT` FAT branch, which this fix deliberately does not touch | **`rustkern/fdopenpolicy.rs` `fd_open_plan_rs()`**, ONE policy answering "does this open destroy data" for every descriptor family, whose `owes_empty` field is `O_TRUNC` **and nothing else**; the file's own doc comment makes that the single rule, "THE ONLY FIELD THAT MAY MAKE A CALLER DESTROY DATA. 1 exactly when O_TRUNC was asked for." Four call sites, FFI layout locked by `_Static_assert`, and a boot self-test. The descriptor mechanics landed with it: one shared read/write position, so `read`, `write`, `seek` and `ftruncate` all act at the same offset. **AFTER: 22 checks, 0 failures.** The four-byte case measures 65536 where it measured 4, `read()` measures 16 where it measured -1, and `ftruncate` shortens to 100 where it had left 65536 untouched | <= build 2563 | **build 2564** |
| MAYTERA-SEC-2026-0044 | SMB client connection-table lifetime and on-demand mount identity (`net/smb.c` `smb_init`, `smb_vfs_ensure`; `rustkern/smbseq.rs`) | CWE-636 not failing securely ("failing open") + CWE-1188 initialization of a resource with an insecure default -> CWE-287 improper authentication | **High** | **Local, any unprivileged Ring-3 program, with no user interaction and no network position.** `smb_init()` was an unguarded wipe of the live connection table AND the open-file table, reachable from **three runtime paths**, one of which is the plain syscall `SYS_NET_LIST_SHARES` (270), which has **no capability check in the dispatcher** (verified: it is a bare dispatcher case, and it appears in neither capability table). So any app could wipe every live SMB mount. Two consequences, and the second is the advisory. (a) The wiped sessions were **abandoned mid-flight**, because the socket handle was zeroed before anything could close it, so no FIN and no RST was ever sent and the server kept the session (MEASURED with a capture on the server: 25 messages sent, then neither FIN nor RST). (b) **The identity was lost with the record.** The on-demand mount behind every `/SMB/...` path keys only on the mount-point string; once the connection record is gone, nothing anywhere records that this path ever had an identity. Its fallback is a **guest mount**, a deliberate and documented feature for a share nobody has configured, using file-static defaults of the user name `guest` and a sixteen-byte **all-zero** NT hash that **no production path ever overwrites** (MEASURED: the only caller of the setter in the whole kernel is the self-test). So a share mounted as a real, named user was silently **remounted anonymously at the same path** and the caller got success. The only notice anywhere was a single kernel log line: **nothing compared the resulting session identity against the configured user**, no return code changed, and the share stayed listed at the same path with no dialog and no indicator. Whether the downgrade lands depends on the server, since an all-zero hash is not the hash of an empty password and so reaches guest only where the server maps a bad password to guest; it was **MEASURED succeeding against a real server** in the red arm. Separately and NOT a security defect, recorded because it is the other way into the same code: the message-sequence counter was bound to nothing, so it carried across a reconnect, the server correctly terminated the connection for violating its credit window, and the client reported that to the user as an **authentication failure**, pointing at the wrong cause | `smb_init()` is now **one-shot**, so neither syscall 270 nor a second mount can wipe a live table; the sequence counter is **bound to its socket** (`rustkern/smbseq.rs` `smbseq_next()`, which refuses on an unbound epoch or a socket mismatch and then declines to transmit, so a zeroed connection fails CLOSED rather than silently restarting the counter); and the reconnect path re-runs the handshake **in place** on the same record, which still holds its user name, domain and hash, so the common-case reconnect keeps its identity. MEASURED GREEN against the same server: re-established in place, 106 bytes byte-identical to the pre-drop read, authenticated as the configured user, 2 shares enumerated, and the server's own log **empty** where the red arm had filled it with sequence rejections. **PARTIAL, and the entry does not claim more**: the unprivileged local trigger is closed; the guest fallback on a FAILED reconnect is not | <= build 2565 | **build 2566** (the local trigger only) |
| MAYTERA-SEC-2026-0045 | SMB client accepted the server's own word on who it had signed in as, on a FIRST mount (`net/smb.c` `smb2_session_setup`; `rustkern/smbident.rs`) | CWE-287 improper authentication -> CWE-636 not failing securely (the "you are the guest account" answer was read into a log line and nothing else) | **High** | **Network-reachable, and the file server decides. The user need only mount a share; no attacker position is required, though an attacker in the path can also force it.** A share mounted with a real user name and password could come up as the server's **anonymous account**, be reported to the caller as success, and be listed at the configured path as that named user, while every read and write on it actually ran as the anonymous account. The mechanism is that SMB2's SESSION_SETUP response carries a `SessionFlags` field whose bit 0 means "I signed you in as guest", and **one line of ordinary server configuration** (`map to guest = Bad Password`, which is a common setting and not a hostile one) makes a server answer an NTLM authentication for a named user with `STATUS_SUCCESS` plus that bit set. The pre-fix client read the bit into a `kprintf` and **nothing else**; tree-wide, the sibling constant for the null-session case was defined in the header and **read by no `.c` file at all**. **MEASURED RED on golden 2567 against a real Samba server, with the SERVER'S OWN AUTH AUDIT LOG as the independent oracle**, because the client's report of who it was is the thing under suspicion and cannot be its own witness: "Logged in as guest", MOUNT OK, read **and** write both succeeded, and the written file landed on the server owned by the server's anonymous account. A second, distinct way in is an on-path attacker corrupting one byte of the client's NTLM proof so that the same server setting substitutes guest; that half is MEASURED too. **Note the dependency, which is why `0047` exists:** until `0047` the flag this check reads arrived over an unauthenticated channel, so an attacker could simply clear it in flight. This row is for the client never looking; `0047` is for the answer not being trustworthy even once it does | A runtime check at `smb2_session_setup()`, the single chokepoint that **every** mount path reaches (first mount, reconnect, the on-demand `/SMB/<server>/<share>` mount, and the share enumerator), rather than at any one caller. The decision is Rust (`rustkern/smbident.rs`, 5 exported symbols, all in `rust-symbols.manifest`, all referenced from shipping objects rather than only the self-test), and on a non-OK verdict the C side **logs off the session, clears `session_established` and `session_id`, and returns -1**, so the mount FAILS rather than coming up degraded. The legitimate anonymous mount is preserved and has its own arm: a configured user name of `""`, `guest`, `anonymous` or `nobody` folds to "the caller asked for anonymity" and is allowed whatever the flags say. An 11-case boot self-test is a required row in the boot self-test policy, so a kernel that stops printing it goes red. **MEASURED GREEN on the same rig:** refused, mount failed, nothing written, and **zero** anonymous sessions from the machine in the server's audit log, with two control arms (correct credentials still work; a deliberately anonymous mount still works) | <= **build 2568** | **build 2569** |
| MAYTERA-SEC-2026-0046 | AI file-protection capture kept no copy of a file it was about to destroy, on a mounted removable volume (`fs/escrow_guard.c` capture hooks; `proc/fdlayer.c` removable open branch) | CWE-693 protection mechanism failure -> CWE-636 not failing securely (the safety check asked the wrong disk, was told "no such file", and concluded there was nothing worth protecting) | **Medium** | **Local, and it is an escape from a restriction the restricted program cannot decline.** The kernel binds the shipping AI assistant programs into a **Recycle-only** mode at launch, before their first instruction, and that binding is genuinely irrevocable here: MEASURED in code, the self-exit syscall returns `ESCROW_E_LOCKED` for the mandatory states and a spawned child inherits the mark rather than laundering it away. So unlike `0042`, whose window had to be argued from the mark being escapable, this one needs no such argument. **On a plugged-in USB volume, three content-destroying routes destroyed the bytes and kept no copy:** a truncating open, an `ftruncate` shrink, and a partial write over existing bytes. Each capture hook asked the ROOT filesystem handle whether the file existed; that handle does not serve a hot-plugged volume, so a file plainly present on the stick read back as **absent**, the hook concluded "nothing to capture", returned success, and the destruction proceeded **silently**, with no pre-image and no on-screen notice, while the **delete** route on the same volume in the same boot worked correctly and did capture. That inconsistency is what makes it a broken promise rather than an absent feature. A second, independent mechanism made it worse: the removable branch of the open path returns early, so such a descriptor never recorded the file's original size (which is the only thing that arms the partial-write hook) and kept a **volume-relative** path, for example `/UT.TXT` for a file at `/USB0/UT.TXT`, which is not an empty string but a plausible absolute path naming a **different file in the root directory**, and two consumers of that field decide where a user's data goes. **MEASURED DESTRUCTIVELY, red then green, on a copy of golden 2567 with two differential kernels differing only in `kernel.elf` and a byte-identical prover in both:** on the pre-fix kernel the count of "PRE-IMAGE kept" lines in the whole serial transcript was **0** and three arms read `destroyed=yes recoverable=no`; on the fixed kernel the same count was **3**, naming the three absolute paths, and all four arms passed. It is Medium, not High, for the same reason `0042` is: **no access is gained that the invoking user did not already have.** What is defeated is recoverability and notification | `recycle_victim_stat()`, one volume-aware answer to "what is at this path", built on the resolver the delete route already owned, returning **1 present / 0 absent / -1 on a mounted volume nothing here can read**; a Rust decision function maps those to PRESENT / ABSENT / UNREACHABLE with 11 new boot self-test assertions; and the four hooks now **`return -1` on UNREACHABLE instead of 0**. That one sign change is the whole fix: a fail-open becomes a fail-closed, and it brings these hooks into line with the rest of the same subsystem, which already refused outright on network and image mounts. The descriptor layer now stamps both missing fields on the removable path | **builds 2559 to 2569** (2559 is where the non-declinable binding first shipped, confirmed by ancestry; before it there was no restriction to escape) | **build 2570** |
| MAYTERA-SEC-2026-0047 | SMB2 message signing was not implemented at all, so nothing authenticated the server or detected tampering (`net/smb.c` `build_smb2_header`, `smb_send_packet`, `smb_recv_packet`; `rustkern/smbsign.rs`) | CWE-306 missing authentication for a critical function -> CWE-345 insufficient verification of data authenticity | **High** | **Remote, from any position that can modify packets between the machine and the file server:** the same LAN segment, a switch or router position, or a hostile wireless network. No credential and no local program is needed. **There was no signing code.** MEASURED on the pre-fix tree: the connection record had a `session_key` field with **zero writes and zero reads anywhere in the kernel**, and the session key was never even derived, every input to it existing as a function local and being dropped on return; the SMB2 header was zeroed and eight fields set, never the 16-byte signature, so **every outbound request carried 16 zero bytes** where the authentication code belongs; and the `SMB2_FLAGS_SIGNED` constant occurred exactly **once** in the whole tree, at its own `#define`, never set outbound and never tested inbound. The field that recorded "this server REQUIRES signing" was written in one place and **read nowhere**, so a server demanding signing was answered with unsigned traffic for ever. What this permits, with nothing detecting it: rewriting the contents of any file read from the share, rewriting directory listings to hide or invent files, turning a failed write into a reported success, rewriting any status code, and **impersonating the server outright**, since nothing authenticated it. **MEASURED: with every server signature stripped in flight by a tampering proxy, the pre-fix kernel mounted, listed, read, wrote, read back and reconnected, and reported its whole self-test as PASS**, on golden 2569 against a real Samba server. The proxy was **calibrated first** against a reference client configured to require signing, which passed the honest arm and rejected all three tamper arms, so the attack tool was validated before it was used as a verdict. **This row also records that `0044`'s published fix was incomplete as published:** that advisory's identity check reads a flag out of the SESSION_SETUP response, and until signing existed that response was unauthenticated, so clearing one bit in flight defeated the check. MEASURED, pre-fix: a falsely-identified working read and list mount | **HMAC-SHA256** for dialects 2.0.2 and 2.1, **AES-128-CMAC** over an SP800-108 counter-mode derived key for 3.0 and 3.0.2, and SMB 3.1.1 **refused** rather than guessed. The live path is AES-CMAC, because 3.0.2 is what is actually negotiated; an HMAC-only implementation would have been self-test-green and inert, which was MEASURED rather than assumed. No new cryptography: it calls the HMAC-SHA256 and AES-CMAC primitives already in the tree. The raw session key is a stack local, consumed once and zeroed on all four exit paths, and only the derived signing key persists, so the long-lived password-equivalent field is **removed** rather than filled. **Inbound signatures are verified and the default policy is `required`**: verification sits ABOVE the message-sequence gate (the sequence number is inside the signed region, so a forged one reports as a bad signature rather than a confusing desync), a mismatch **discards the response, condemns the connection and returns -1** rather than logging, the comparison is constant-time, and a server advertising no signing is refused **before any credential leaves the machine**. A third, explicit verification of the final SESSION_SETUP response happens **before** its identity flags are read, and that is what repairs `0044`. Cross-implementation evidence that the key derivation and framing are byte-correct: the real server **accepted every signed request**, and named AES-128-CMAC in its own rejection text in the red arms | <= **build 2570** | **build 2571** |
| MAYTERA-SEC-2026-0048 | the file-OPEN path skipped the permission check entirely on a mounted removable volume (`proc/fdlayer.c` `sys_open_k`; fixed with `rustkern/volperm.rs` and `drivers/hotplug.c`) | CWE-693 protection mechanism failure -> CWE-862 missing authorization, with **CWE-200 information disclosure** as a measured second consequence | **High** | **Local, any unprivileged program, no crafted input and no attacker postulated.** The entire permission block in the open path, including its single `perms_check()` call, was wrapped in a condition that was false for any path on a mounted removable volume, so for such a path **no permission check of any kind ran**. It was a deliberate carve-out with its reasoning in its own comment. **MEASURED on a kernel built from the tree as it then stood, as one uid-1000 process against a uid-1001 file in an all-users sticky directory on a mounted volume, with the IDENTICAL fixture on the root filesystem in the SAME boot as the control.** On the volume: a truncating open **blanked** another user's file (length 0); a plain write-only open with no truncation **overwrote its bytes in place, leaving the length unchanged**, which is the stealthiest arm because nothing watching for a file getting shorter can see it; and a read-only open of a **mode-0600 file owned by the other user returned its 64 bytes in full**. On the root filesystem every one of those was refused with a permission error. So the medium was the only thing that decided. **The confidentiality arm is the part nobody had named**: the carve-out skipped the read check too, because `R_OK` is derived inside the same block, and this is an information-disclosure failure and not only tampering. The asymmetry that makes it indefensible: `unlink` on the same file, in the same boot, **was** refused, so **you could be denied permission to delete another user's file and permitted to empty it instead**. The open path's own comment, thirty lines above the carve-out, had already stated the principle the carve-out was breaking. **Both halves of the original reasoning were tested rather than argued.** Its PREMISE, "a removable volume carries no ownership, so any answer we invent is arbitrary", is now FALSE of this system, measured two ways: the permission table **has** an opinion about a path on a volume, and the delete path acts on that opinion and **discriminates correctly**, refusing another user's file and allowing the caller's own in the same directory. Its CONCERN was CORRECT and also measured: the mount point had **no permission row at all**, and the no-entry default grants no write to anyone but root, so removing the carve-out ALONE would have made a USB stick read-only to the person who plugged it in, which is a worse bug that would have shipped looking like a security win | The carve-out is **deleted**, and the fix is **two halves in one change** because either alone is a regression. The enforcement half routes the decision through Rust (`rustkern/volperm.rs`), which returns "exempt" only for network shares and **deliberately ignores** the is-removable argument it is still passed, so reinstating the carve-out requires editing that function and facing its test vectors rather than appending a clause at a call site. The usability half gives the mount point an ordinary permission row with the shared-temporary-directory contract: any user may create, a created file gets **that user's** ownership row so it is theirs to empty or delete, and the sticky bit stops them touching anyone else's name. The row is deliberately not owned by whoever plugged the device in, and deliberately not persisted, so its lifetime matches the medium's. A **boot constant lock** calls the live Rust gate with four known vectors and compares the result against the C constants, because a self-test can prove the Rust is self-consistent but not that both sides name the same integer, and silent drift there would stop every removable path being judged. A second defect fixed in the same change because deleting the carve-out makes it live: the existence probe that decides "create or overwrite" also asked the root handle, and now fails closed. **MEASURED: 25 asserted arms, every one half of an ALLOW-and-DENY pair** on the same verb and uid differing only in who owns the target, plus two usability arms, red harness exit 1 to green exit 0 with only `kernel.elf` changed. The paired shape is deliberate: a deny-only arm passes on a kernel where the operation is simply broken, which this project had nearly shipped one gate earlier | <= **build 2574**; the carve-out itself dates to **build 2020** (2026-08-23), see the window note below | **build 2575** |
| MAYTERA-SEC-2026-0049 | SMB file contents crossed the network in clear text, and the opportunistic policy that would have encrypted them could be switched off by an attacker (`net/smb.c` send and receive chokepoints; `rustkern/smbcrypt.rs`, `rustkern/aesccm.rs`) | CWE-319 cleartext transmission of sensitive information, plus **CWE-757 selection of a less-secure algorithm during negotiation** for the downgrade | **High** | **Remote and passive for the disclosure: anyone who can see the traffic reads the files.** No credential, no local program, no active tampering. Every byte of every file read from or written to a Windows file share crossed the network unencrypted. **MEASURED at the byte level rather than argued**, with a fixed canary string written into a test file and then counted as raw bytes in a packet capture taken on the server: **3 occurrences in the clear before the fix, 0 after**, and the frame census moved from **89 plaintext SMB2 messages and 0 encrypted** to **18 plaintext and 71 encrypted**, the 18 survivors being the pre-key negotiation and session setup, which cannot be encrypted. The second half is active and is why the default had to change: a server advertises "I can encrypt" as **a single capability bit in its NEGOTIATE response**, that response **precedes every key, is unsigned, and is never retroactively validated at the negotiated dialect**, so an attacker in the path clears that one bit, an opportunistic client concludes the server cannot encrypt, and the mount comes up in plain text **reporting ordinary success**, with nothing the user sees to distinguish it from a server that genuinely cannot. **MEASURED both ways on booted machines with a proxy that clears exactly that bit**, and the pre-fix golden under the same attack completed its entire self-test and reported **PASS**. Also measured and load-bearing: the server sets its encryption capability **only if the client advertised one first**, so without the client-side bit a require-encryption policy would have refused every mount for a reason of its own making | **AES-128-CCM** under the SMB2 transform header, with the key derivation **shared** with the signing module rather than copied and the block cipher taken from the existing implementation. The obvious cipher was wrong and that was MEASURED, not assumed: at the negotiated dialect the real server **accepted** a CCM-framed message and **killed the connection** on a GCM-framed one, because the specification fixes the cipher at CCM for 3.0 and 3.0.2 and cipher negotiation only arrives with 3.1.1; reusing the tree's existing GCM would have been self-test-green and unable to talk to a server. **The default is `require`, not opportunistic**, decided before any credential is sent, re-checked at two later points, and any unrecognised value of the configuration knob resolves to **require** rather than to the weaker setting. If the server cannot encrypt the client **refuses**, naming the reason and the override; the override falls back and then says in plain words on every such mount that everything crosses in clear text, which it then measurably does. Three chokepoints mirror the signing discipline, and once a session is encrypted a plaintext answer other than negotiation or session setup is **discarded and the connection condemned**. A per-share "encryption required" setting that leaves the session-level flag clear is also honoured, which a session-flag-only client would have missed and then failed later with a bare error code. Measured cost: about 20 percent on a 2 MiB read in software with no CPU acceleration, reported rather than asserted as a threshold | <= **build 2576** | **build 2577** |
| MAYTERA-SEC-2026-0050 | the file holding saved network-share credentials had no entry in the permission policy, so it sat on the world-readable default (`/CONFIG/NETMOUNTS.CFG`; `kernel/fs/perms.c` `perms_system_seed[]`; written by `userland/apps/files/main.c` `save_netmounts()`) | CWE-732 incorrect permission assignment for a critical resource -> CWE-522 insufficiently protected credentials, with **CWE-200 information disclosure** as the realised consequence. NOT memory safety and no crafted input | **High** | **Local, any unprivileged program, no crafted input and no attacker postulated.** `perms_check()` defaults a path with NO entry in `perms_system_seed[]` to root-owned 0755, which is world READABLE (`kernel/fs/perms.c:157`). The saved-network-locations file had no entry at all, and the fifth `|`-separated field of every line was the SHARE PASSWORD IN CLEARTEXT: MEASURED by reading the pre-fix writer, where `save_netmounts()` assigns `flds[4]=m->pass` from `netmount_t.pass[40]` and writes it verbatim. So an ordinary `open("/CONFIG/NETMOUNTS.CFG", 0)` from any Ring-3 process of ANY uid returned every saved share password on the machine, and that is the same call the Files app already makes. **The boundary this crosses on a DEFAULT install, with no second account postulated**, is root to the desktop session: the file is root-owned by the policy default and the first interactive account first-boot setup creates is **uid 1000**, not uid 0 (`rustkern/sessionid.rs` `FIRST_ADMIN_UID = 1000`, in effect since 2026-08-07), so a non-root session could read a root-owned secret. The fix's own recorded COST is the cleanest evidence that the read was genuinely happening across that boundary: once the mode became 0600, "a non-root Files app can no longer READ the list either, so the /NET folder shows no saved locations". **THE REACHABILITY DEPENDS ON A SECOND FACT THAT WAS CHECKED RATHER THAN ASSUMED, because a world-readable file inside an unsearchable directory is not reachable at all.** Since #674 (2026-08-05) `perms_check()` resolves a path POSIX-style and requires `X_OK` on every intermediate component, so `/CONFIG`'s own mode is a precondition for reaching anything under it. MEASURED by loop-mounting the current golden (build 2578, commit `e10a9ba9`): the shipped `/CONFIG/PERMS.DB` carries `/CONFIG:0:0:0700`, which standing alone would deny search to every non-root process and would have masked this defect entirely. It does not stand alone: `perms_fix_config_dir()` rewrites that exact known-bad shipped value to **0711** at every boot, searchable but not listable, and the #745 comment block states the intent plainly, that the directory must be traversable and the protection belongs on the individual file. So on a running machine `/CONFIG` is searchable by anyone, a caller who names `NETMOUNTS.CFG` resolves to it, and the leaf's own mode is the only thing deciding, which is exactly what was missing. The same reading answers the obvious follow-up: `/CONFIG/MFA.DB`, named in that block as another file that would fall to the world-readable default the moment enrolment creates it, now carries its own `0600` seed entry, so it is covered and is not a second open finding. **MEASURED on the same mount: the shipped image contains no `/CONFIG/NETMOUNTS.CFG` at all**, so the file exists only once a user saves a network location and no credential ships in the golden at this path. It also confirms the fix is not shadowed: the seed fills a MISSING entry only, and none of the six rows in the shipped database names this path. **What was NOT measured:** no booted deny-and-allow arm exists for this path, and no boot self-test covers the 0600 mode; the pre-fix exposure and the post-fix mode are both established by reading the code and the shipped policy table, not by a run on a booted image. Nor was the exposure re-tested on an OLD image: the window is argued from the code as it then stood, and there is a sub-window to be honest about, between #674 making traversal live and the rewrite settling `/CONFIG` on a searchable mode, in which a root-owned 0700 directory would have masked the defect for non-root callers. That makes the range an upper bound on exposure rather than a claim that every build in it was exploitable | One line added to `perms_system_seed[]`, `{ "/CONFIG/NETMOUNTS.CFG", 0600 }`. It takes effect on an already-installed machine because the seed pass runs UNCONDITIONALLY rather than only in the "no PERMS.DB found" branch (`kernel/fs/perms.c:167`), so it is not a create-time-only default; it fills a MISSING entry only, on the stated "operator wins" rule, which bounds the fix honestly: a `/CONFIG/PERMS.DB` that already carried a permissive row for this path would shadow the seed, and the shipped database is recorded as holding three entries, none of them this path. The file was NOT relocated to per-user storage, deliberately, because the kernel reads it at boot to restore mounts before any account exists. A SEPARATE later change (`a1727fee`, 2026-09-29) replaced the cleartext field with the NT hash as `nt$<32 hex>`; **it lands AFTER this fix and narrows nothing about this advisory's window**, in which the field was cleartext throughout. The hash is also password-equivalent for SMB, so 0600 is recorded as having to stay regardless | <= **build 2489**. The feature that created the file shipped at **build 524** (2026-06-30, CHANGELOG #317) with the cleartext `label|server|share|user|pass` format, so the defect is as old as the feature; the floor is taken from the CHANGELOG and NOT from ancestry, because the repository's history begins at build 850 | **build 2490** |
| MAYTERA-SEC-2026-0051 | the OPENING HANDSHAKE of a file-share connection was authenticated by nothing, and what downgrade resistance existed came from the ENCRYPTION setting rather than from a control of its own (`net/smb.c` NEGOTIATE and SESSION_SETUP; closed with the new `rustkern/smbpreauth.rs`, plus `rustkern/smbsign.rs` and `rustkern/smbcrypt.rs` extended for the 3.1.1 key schedule) | CWE-345 insufficient verification of data authenticity -> CWE-757 selection of a less-secure algorithm during negotiation. The configuration half is deliberately given NO CWE: a control named for confidentiality was load-bearing for integrity, and inventing a number for that would be more precise than the evidence. NOT memory safety, no crafted input, nothing is corrupted | **Medium** | **Remote, but only from a position that can MODIFY packets between the machine and the file server**: the same LAN segment, a switch or router position, or a hostile wireless network. No credential and no local program is needed. **The pre-fix client offered at most dialect 0x0302** (MEASURED on the wire: `0x0202, 0x0210, 0x0300, 0x0302`), and no dialect at or below 3.0.2 authenticates the opening exchange at all, so on EVERY mount the negotiation of version, signing, encryption and cipher happened in the clear and nothing afterwards could tell that it had been edited. Message signing (`0047`) cannot cover it by construction, because the key it needs does not exist yet. **MEASURED red and green on booted machines, two kernels differing only in `kernel.elf` on the same image, against the same server, through a proxy that clears exactly ONE capability bit in the CLIENT's NEGOTIATE request:** on the shipping kernel the server consequently declined encryption and the mount completed, enumerated the share, read a file and **wrote** one, the kernel reporting `NOT ENCRYPTED ... crosses the network in PLAIN TEXT`; on the fixed kernel the same edit is rejected at the final SESSION_SETUP with "the signature DOES NOT MATCH". A second arm flipped **one bit of the random salt**, which changes nothing functional and is detectable only by a hash over the whole exchange, and that is rejected too; two further arms (a stripped negotiate context, a cipher the client never offered) are refused by name. **THE PRECONDITION, stated because it is what sets the severity: both measured arms ran with `encrypt=enabled`, which is NOT the shipped default.** Under the default `encrypt=required` the identical edit makes the PRE-FIX client refuse the mount, because `0049`'s fix already requires encryption, so **no handshake edit tested here produced a security loss at the shipped default**, and that is why this is Medium and not High. What `0051` adds over `0049` is that the refusal was coming from a setting named for CONFIDENTIALITY: a user who relaxed `encrypt=` to reach an older server, which is documented and supported, unknowingly gave up every defence against handshake tampering as well, and had no way to keep one without the other | **SMB 3.1.1 pre-authentication integrity**, new, in `no_std` Rust per the kernel policy. The client now offers 0x0311 and folds every byte of the five handshake messages into a running SHA-512 (`H = SHA512(H_prev || message)`, seeded with 64 zero bytes) that is mixed into the session key schedule, so any edit makes the two sides' keys diverge and the signature on the final SESSION_SETUP response fails. **TWO downgrade attacks are given TWO defences on purpose**, because conflating them is how encryption came to hold up version security: a FEATURE downgrade inside 3.1.1 is caught by the hash, while a VERSION downgrade cannot be caught by anything inside 3.1.1, since it prevents 3.1.1 from happening, so it is refused instead by a new client-side **dialect floor, `min_dialect=3.1.1` by default**. The key schedule was derived against a real server BEFORE it was written, by reconstructing a captured session and checking the derived key against the signature that server itself produced, which is a check a synthetic vector cannot perform on the labels it is itself choosing. One coupling is kept and stated rather than hidden: at 3.1.1 the final SESSION_SETUP response must be signed and must verify whatever the signing policy says, because the hash is only ever CHECKED by that signature | <= **build 2580** | **build 2581** |

### 0042: the honest limits, and the window argued rather than asserted

- **Why the range starts at 2559 and not earlier.** A hole in this protection is only an escape if the
  program cannot walk out of the protection by itself. Two marked states existed before today and
  BOTH were escapable or absent on a shipping image. The voluntary state is entered by a syscall the
  process calls on ITSELF, and the same process may leave by the paired syscall, so for it a missing
  capture is a mechanism gap and not an escalation. The mandatory state had exactly one producer, a
  kernel-side trusted spawner, whose only callers at build 2554 are two boot self-tests that are
  **double-gated**, and on a shipping image both gates are shut: each needs a marker file in
  `/CONFIG`, and each would spawn a binary that the build gate removes from the image. MEASURED by
  loop-mounting builds 2554 and 2559 and reading them: zero marker files, and none of the six escrow
  test binaries present. So **before 2559 no shipping image bound any program to a restriction it
  could not decline**, and there was nothing to escape. Build 2559 is the first image in which the
  mandatory Recycle-only binding shipped, and the AI assistant programs it names are measured present
  on that image.
- **The exposure is three internal builds and no release.** Builds 2555 to 2558 did not finish their
  checks and are not usable images. **No published release has ever carried the mandatory binding at
  all**, so nobody running a downloadable MayteraOS was exposed. The newest published release is far
  older than the feature.
- **Severity was argued, not assigned.** Graded by access gained, it is low: nothing becomes writable
  that the invoking user could not already write. Graded by whether it defeats the control's central
  promise, it is High, because along these two routes the recoverability guarantee and its on-screen
  notice are simply absent while the sibling routes announce themselves. Medium is recorded, with the
  disagreement stated rather than resolved silently.
- **The range before 2559 is recorded as a hardening gap, not as this advisory.** For builds from the
  escrow feature's introduction up to 2554, the voluntary mode did not capture rename-over, `O_TRUNC`,
  `ftruncate`, package write or partial write. Nothing was displayed to a user in that range that
  turned out to be untrue, because the capture and its on-screen notice did not exist yet; the
  guarantee lived only in a repository document that does not ship. The floor of that range is
  **build 2397**, DERIVED rather than measured, from the build number recorded in the source at the
  feature's introducing commit plus the way the build allocates numbers. **The exact first shipped
  build is unestablished**: goldens from that period are not retained and the build queue's history no
  longer reaches back to it. It is stated as a derived floor so nobody later reads a mountable-image
  convenience as a measurement.
- **One claim this entry deliberately does NOT make.** It does not say the mandatory marking is
  unreachable from Ring 3. It is not: the voluntary mark is entered by a syscall with no uid, name,
  capability or consent check, and from build 2559 the inheritance rule deliberately promotes a
  voluntary parent's child to the mandatory state, so any program can reach the mandatory state by
  marking itself and then launching a child. That route arrived already fixed and is not a defect
  here, because the mark is a restriction rather than a permission and a caller gains nothing by
  taking it on. It is recorded because an advisory asserting kernel-exclusive control of the mark
  would be wrong as written, and it is escalated below as a thing worth deciding on purpose.
- **One secondary claim is INFERRED and must not be restated as measured.** That the shipping file
  manager enters the voluntary state while holding a paste contract rests on a complete static call
  chain plus a fail-closed comment; it was **not executed on a booted machine**. It does not bear on
  the 2559 window. The experiment is named in the escalation list.
- **Recoverability is confirmed, which the first reading of this got wrong.** The bin exists at the
  root of both builds 2554 and 2559, and the retrieval user interface is in the **Files app, which the
  build gate requires**, not in the standalone bin viewer that the build gate removes. A user can get
  a captured pre-image back. Where a bin does not exist, the capture **refuses the operation** rather
  than destroying silently.

### 0043: the honest limits

- **The fix is on `dev` and has NOT been promoted.** MEASURED with `git merge-base --is-ancestor`: the
  fix commit is on `dev` and is absent from `testing`, `stable` and `main`. Goldens are built from
  `dev`, so "patched in build 2564" is true of the golden and of nothing else.
- **Two of the SMB cases are proven on NFS and `/MNT`, not on SMB.** The `O_TRUNC` and `ftruncate`
  cases were **SKIPPED, not passed**, in both green runs, blocked by the separate SMB reconnect defect
  that is now `0044`. They execute the same shared code, so the gap is believed to be in the evidence
  rather than in the code, and it is stated that way rather than counted as a pass. An earlier draft of
  the internal entry summarised the unfixed arm as "3 pass / 5 FAIL"; the kept transcript says
  **3 pass / 4 FAIL with one SKIPPED**, and the entry was corrected to match the artifact rather than
  an earlier observation of the same thing. The immutable commit message still carries the uncorrected
  numbers; the corrected entry is the record.
- **A preserving open must hold the file in kernel memory, so the fix introduces refusals rather than
  silent fallbacks:** 16 MiB for SMB and NFS, the pre-existing 256 KiB for `/MNT`, each returning
  `-EFBIG` with a log line, and a short preload refused too. That is a deliberate trade and a real
  behaviour change, not a free win.
- **The read-only path still has no size bound at all.** Pre-existing, explicitly untouched by this
  fix, and not assessed, so not an advisory here. Naming it is cheaper than letting the next reader
  assume it was covered.
- One stale comment survives the fix: the `SYS_FTRUNCATE` header comment in `fdlayer.c` still says SMB
  and NFS "upload whole files on close with no server-side truncate here", which this very commit made
  false. No behavioural effect; recorded so it is not re-derived as a finding later.

### 0044: what is closed and what is not

**CLOSED, and this is the whole of what the row claims:** the unprivileged local trigger. An ordinary
app can no longer wipe the live connection table, so it can no longer convert an authenticated share
into an anonymous one. That is the reachable, demonstrated defect and it is what the row is filed for.

**NOT CLOSED, acknowledged on the public page WITHOUT a reproduction:** when the in-place reconnect
**fails**, the connection is marked inactive, the live-connection lookup returns nothing, and the
on-demand mount falls through to the **identical guest mount**, silently, reporting success. The
guest-mount body is byte-identical before and after the fix. What the fix added on this path is
**detection in the self-test only**, an epoch check whose own comment admits that without it "this
step passes on a FAILED reconnect". That is a test assertion, not a runtime guard. **Nothing in the
production path refuses an anonymous session where a named user was configured.** The single measured
instance of the downgrade was produced with a deliberately re-broken reconnect, so it describes the
post-fix control flow as much as the pre-fix one.

**Who can reach the residual is INFERRED, and must never be restated as measured.** By code reading, a
hostile SMB server, or a network attacker who can drop the connection and keep it down, controls both
halves: forcing the reconnect and then failing it. **Nobody has run that experiment.** No
attacker-controlled server and no injected reset was used. The register therefore records the closed
defect as **local and measured** and the residual as **network-adjacent and inferred**, and the public
page says an open gap exists without saying how to drive it.

### 0044: AMENDED 2026-10-04. The residual is CLOSED, and the fix as published was defeasible

Two corrections to the entry above, both measured the day after it was written. Neither is a new
defect in `0044`'s own fix; one closes its stated gap and one narrows what that fix was ever worth.

**THE RESIDUAL IS CLOSED.** The gap this entry named, that a FAILED in-place reconnect fell through
to an identical silent guest mount at the same path and reported success, now has a **runtime guard
rather than a test assertion**. Three changes do it: the reconnect path keeps the mount and its
credentials in the table when revival fails, so the lookup can still see that this path belongs to a
named user; the on-demand mount **refuses** rather than re-mounting when a mount already exists at
that path, with the reason named; and the mount allocator refuses to hand a path already held by a
different identity to another one. Callers see an open, read or list **failure**; nothing succeeds on
somebody else's data. **MEASURED against a real server on a copy of golden 2567**, reproducing the
exact scenario this entry described (correct password, the account's password rotated on the server
while mounted, reconnect rejected): pre-fix the path changed hands to the anonymous account in
**60 ms** by the server's own microsecond audit log and the read **succeeded**; post-fix the mount is
KEPT as the configured user, the re-mount is REFUSED, the read FAILS, and the server's log shows
**zero** anonymous sessions from the machine. The legitimate anonymous mount still works, with its own
control arm. Escalation item 3 of 2026-10-03, "the one item I would fix first", is therefore closed.
The public page's acknowledged-gap note for `0044` has been rewritten accordingly rather than left
standing, because a published note saying a defect is open, after it is closed, is the same class of
inaccuracy this register exists to prevent, pointing the other way.

**AND THE FIX AS PUBLISHED WAS DEFEASIBLE BY ONE BIT IN FLIGHT.** `0044`'s identity check reads the
`SessionFlags` field of the SESSION_SETUP response, bit 0, and at the time it shipped **that response
was unauthenticated**: there was no message signing in the kernel at all (see `0047`). So an attacker
in the path cleared the bit and the check saw nothing. MEASURED pre-fix: a working, listable mount
reported as the named account while actually running as the server's guest. **For precision, the full
attack is two tampering operations, not one**, and the shorthand "defeated by one bit" should not be
restated without the other half: clearing the bit defeats the detection, and corrupting a byte of the
client's NTLM proof is what makes the server substitute guest in the first place. An in-path attacker
has both. `0047` repairs this by verifying a signature on that response **before** its flags are read.
`0044` is NOT re-dated or re-numbered for this: what it filed, the unprivileged local trigger, was
real and is closed. What is corrected is the implied strength of the remaining check, and the honest
reading is that `0044` was a configuration check resting on the server's unauthenticated word until
`0047` was in the same build.

**A DIFFERENT RESIDUAL REPLACES THE OLD ONE, and it is escalated below rather than closed.** A server
configured to act as one account regardless of who signs in authenticates the session as the real
named user, with the guest flag **clear**, and still performs every operation as the anonymous
account. The protocol at this dialect exposes nothing on the wire that a client could check, so the
new guard covers the identity the protocol reveals and no more. It is named on the public page as an
open gap without a reproduction, replacing the note for the residual that is now closed.

### 0044: a verification claim whose artifact is gone

The internal entry says all 11 Rust symbols were proven **LINKED** by `nm` on the debug kernel rather
than by grepping source. That artifact no longer exists and both surviving reproducer kernels are
stripped, so `nm` reports nothing. The substitute evidence is adequate but is NOT the check the entry
describes: the names are in `rust-symbols.manifest`, the kernel `Makefile` fails the build if rustkern
stops exporting a manifest symbol, and `strings` on the green kernel shows the self-test format
strings. Recorded because an evidence claim whose artifact has been destroyed should be downgraded in
writing rather than left standing.


## The issuing ledger (machine-readable)

**THIS BLOCK IS THE ISSUING POINT FOR `MAYTERA-SEC-2026-NNNN`.** An ID exists when it has a row here,
and `max+1` means max of this block. `tools/advisory-id-gate/advisory-id-gate.py` parses it on every
build and fails if an ID is cited anywhere in the tree without a row, if a row is duplicated, if the
sequence has a gap, if a WITHDRAWN row is still cited, or if a citation sits nowhere near any of that
ID's anchors.

`ANCHORS` are terms that identify the FINDING, separated by `;`. At least one must appear within a few
lines of every citation. They are what makes "one ID, one meaning" machine-checkable: two different
findings sharing one number cannot both match the same anchors. Keep them specific (a function name,
a ticket number, a filename), never generic.

`STATUS` is `LIVE` or `WITHDRAWN`. A WITHDRAWN ID keeps its row forever and may have no citations.

<!-- ADVISORY-LEDGER-BEGIN -->
| ID | STATUS | Meaning (one line) | ANCHORS |
|---|---|---|---|
| MAYTERA-SEC-2026-0001 | LIVE | ext2 directory parse OOB read (`fs/ext2.c`) | ext2; #476 |
| MAYTERA-SEC-2026-0002 | LIVE | DHCP reply option-TLV walk OOB read (`net/dhcp.c` `dhcp_parse`) | dhcp_parse; DHCP reply; dhcp |
| MAYTERA-SEC-2026-0003 | LIVE | ELF loader load-bounds overflow (`exec/elf.c`) | elf |
| MAYTERA-SEC-2026-0004 | LIVE | FAT/VFAT long-file-name reassembly OOB write (`fs/fat.c`) | fat_dir_step; LFN; vfat; fs/fat.c |
| MAYTERA-SEC-2026-0005 | LIVE | PNG IHDR size-math overflow (`gui/png.c`) | png |
| MAYTERA-SEC-2026-0006 | LIVE | JPEG SOF0/SOS/DHT header parse (`gui/jpeg.c`) | jpeg; DHT |
| MAYTERA-SEC-2026-0007 | LIVE | TLS 1.2 handshake-message loop OOB read (`net/tls/tls.c`) | tls; handshake |
| MAYTERA-SEC-2026-0008 | LIVE | HTTP chunked-decode overflow (`net/https.c` `https_dechunk`) | dechunk; chunked; chunk |
| MAYTERA-SEC-2026-0009 | LIVE | AAC/M4A ISO-BMFF sample-table parse (`media/aac.c`) | aac; mp4; m4a; ISO-BMFF |
| MAYTERA-SEC-2026-0010 | LIVE | HTTP/2 PADDED frame NULL deref (`net/http2.c`) | http2; HTTP/2 |
| MAYTERA-SEC-2026-0011 | LIVE | on-disk xattr entry-walk OOB read (`fs/xattr.c`) | xattr |
| MAYTERA-SEC-2026-0012 | LIVE | NFS3 READ reply destination over-write (`net/nfs.c`) | nfs |
| MAYTERA-SEC-2026-0013 | LIVE | JPEG Huffman table build OOB write (`gui/jpeg.c` `build_huffman`) | huffman; jpeg |
| MAYTERA-SEC-2026-0014 | LIVE | IPv4 receive-path length underflow (`net/ip.c` `ip_handle`) | ip_handle; IPv4; net/ip.c; total_length; ihl; #489 |
| MAYTERA-SEC-2026-0015 | LIVE | predictable DHCP transaction ID, CWE-330 (#495/#521) | dhcp; xid; #495; #521; 0xDEADBE; 0xdeadbe |
| MAYTERA-SEC-2026-0016 | LIVE | syscall pointer-validation chokepoint, CWE-284 (#500/#503/#565) | #500; #503; #565; argtab; validate_user; user pointer; user-pointer; choke point; CHOKE POINT; pointer |
| MAYTERA-SEC-2026-0017 | LIVE | TLS 1.3 CertificateVerify live verification, NOT A DEFECT (#510) | #510; CertificateVerify; certverify; 8446; TLS 1.3; not a table entry |
| MAYTERA-SEC-2026-0018 | WITHDRAWN | renumbered to 0033 (`sys_shm_map()` missing authorization) | WITHDRAWN |
| MAYTERA-SEC-2026-0019 | WITHDRAWN | renumbered to 0034 (`/dev/` open ordering) | WITHDRAWN |
| MAYTERA-SEC-2026-0020 | WITHDRAWN | renumbered to 0035 (`/SCREENSHOT.REQ` file drop) | WITHDRAWN |
| MAYTERA-SEC-2026-0021 | WITHDRAWN | renumbered to 0036 (ambient `/dev/ttyACM0`) | WITHDRAWN |
| MAYTERA-SEC-2026-0022 | WITHDRAWN | renumbered to 0037 (`/TESTINPUT.TXT` marker) | WITHDRAWN |
| MAYTERA-SEC-2026-0023 | WITHDRAWN | renumbered to 0038 (consented cross-app `input.inject`) | WITHDRAWN |
| MAYTERA-SEC-2026-0024 | LIVE | CSS colour parse infinite loop (`cssvar.c` `parse_color`) | cssvar; parse_color; CSS colour; CSS color; rgba; leading-dot |
| MAYTERA-SEC-2026-0025 | LIVE | `Math.log(Infinity)` infinite loop (`duk_support.c`) | duk_support; Math.log; logarithm; mantissa |
| MAYTERA-SEC-2026-0026 | LIVE | progressive-JPEG uncapped scan count (`gui/jpeg.c` SOF2) | SOF2; progressive; scan count; jpeg |
| MAYTERA-SEC-2026-0027 | LIVE | `tls_recv` re-armed deadline + recursion (`net/tls/tls.c`) | tls_recv; ChangeCipherSpec; TLS_MAX_SKIP_RECORDS; skippable; TLS record |
| MAYTERA-SEC-2026-0028 | LIVE | `http2_get` re-armed no-progress cap (`net/http2.c`) | http2_get; h2_read_full; HTTP/2; http2 |
| MAYTERA-SEC-2026-0029 | LIVE | `find_by_id` uncapped DOM recursion (`duk_dom.c`) | find_by_id; getElementById; duk_dom; DOM |
| MAYTERA-SEC-2026-0030 | LIVE | `SYS_GET_KEYBOARD` ungated keystroke theft (capability Stage 0) | SYS_GET_KEYBOARD; keystroke; key ring; cooked key; getkbd; keyboard |
| MAYTERA-SEC-2026-0031 | LIVE | `SYS_INJECT_KEY` ungated input injection (capability Stage 0) | SYS_INJECT_KEY; inject |
| MAYTERA-SEC-2026-0032 | LIVE | legacy raw-index TCP ownership break (`net/tcp.c` `tcp_get_conn`) | tcp_get_conn; raw-index; owner_pid; TCP; tcp |
| MAYTERA-SEC-2026-0033 | LIVE | `sys_shm_map()` missing authorization (`ipc/shm.c`) | shm_map; shm |
| MAYTERA-SEC-2026-0034 | LIVE | `/dev/` node open bypassed `perms_check()` (`proc/fdlayer.c`) | fdlayer; sys_open_k; /dev/; /DEV/; device node; perms_check |
| MAYTERA-SEC-2026-0035 | LIVE | `/SCREENSHOT.REQ` ambient whole-screen capture | SCREENSHOT.REQ; screenshot; screen.capture; capture |
| MAYTERA-SEC-2026-0036 | LIVE | ambient `/dev/ttyACM0` USB-serial access (`kernel/fs/perms.c`) | ttyACM0; TTYACM0; serial.port; USB-serial; serial |
| MAYTERA-SEC-2026-0037 | LIVE | unasserted `/TESTINPUT.TXT` debug-injection marker | TESTINPUT; testinput; input.inject |
| MAYTERA-SEC-2026-0038 | LIVE | consented cross-app `input.inject`, NOT A DEFECT (Stage 4) | input.inject; WINDOW_TARGET; cross-app; CROSS-APP; inject |
| MAYTERA-SEC-2026-0039 | LIVE | four O(depth) ancestor walks per DOM insertion + uncapped tree depth (libdom/hubbub) | deepdom; MAYTERA_DOM_MAX_DEPTH; element_in_scope; insert_before; dispatch_event; _dom_node_readonly; dom-limits |
| MAYTERA-SEC-2026-0040 | LIVE | userland `malloc()` walked ALL blocks (`libc/stdlib.c` `find_free_block`) | find_free_block; bin_index; free_bins; HEAP_BIN_SCAN_MAX; g_free_hint; malloc_locked; domscript |
| MAYTERA-SEC-2026-0041 | LIVE | `do_mmap()` never coalesced adjacent VMAs; `vma_merge()` had zero callers | vma_merge; do_mmap; vmascale; VMASCALE; mmapscale; vma_count |
| MAYTERA-SEC-2026-0042 | LIVE | AI file-protection chokepoint missed `SYS_PKG_WRITE` and partial overwrite (`fs/escrow_guard.c`) | escrow_replace_victim; escrow_overwrite_victim; SYS_PKG_WRITE; pkg_write_permit; pkgwrite; partialwrite; escrow_active; RECYCLE |
| MAYTERA-SEC-2026-0043 | LIVE | write-intent open EMPTIED the file on SMB, NFS and `/MNT` (`proc/fdlayer.c` `sys_open_k`) | fd_open_plan_rs; fdopenpolicy; owes_empty; netrunc; write_whole; sys_open_k; NETFS_RW_MAX_BYTES |
| MAYTERA-SEC-2026-0044 | LIVE | SMB connection table wiped at runtime; share silently remounted as guest (`net/smb.c`) | smb_init; smb_vfs_ensure; smbseq; smbseq_next; smbreconn; g_smb_def_user; guest mount; SMB2 |
| MAYTERA-SEC-2026-0045 | LIVE | SMB first-mount guest-identity downgrade unchecked (`net/smb.c` `smb2_session_setup`) | smbident; smbident_check; guestmount; SessionFlags; IS_GUEST; map to guest; #749 |
| MAYTERA-SEC-2026-0046 | LIVE | AI file-protection capture kept no pre-image on a removable volume (`fs/escrow_guard.c`) | recycle_victim_stat; escrow_victim_look; escmark_victim_disposition_rs; usbescrow; ESCROW_VIC_UNREACHABLE; fat_handle_set_abs_path |
| MAYTERA-SEC-2026-0047 | LIVE | SMB2 message signing absent; inbound signatures unverified (`net/smb.c`, `rustkern/smbsign.rs`) | smbsign; smbsign_begin; SMB2_FLAGS_SIGNED; AES-CMAC; aes_cmac; session_key; smbseq_condemn |
| MAYTERA-SEC-2026-0048 | LIVE | file-OPEN path skipped `perms_check()` on a removable volume (`proc/fdlayer.c` `sys_open_k`) | openperm; openperm_gate_rs; volperm; OPENPERM_ENFORCE; #250; carve-out; USBOPN |
| MAYTERA-SEC-2026-0049 | LIVE | SMB file contents sent in clear text; encryption downgradable by one bit (`rustkern/smbcrypt.rs`) | smbcrypt; aesccm; AES-128-CCM; transform header; CAP_ENCRYPTION; encrypt=required; cleartext canary |
| MAYTERA-SEC-2026-0050 | LIVE | saved network-share credentials were world-readable in cleartext (`/CONFIG/NETMOUNTS.CFG`, `kernel/fs/perms.c`) | NETMOUNTS.CFG; netmount_t; save_netmounts; perms_system_seed; peruser2; smbdisk; ntsecret; nt$; FIRST_ADMIN_UID |
| MAYTERA-SEC-2026-0051 | LIVE | SMB opening handshake authenticated by nothing; downgrade resistance came from the encryption setting (`net/smb.c`, `rustkern/smbpreauth.rs`) | smbpreauth; pre-auth integrity; PREAUTH_INTEGRITY; min_dialect; dialect floor; smbsign_begin311; smbcrypt_begin311; 0x0311 |
<!-- ADVISORY-LEDGER-END -->


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


### 2026-10-02, second review pass: the twelve commits after `31952105`. NO ADVISORY.

The day's later work closed four assurance defects, one build defect and one rendering defect. None
is a reachable defect in a shipped MayteraOS artifact, so none is an advisory and `0042` stays free.
Each is recorded here with the reasoning, because filing nothing is only honest if the reasoning is
written down; otherwise the next review either re-litigates it or inherits a verdict it cannot check.

- **The `mwt` pre-land loop silently skipped every argument-taking gate** (`tools/worktree/mwt`
  `land_one()`, fixed `db06a8b3`). The loop decided whether any gate needed a path argument with
  `preland_entries ... | grep -q <TAB>` under `set -uo pipefail`. `grep -q` exits at the first match,
  the producer dies of SIGPIPE, and pipefail makes the pipeline status 141 **on a successful search**,
  so the loop concluded "no entry takes arguments" and reported each such gate "not on this branch"
  while it was on the branch. MEASURED: wrong on 18 of 20 runs against the 23-entry list and 20 of 20
  against the 29-entry list, so the gates wired in that morning were skipped on roughly nine lands in
  ten from the moment they were wired.
  **This is the closest thing in the day to a security defect, and it is still not one.** Three
  reasons, in increasing order of weight. (a) The component is a developer tool that runs on the
  internal build host. It is in no image, it takes no input from a running system, and its only
  caller is a developer landing their own branch, who already holds write access to `dev`. There is
  no attacker and therefore no reachability story, which is the test this register applies to
  everything else. (b) The consequence set was **measured empty**: all twelve affected gates were run
  by hand against `dev` @`2b5957c5` from a clean `git archive` extraction, twelve of twelve exit 0, so
  nothing defective reached a build through the window. (c) The secret-scrub gate was **not among
  them**. `tools/release-gate/release-secret-gate.sh` is not a pre-land entry at all, and
  `tools/release-gate/publish-exclude-parity.sh` is argument-free and ran throughout, so the one
  mechanism whose failure has already produced a real credential leak on this project was never
  affected. Of the twelve, only `build/canary-gate.sh` (userland `-fstack-protector-strong` coverage)
  and `tools/contract-lint/contract-lint.py` (#233 app contract) carry security weight, and both were
  among the twelve verified green.
  **`0037` is the nearest precedent and it does not carry over.** `0037` is a build-integrity
  advisory, which is why the shape invites the comparison, but what `0037` describes is a property a
  SHIPPED IMAGE could have, an armable host-to-guest injection channel, and that is what gives it an
  affected-build range. This has no such property: no build is affected, and the
  `Affected builds` / `Patched in` columns could not be filled in honestly, which is a structural
  signal and not a technicality. A weakened assurance process whose consequence set is measured empty
  belongs in `CHANGELOG.md` and `blame.md`, where it is recorded at length, and not here as a row.
  Written down because the SHAPE recurs on this project and the next reviewer should reach a verdict
  on the evidence rather than by reflex in either direction.

- **Two kernel over-read oracles ran nowhere** (`kernel/crypto/run-all-kats.sh` globbed `*_kat.sh`
  while two oracles are named `*-oracle.sh`, fixed `4ac3fb7e`). This one deserved the most care,
  because `kernel/fs/ext2oob/ext2oob-oracle.sh` is the regression oracle for **`MAYTERA-SEC-2026-0001`**
  and `kernel/gui/imgoob/imgoob-oracle.sh` covers a BMP decode over-read, both against shipping Ring-0
  parsers of untrusted input. A regression in a FILED advisory could have gone undetected.
  **Both oracles PASS**, so no filed advisory has regressed and both guards are still load-bearing.
  That is not an assumption: the A/B disabled the real bound at `kernel/gui/image.c:100` and the old
  glob reported rc=0 with 12 of 12 passing and saw nothing, while the new glob reported rc=1 with 1 of
  14 failing and named it. No defect in a shipped artifact, so no advisory; the exposure was to a
  future regression, not to a present one.

- **Sixteen libc host unit tests had zero callers and six were RED** (fixed `8985da8b`). Not one of
  the six was red about the code it tests: four were AI-executor harnesses missing a mock layer that
  never followed the code it mocks, one asserted that `flock`, `utime` and `nftw FTW_MOUNT` "must
  never claim success" after all three had been implemented, one modelled a syscall number that had
  moved, and one was ASan flake. **No libc defect was being hidden** -> no advisory. Noted anyway
  because "the test was red for a year about nothing" and "the test was red about something" are
  indistinguishable until somebody reads all sixteen.

- **`kaslr-audit --self-test` leaked one `mkdtemp` per land** (fixed `7240b4c2`; 118 abandoned
  directories, 944K, on the build container). Unbounded growth, which is the availability class's
  shape, but on the build host, driven by developers landing commits, with no crafted input and
  nothing shipped -> no advisory.

- **Three apps were absent from a clean build** (`userland/apps/{aichat,flowedit,flowrun}`, fixed
  `d1137e27`): nothing built the vendored libyaml except `userland/apps/yamltest`, which sorts after
  all three, so a machine that had built before linked a stale library and a clean checkout failed.
  Checked for the one thing that would have mattered, whether a capability or permission decision
  parses YAML: **it does not and cannot.** The kernel links no YAML parser at any point, so no
  Ring-0 enforcement path can reach one; capability gating is kernel-side
  (`rustkern/capgate.rs` and the `proc/syscall.c` dispatcher, per `0030` and `0031`), and the YAML
  contracts are a development-time artifact validated by `tools/contract-lint/contract-lint.py`.
  A build-reproducibility defect -> no advisory.

- **The desktop widget layer stopped repainting whenever any window was open**
  (`userland/apps/compositor/main.c`, the `_apps > 0` idle branch, fixed `208cb95c`). `#564` had
  dropped the desktop layer on that branch as occluded decoration; the premise fails on a default
  install, because AICHAT holds a visible 12x764 collapsed dock handle on a 1280-wide screen for the
  whole session, occluding nothing. An idle machine's desktop clock and Uptime widget therefore froze
  silently, and a keypress snapped them up to date. (`9a96ed2f` then corrected the record that the
  12-pixel sliver is a deliberate dock handle and not itself a defect.)
  **Not an availability advisory, and it is the inverse of one.** The bar in this file is a crafted
  input causing unbounded or grossly disproportionate WORK; here strictly LESS work happened than was
  intended, with no crafted input, no attacker-controlled trigger, no loop and no allocation. The fix
  COSTS 0.56 percentage points more of one core (8.01%/7.94% before, 8.55%/8.52% after, two runs
  each). A Ring-3 app can pin the branch with a single static 1x1 window, but the denied service is
  desktop decoration, any input restores the full render path, and after the fix the residual is
  legitimate occlusion semantics, since `damage_drop_inside()` drops only rects wholly inside one
  opaque window and keeps partly-covered ones.
  **Checked specifically for the thing that would have made it security-relevant**, a frozen clock
  next to a lock screen, and it is clean. The screensaver arm
  (`screensaver_check_timeout()` inside `process_events()`), the idle session lock (`lock_poll()`),
  the capability-consent poll and the elevation poll all run at the TOP of every loop iteration,
  hundreds of lines before the fork, and the two timeouts read `uptime_ms()` against `g_idle_ms`
  rather than anything on the repaint path. The lock screen is force-full-rendered via `ui_busy`, so
  its own clock never froze, and arming either prompt sets `g_needs_redraw` and forces a full
  composite. The only coupling is loop cadence: a zero-damage tick backed the sleep off to 120 ms, so
  a minute-scale timeout could be observed up to about 120 ms late, and the fix tightens that rather
  than loosening it. One residual worth stating: a screenshot taken with no local input carried a
  stale DESKTOP clock while the taskbar clock in the same image was correct, which is how the defect
  was found. Nothing treats a screenshot as evidence of when it was taken today, and anything that
  ever does should read the taskbar clock.

- **Check-system work that is a strengthening, not a defect fix** (`dae8e8a1`, `46bbad35`,
  `852dd109`, `2b5957c5`). Seven, then five more, source checks moved from image-build-time only to
  running on every commit, and `build/preland-coverage-gate.sh` now FAILS when a source-only check is
  reachable from the image build and is in neither the pre-land list nor an exemption policy where
  every exemption carries a written reason and a stale one fails. Nothing was defective in shipped
  code; the position of a check changed. No advisory, and the direct counterweight to the first bullet
  in this list.


### 2026-10-03 daily review: the 35 commits after `5f0a7082`, across eight agent branches. NO ADVISORY.

`0042` stays free. The day closed a privilege-boundary escape, three ways to destroy a file that are
not deletes, a public build that could not fail, and a release gate that could not boot anything. Not
one of them is a reachable defect in a shipped MayteraOS artifact, which is the test this file
applies to everything else, so none of them is an advisory. The reasoning is written down because
filing nothing is only honest if the reasoning can be checked. Otherwise the next review either
re-litigates it or inherits a verdict it cannot see behind, which is how both halves of the
2026-10-02 drift happened.

SCOPE, stated so it can be re-derived. `git log 5f0a7082..dev` is **35 commits** carrying the tags
`[escrowreal]`, `[pubfailhard]`, `[bootgate]`, `[gatelog]`, `[recyname]`, `[escrvtexempt]`,
`[deadbtn]` and `[landlore]`, plus the day's 15 `CHANGELOG.md` entries and 21 `blame.md` entries.
**23 of the 35 are in golden 2559 (`5c6811ca`), and 12 are not in any golden**, which matters below.
Every claim here was checked against the diff, not the commit message: today's own record contains a
retraction (`7beb5b09`) of a shipping-regression claim that a commit message had already asserted.

#### The AI escrow delete-protection work (#246, `[escrowreal]`). Four real holes, and why none is an advisory.

The work makes the shipping AI programs unable to DESTROY a user's files: the kernel binds
`/APPS/AICHAT`, `/APPS/AIDEMO` and `/APPS/AIBENCH` into a RECYCLE-only mode at launch, before the
first instruction, and a delete by such a process is redirected into the user's Recycle Bin instead
of unlinking. Four ways to get round that were found and closed on the same day, three of which do
not look like a deletion at any layer above the filesystem:

1. **Launching a program dropped the mark entirely** (`kernel/proc/process.c` `init_proc`, closed in
   `8b8cedc0`). `fork` and `clone` carried the mark through their whole-struct copy and `execve` kept
   the same PCB, but `SYS_SPAWN` built a FRESH `process_t` through a `memset`, so a marked actor
   could spawn an ordinary unmarked uid-0 child and that child could hard-delete anything. Copying a
   process kept the restriction; launching a program lost it. This is the one of the four that is a
   privilege-boundary escape rather than a missed destruction route.
2. **A rename over an existing file destroyed it** (`sys_rename`, closed in `3076f208`). Both
   `fat_rename()` and `ext2_rename()` REPLACE the destination, so `rename("junk", "precious")` lost
   `precious` without going near the delete chokepoint.
3. **An `O_TRUNC` open destroyed every byte** (`sys_open`, closed in `cd6a1c8d`).
4. **`ftruncate()` shrinking an already-open file destroyed the tail** (`sys_ftruncate`, closed in
   `0b98bc06`). The open-time check could not see this one coming, because the open may have carried
   no `O_TRUNC` at all.

**THE REACHABILITY FINDING, which is what decides all four.** The protection these are holes in did
not exist in any build before today, and **no shipped image has ever contained a program that becomes
a marked escrow actor.** MEASURED, two independent ways:

- The only programs that call `SYS_ESCROW_ENTER` are `ESCROWT`, `ESCROW2T`, `ESCRVTH` and `ESCU6`,
  and the only programs the kernel-side trusted spawner launches are `ESCROW2T`, `ESCROW4T`, `ESCRVT`
  and `ESCRVTH`. **Every one of them is in `build/unshipped-apps.list`**, and
  `build/invariant-gate.sh` FAILS a finished image that contains any of them, independently of the
  code that produced it. No shipping app calls `escrow_enter` at all.
- Each kernel verifier that would spawn a marked actor returns silently unless a marker file exists
  (`/CONFIG/ESCRVT.TEST` and its three siblings), and **nothing in the tree creates any of them.**

So before today there was no marked actor on any shipping image, and therefore nothing to escape
from: the escrow mechanism confined only our own unshipped test programs. The commit that first
creates a shipping marked actor, `8b8cedc0`, is the same commit that closes the spawn escape.

**BUILD BRACKETING, by ancestry rather than by date or version string.** The last golden before any
of this work is **2554** (`c5a8c9082ca6`); `git merge-base --is-ancestor` places none of the four
fixes, and not the RECYCLE binding either, in it. The first golden that carries the protection is
**2559** (`5c6811ca`, read from the image's own `/BUILDINFO.TXT`, not from a filename), and it
contains all four closures and the case-folding fix. Builds **2555 to 2558** are archived as
`FAILED-...-NOT-A-GOLDEN`; they are spent build numbers, not images. **There is therefore no build in
which a marked actor exists and any of the four holes is open**, so the `Affected builds` and
`Patched in` columns could not be filled in honestly, which is the same structural signal that
settled the `mwt` question on 2026-10-02.

Two near-misses inside the same work, recorded so they are not re-derived. `ff9fea4c` made the AI
program list case-insensitive; before it, a lowercase install name would not have been matched and the
binding would simply not have applied. And `44f41286` fixed a case-only mismatch between the
recycler's declared and built install names. Both are same-day, both are in 2559, and the second
failed CLOSED (golden 2555 died on it) rather than shipping a dead daemon. The adjacent worry, a
case-sensitive compare in a protection path, was chased to the bottom and is NOT present:
`kernel/fs/perms.c` `normalize_path()` upper-cases on BOTH the lookup and the insert side, so the
permission layer is case-insensitive in exactly the sense the filesystem is and the two layers agree.

#### `[pubfailhard]`: the public build reported SUCCESS while shipping apps that were not on the disk.

`tools/release/scaffold/build.sh`, which is the build script published in the public source tree, ran
its optional-app loop as `if ! run_make ...; then FAILED_APPS="$FAILED_APPS $name"; fi`. The `if !`
suppresses `set -e` by design, and `FAILED_APPS` was only ever PRINTED, never turned into an exit
status, so the script ran off its end and exited 0 after announcing its own failures. MEASURED
consequence, not hypothetical: AI Chat and Maytera Flow (three binaries, `aichat`, `flowedit`,
`flowrun`) were genuinely absent from a public release candidate, and that candidate's own boot log
records `[StartMenu] DROPPED 'Maytera AI': launch target '/APPS/AICHAT' does not exist on this disk`.

**Judged as NOT an advisory, and this was the closest call of the day.** It is recorded at length
because the shape recurs. Three reasons, in increasing weight. (a) **The blast radius stops at the
optional app loop.** `libc`, `libgl`, the kernel and the UEFI bootloader are built by unguarded
`run_make` calls under `set -e` and always failed hard, so no security-enforcing component could be
silently dropped by this mechanism; what could go missing is an application. (b) **The measured
consequence set is missing functionality, not a weakened boundary.** Nothing was mis-permissioned,
nothing was exposed, and no credential path was involved; the secret-scrub gate is a separate
mechanism that this script never gated. (c) **No shipped MayteraOS image is affected.** The internal
golden build has never used this script (MEASURED: `build/build-golden.sh` contains no reference to
the scaffold), and the public image in which the apps went missing was a release candidate that was
not published. A build system that cannot fail is a serious assurance defect and it belongs in
`CHANGELOG.md` and `blame.md`, where it is recorded in full; it is not a row here, because an
advisory row makes a claim about an image and this makes none.

The fix also added `build/app-presence-gate.sh`, which derives the set of promised apps from the
start menu, the first-boot wizard and spawn call sites, and fails if any of them is absent from the
disk. It is wired in fatally on both paths (before any byte of a public image is written, and in the
golden build) and its `--self-test` is 10 arms with six RED, which is the property the boot gate below
lacked. It went red in anger the same day on `/APPS/escrvt`.

#### `[bootgate]`: a release gate that could not boot any image, and could only ever say no.

`tools/release-gate/boot-gate.sh` could not boot a current golden on this host at all: the minimal
QEMU device set left the firmware's boot-services allocations across the kernel's fixed link range,
and the bootloader correctly refused to load in place. Its single self-test arm was a RED arm, proving
only that it could refuse. **It is not an advisory, and the reason is the one detail that matters: it
reported exit 2, "could not run the check", and never a pass.** The pre-image header is explicit that
2 is "fail-closed, never treated as a pass". A gate that cannot run and says so has given no false
assurance, and no artifact was ever certified on the strength of it. It is also advisory and manual
against a real release artifact: MEASURED, nothing in the repo runs `boot-gate.sh <artifact>`, and its
only automated role is a self-test on two pinned fixtures.

Genuinely useful and worth keeping in view: `7beb5b09` added a refusal to render a verdict on a failed
build's output, keyed on the `${OUT_IMG}.FAILED` sidecar and the `FAILED-*`/`*NOT-A-GOLDEN*` naming,
because `build-golden.sh` stamps `/BUILDINFO.TXT` BEFORE its gates run and a dead build therefore
leaves a complete, correctly stamped, entirely plausible non-golden behind. **A stamp is not a pass**,
and that is now enforced rather than remembered. The same commit added `SETUP` to the invariant gate's
core app set, which is a real improvement for a real reason even though the regression that prompted
it did not exist.

One record-accuracy caution rather than a defect: `835f7ecb`'s message states both that the self-test
"passed throughout" and that the red fixture was being rejected for the firmware refusal. Those cannot
both be true of the pre-image, since the refusal path returns 2 and the old self-test counted only 1
as a pass. Treat the second as unproven; the lesson that a red-only self-test proves nothing stands on
its own.

#### The rest, briefly.

- **`[gatelog]`** (`20723fba`): `mwt` deleted every failing pre-land check's output and printed only
  lines matching `FAIL`, so a gate that died saying `error:` printed its name and nothing. Developer
  tooling on the build host, not in the golden and not in the public subset, with no principal on the
  other side of it, and it never made a land weaker: the refusal was identical before and after, only
  the explanation changed. No advisory. The full output is now kept per gate under
  `.git/mwt-gate-logs/`, with a self-test wired into the pre-land policy. One forward-looking note,
  INFERRED rather than audited: retained gate logs are new data at rest, and they would persist a
  secret if any pre-land gate ever echoed one. The secret-scrub gate reports pattern names and hit
  counts rather than matched bytes, so it does not; the other gates were not each checked.
- **`[recyname]`, `[escrvtexempt]`**: a build-time naming mismatch that failed closed, and a
  one-line, exact-match exemption recording that the uncooperative escrow prover is deliberately
  unshipped. The exemption is NARROW by construction: every comparison in the gate is whole-line
  fixed-string, so it cannot silently cover a future app. No advisory.
- **`[deadbtn]`**: three launch buttons that did nothing now work, and a failed launch now tells the
  user. A usability defect. No advisory.

#### ONE LIVE GAP, ESCALATED AND DELIBERATELY NOT PUBLISHED.

`SYS_PKG_WRITE` (301) is an FS-mutating syscall in the dispatcher that **has no escrow hook at all**
(MEASURED: zero occurrences of `escrow` anywhere in its authorization helper or its dispatcher case).
It replaces a whole named file in one call. A RECYCLE-marked AI process can therefore overwrite any
file the invoking user may write, keeping no pre-image, entirely outside the chokepoint. Boot paths
are refused for every Ring-3 caller and ordinary POSIX permissions apply, which bounds it, but those
are the same permissions the recycle redirect already sits on top of for `unlink`.

This also means today's coverage table, which states that it enumerates "every FS-mutating syscall in
the dispatcher", is **overstated by at least one entry**. The table's own justification for leaving
ordinary overwrite uncaptured does NOT cover this route: that justification is that at open time an
append cannot be told from an overwrite, whereas this call names one file and its full new length up
front, making it exactly as deterministic as the `O_TRUNC` case that IS captured.

Related and lower weight, recorded so the next slice sees the whole set: `SYS_BLK_WRITE` (451),
`SYS_PART_WRITE` (453) and `SYS_MKFS` (454) carry no escrow hook either. All three require euid 0,
which is a real control, so a marked AI running as an ordinary user cannot reach them.

**No advisory and no public write-up, because none of this is fixed.** It is escalated to the owner.


**RECONCILED 2026-10-03 BY RE-DERIVATION, not inherited from the previous entry.** The next free ID is
`0042`, confirmed two ways: `max+1` over this file's ledger block, and an independent sweep of every
`MAYTERA-SEC-2026-NNNN` string in the tree, whose highest is `0041`. The public page was parsed rather
than counted by tag: **33 advisory rows, 14 of them carrying `adv-row-remote`, every ID unique, and no
`0042` anywhere.** That is 7 out-of-bounds writes + 7 reads/null-deref + 8 access control + 9
availability + the two July rows `0015` and `0016`, which is what the page's own summary tiles say.
Table UNCHANGED today, because nothing was filed.

**ONE PAGE EDIT, no rows.** The "newest build with every fix" tile and its note read `2554`, correct
when written yesterday. Golden **2559** was produced today and the ancestry test was re-run against it
(`git merge-base --is-ancestor` for `74fa7e6e`, `b93cab6d`, `ad00bf9f` and `cd46b6dc`, the fix commits
for the three newest advisories, plus 2554's own commit), which places 2559 on the right side of every
fix on the page. Updated to `2559`, as of 3 October 2026, with the gap from 2554 explained in view:
builds 2555 to 2558 did not finish their checks, and a build number is spent whether or not the build
succeeds, so a gap in that sequence is not a missing release. The standing correction that no
PUBLISHED release carries these fixes is left intact and is still true.

### 2026-10-03 second review pass: the 25 commits after `ffe69f41`. THREE FILED, SIX ASSESSED AND NOT FILED.

SCOPE, stated so it can be re-derived. `git log ffe69f41..dev` is **25 commits** carrying the tags
`[pkgwrite]`, `[escrowred]`, `[partialwrite]`, `[foundbugs]`, `[netrunc]`, `[smbreconn]` and
`[noaccount]`, plus the day's CHANGELOG and blame entries. Every claim below was checked against the
diff and against the code at `dev`, not against the commit message. `0042`, `0043` and `0044` are
filed above. The six items below are NOT, and each is named individually, because a candidate that
does not survive contact with the diff is a finding about the record and not a failure of the review.

1. **Launching a program dropped the AI mark entirely** (`proc/process.c` `init_proc`, closed in
   `8b8cedc0`). REAL as code, and the most advisory-shaped lead of the day, but **NOT an advisory**:
   the fix is in the SAME single diff that CREATED the mandatory mode and its program list (confirmed
   by reading that one commit, and by the fact that the mark-policy module is born in it). So no
   shipping image ever carried a mandatorily-marked actor with this hole open, and the hole was closed
   in the same breath as the thing that would have made it exploitable. For the voluntary mode that
   did exist earlier, the route adds nothing, because such a process may leave its contract by
   syscall. A hole found and closed before it could ship is a good blame entry, not an advisory.

2. **The Recycle Bin capture asked the wrong filesystem** (`fs/recycle_fs.c`, closed in `5cbf4095`).
   REAL, and **NOT an advisory, because it failed CLOSED.** The capture asked the root filesystem
   about a file on a removable volume, which that handle does not route, so it could not read the
   victim's bytes, and the guard's contract on that outcome is explicit: it returns failure and
   **nothing has been removed**, on the stated principle that if the kernel cannot promise the bytes
   are recoverable it does not let them go. So on a plugged-in volume a marked AI could not delete at
   all. That is availability of a feature, not confidentiality or integrity: nothing was destroyed and
   no file was lost. One related shape was closed with it, and is INFERRED rather than measured: a
   path that happened to exist on the root with the same spelling would have had the ROOT copy
   captured and removed, leaving the intended file untouched.

3. **A FAT file handle remembered the name but not the location** (`proc/fdlayer.c`, closed in
   `fa926cc8`). **HELD BACK, and this is the one I would most like to file and cannot yet.** Its two
   halves fail in opposite directions. The advisory-lock half is a false CONFLICT, over-locking, which
   cannot let two writers both hold a lock, so it is correctness and not security. The other half is a
   genuine **fail-open**: the `ftruncate` pre-image hook resolved a bare basename, apparently
   successfully, the existence test then did not find that name, and the hook answered "nothing at
   risk", so a marked actor shrinking a file on the boot partition or on any removable volume
   destroyed the bytes with **no copy kept** and no refusal, where every sibling hook refuses and
   journals when it cannot keep a pre-image. **It is not filed because it has no demonstration.** Two
   independent reviews reached it from different directions and both stopped at the same place, and
   the fixing commit says so itself: "NOT verified: the capture exercised end to end by a marked
   program on a booted machine. That is an argument from the ext2 path, not a measurement." Filing it
   on a code reading would claim a witness this register does not have. The missing experiment is one
   run and is named in the escalation list; if it goes red, this is next review's advisory.

4. **An empty file could not be captured, and the program list did not fold case** (`ff9fea4c`). NOT a
   destruction route and should not be recorded as one. The empty-file half **refused** the delete,
   which is availability. The case-fold half is hardening and was MEASURED not to matter for the
   shipping image, where the installed entries are already uppercase and the launcher uses the literal
   uppercase path; it was done because the package installer can install either spelling.

5. **67 of 82 per-account files could not be written on a machine with no account** (`[noaccount]`,
   closed across `c47010db`, `1706c4fe`, `aad8b48e`). A severe usability defect, and **NOT an
   advisory**, on both halves of the test it has to pass. (a) The genuinely security-relevant sinks
   were not affected or not reachable: the kernel **security log** was never per-account and never
   moved, the escrow decision **journal** is a Ring-0 write and never moved, and the AI audit, grant
   and consent records were **unreachable** in this state, because the AI credential is root-owned
   0600 so a bootstrap session cannot start an AI task at all and therefore never writes one. (b) The
   one reachable sink is the Recycle Bin daemon's log, which is an operational diagnostic by its own
   description and not an audit trail, and whose every line is written to serial **unconditionally
   before** the durable copy is attempted, so what was lost was durability across a reboot and not the
   record. The dependent protection **failed closed and journalled the refusal**. The failure was
   silent to the user but **not** silent to diagnostics in the measured window. Also worth stating:
   the **67 and the 82 are asserted, not measured** (hand inventory; the one lint that exists counts a
   different thing and says so in its own header), while the seven refusals observed on a boot ARE
   measured. The no-account state is genuinely reachable on a shipping image, so the defect is real;
   it is simply not a security defect.

6. **`execve` has no kernel handler at all** (`[foundbugs]`, `cc33e28a`, `b447292d`). NOT an advisory,
   as expected. The syscall is a documented non-goal, the dispatcher's default arm **logs** the unknown
   number and returns an error, the library wrapper sets `errno`, and the one real caller surfaced
   "shell returned 127" on screen. A missing feature that returns an error and logs it is not a
   vulnerability, and it was routed around at that call site with the supported primitive rather than
   implemented. The sibling commit's **split notification spool** was assessed separately, because a
   security warning written to a file nobody drains WOULD be a finding: the mechanism is real, but it
   existed on `dev` for **16 minutes** and **no image was built inside that window**, and the session
   type it affected (root) was never broken. Before it, both sides resolved to the same path and
   kernel-posted security alerts were readable and did display. There is no shipped artifact to advise
   anybody about.

**RECONCILED 2026-10-03 BY RE-DERIVATION, a second time, not inherited from the earlier entry.** The
next free ID was `0042`, confirmed two ways exactly as the morning pass did: `max+1` over this file's
ledger block, and an independent sweep of every `MAYTERA-SEC-2026-NNNN` string in the tree, whose
highest was `0041`. **Both methods agreed with the morning pass**, so that measurement is confirmed
rather than merely inherited. The public page was parsed rather than counted by tag: **33 advisory
rows, 14 of them carrying `adv-row-remote`, every ID unique**, which agrees exactly with the morning
pass's parse. The standing `0017`-`0023` publication backlog was re-checked and **is closed**: the
September batch that once held those numbers is published as `0030`-`0037`, `0038` is on the page as a
capability rather than a defect, `0017` is on the page as a record rather than a defect, and
`0018`-`0023` are explained on the page as withdrawn. Nothing was republished and no duplicate was
created. The next free ID is now **`0045`**.

**ONE MEASUREMENT WHERE THIS PASS DISAGREES WITH WHAT IT WAS TOLD, recorded because one of us is
wrong and it matters.** The brief for this pass stated that golden **2564** is the current good image
and that builds **2565 and 2566 are burned failures**. Neither half holds. The live golden is
**build 2567**, commit `aad8b48e`, and 2565, 2566 and 2567 are all GOOD goldens. METHOD: each archived
image's own `/BUILDINFO.TXT` was read for its build number, commit and build timestamp, and that
timestamp was mapped into the build queue's recorded job windows (end time minus duration) and the
recorded exit code taken from the queue's own history. Build 2565 falls inside a job that **exited 0**
(1776 s), 2566 inside a job that **exited 0** (1609 s), 2567 inside a job that **exited 0** (1556 s).
The three jobs that DID fail that afternoon ran 122, 147 and 123 seconds, far too short to have
produced an image, and none of their windows contains any archived image's stamp. Consistently with
that, 2555 to 2558 are named `FAILED-...-NOT-A-GOLDEN` in the archive by the automatic rotation and
2565/2566 are not. The brief was most likely written before 2567 existed, which is exactly why a
build number in prose goes stale and why the per-advisory column is the thing to read.

### ESCALATED 2026-10-03 (second pass): live, unfixed, and deliberately NOT published

None of the following is on the public page with a reproduction. Where the public page needs to admit
a gap, it does so in one sentence that says a gap exists and not how to drive it.

1. **LIVE, fail-open: the AI file-protection capture is blind on hot-plugged removable volumes.** Four
   of the five non-delete capture hooks open with an existence test against the ROOT filesystem
   handle, and that handle does not route a hot-plugged volume's mount point: the file layer resolves
   such a path against that volume's OWN filesystem object. A negative answer from the test means
   "allow, nothing at risk", so for a file on a plugged-in stick the hook waves the operation through
   with no pre-image. The delete route and the partial-overwrite route are both volume-aware and DO
   work there, which is what makes this inconsistent rather than merely absent. This is the same
   routing mistake that `5cbf4095` fixed for the delete path only, and the escrow coverage document
   does not mention it at all. **INFERRED from code at `dev`, not executed.** The harness already
   hot-plugs a synthetic stick, so the missing arm is cheap.

2. **LIVE, fail-open: `/MNT`, `/SMB` and `/NFS` destroy whole files outside any capture route**, and
   the project's own escrow document says so in writing. The capture's existence test does not route
   those families, so it reports nothing at risk. `0043` above narrowed this considerably by making a
   write-intent open stop destroying the file, but an `O_TRUNC` open on those mounts still destroys
   and no capture sees it. Not fixed.

3. **LIVE: an SMB share whose reconnect FAILS is silently remounted anonymously.** The residual of
   `0044`, described in full in that entry. The production path has no guard; only the self-test
   detects it, and the self-test's own comment admits that without its epoch check the step passes on
   a failed reconnect. Who can force it is INFERRED and the experiment has not been run. **This is the
   one item I would fix first**, because it is an authentication outcome rather than a data-protection
   one, and because a hostile server plausibly controls both halves.

4. **LIVE, not security but silent: a write open on a `/MNT` FAT volume reports success and can never
   commit.** The close-time commit hits an in-use error for an existing name, so the write is
   discarded. `0043` deliberately did not touch that branch. A silent write failure, not a
   destruction.

5. **DECIDE ON PURPOSE: the AI mark is not kernel-exclusive.** The voluntary escrow mark is entered by
   a syscall with **no uid, name, capability or consent check**, and it appears in neither capability
   table. From build 2559 the inheritance rule promotes a voluntary parent's child to the MANDATORY
   state. So any program can reach the mandatory state by marking itself and launching a child. This
   is not an escalation, because the mark is a restriction and a caller gains nothing by taking it on,
   and it is not one of today's defects. It is escalated because the design reads as "the kernel
   decides who is an AI actor" and that is not what the code says, and because any future advisory or
   public claim that asserts kernel-exclusive control of the mark would be wrong as written.

6. **TWO MISSING MEASUREMENTS, each one run, each of which would settle a held-back finding.**
   (a) A marked program shrinking a file on a FAT volume on a pre-fix build and on a post-fix build:
   this is the only thing standing between item 3 of the not-filed list and an advisory. (b) The
   shipping file manager holding a paste contract on a booted machine, to confirm that it does reach
   the voluntary marked state at runtime: currently a complete static call chain and nothing more.

7. **A RECORD-KEEPING GAP, not a defect: the escrow coverage document is stale by one commit** and
   still describes two behaviours that the same day changed. It is the authoritative statement of what
   the protection covers, so it should not be quoted as current, and it says nothing about item 1
   above.


### 2026-10-04 daily review: the 60 commits after `18e546f2`. FIVE FILED, SEVEN ASSESSED AND NOT FILED.

SCOPE, stated so it can be re-derived. `git log 18e546f2..dev` is **60 commits** (dev head
`4ef70822`) carrying the tags `[guestmount]`, `[buildqnum]`, `[usbescrow]`, `[smbsign]`,
`[renamevol]`, `[jsdom]`, `[volaudit]`, `[openperm]`, `[boottests]`, `[provetwo]`, `[badexempt]`,
`[buildqcommit]`, `[smbcrypt]` and `[bootgate2]`, plus the day's CHANGELOG, `blame.md` and generated
index entries. Every claim below was checked against the diff and against the code at `dev`, not
against the commit message. `0045` to `0049` are filed above. The seven items after them are NOT,
and each is named individually, because a candidate that does not survive contact with the diff is a
finding about the record and not a failure of the review.

**THE DAY'S SHAPE, because it is the most useful thing a later reader can take from it.** Five
separate pieces of work found permission or protection checks that did not apply to a mounted USB
stick, and the obvious conclusion, "five advisories", is wrong. **Only one of them was exploitable on
an image anybody could run.** The others were authorization errors sitting underneath an operation
that **did not work on removable media at all**, so the kernel's answer to every question was
"refused", and a defect you cannot reach is a latent defect and not a vulnerability. Three of the
five were fixed in the very same commit that made the operation work, which is the same reasoning
that kept the 2026-10-03 pass from filing the dropped-AI-mark finding. The one that WAS reachable,
`0048`, is reachable precisely because the open path **did** work on a stick while its permission
check was carved out. **The discriminator is not "is the check wrong" but "does the operation
underneath the check reach the medium".** Applied honestly it turns five leads into one advisory, and
applied carelessly it would have inflated the table by four rows and the public count with it.

#### Build bracketing method for all five, stated once

Every "Affected" and "Patched in" above was established by `git merge-base --is-ancestor <fix>
<golden-commit>` against the archived goldens' own stamped commits, **never from a filename and never
from a date**. The build-to-commit map comes from the golden archive's rotation names, which the build
script derives from each image's own `/BUILDINFO.TXT` at rotation time; I spot-checked three of them
by loop-mounting the image read-only and reading the stamp, and all three matched their names, with
the loop devices detached afterwards on both paths. The current golden is **build 2577, commit
`559817b944b4`**, confirmed two ways: its own stamp, read off the image, and the build queue's new
commit column (job `j00001583`, exit 0), which is itself one of the day's changes. That ancestry test
also corrected one of my own readings: I had placed `[volaudit]`'s fix in golden 2573 from the commit
log's order and the test put it in **2574**, which is exactly the mistake the method exists to catch.
**A caveat on three of the "Patched in" numbers.** For builds 2569, 2570 and 2571 the build queue's
retained history no longer reaches back far enough to show their job exit codes, and the queue's
build-number column did not exist when they were made. The evidence that they are goldens rather than
burned numbers is each image's own stamp plus the archive's automatic rotation, which files a failed
build under a `FAILED-...-NOT-A-GOLDEN` name and did not do so for these three. That is weaker than
the exit-0 record available for 2572 onward, and it is stated rather than smoothed over.
**None of the five fixes is promoted beyond `dev`**, MEASURED: all five are absent from `testing`,
`stable` and `main`. Goldens are built from `dev`, so every "Patched in" above is true of the golden
and of nothing else.

#### 0048: the window, argued rather than asserted

The carve-out was introduced on 2026-08-23 and **first shipped in golden build 2020**, the golden
built from the very commit that introduced it, established by ancestry across all 505 retained
goldens rather than from the date. So for the whole life of removable-volume support, no permission
check of any kind ran on a Ring-3 open of a path on a stick. **But the measured inter-user breach
needs something that arrived only on 2026-10-04:** files created on a volume did not get a real
ownership row until the `[renamevol]` work, and four other verbs began enforcing against those rows in
the same change. The three images that carry per-user ownership on removable media **without** the
open-path fix are goldens **2572, 2573 and 2574**, and those are the ones where the demonstrated shape
is reachable. The row therefore records `<= build 2574` as affected, with the carve-out's own floor at
2020 named separately, because the earlier range is the same defect with nothing yet to enforce rather
than a different defect. **CORRECTED 2026-10-05 (sechist7). This paragraph previously asserted that the permission layer is
"inert on a default installation, because every account first-boot setup creates is an administrator
account and there is no non-root user tier yet". THAT WAS FALSE WHEN IT WAS WRITTEN, and this same
document already carried the measurement that disproves it.** First-boot setup creates the human
account at **uid 1000**, not uid 0: the policy constant is `FIRST_ADMIN_UID = 1000` in
`kernel/rustkern/sessionid.rs`, `users_create_first_admin()` takes the uid from it rather than
hard-coding zero, and a separate `root` account stays at uid 0 owning the system files with nobody
sitting in it. That flip landed on 2026-08-07, so the claim was already seven weeks stale. The
measurement is recorded earlier in this register, where the desktop session is observed at
`uid=1000 gid=1000` on the RED rig and the note says plainly that it "corrects several in-tree
comments that assert the session is uid 0". A register that contradicts itself on whether a whole
protection layer does anything is worse than one that says nothing, and the false copy was the one a
reader would reach while grading severity. **What the honest bound actually is.** A default install
has ONE human account, so the multi-user shape these rows measure needs a second account that the
system can create but the shipping default does not. What survives on a single-account machine is the
root-to-session boundary, which is real and is what MAYTERA-SEC-2026-0050 crosses. So severity here
is bounded by there being no second user to victimise, NOT by the layer being inert. **Severity was
argued, not assigned.** Graded by exposure on a default install it is low, because there is no second
user to victimise. Graded by what it removes, it is High: a complete absence of authorization on a
whole class of storage, with a measured confidentiality breach as well as a measured integrity one.
High is recorded with the precondition stated in the row, because grading every permission advisory
down for the inert-layer caveat would make the whole class read as harmless right up until the day a
user tier ships.

#### SEVEN ASSESSED AND NOT FILED

1. **`rename`, `mkdir`, `rmdir` and `unlink` were blind on a mounted volume, and the permission gate
   that guards a rename-over was being skipped there** (`9c105be1`). The availability half is real and
   is not security: MEASURED, all four returned an error and nothing moved, where the same operations
   worked on the root filesystem in the same boot. The authorization half is a real error of the right
   shape, and it is **NOT an advisory because it was not reachable on any image.** The gate asked the
   root handle whether the destination existed, read "absent" for a path on a volume, and so took the
   weaker "this is a create" branch and checked write access to the **folder** instead of remove
   rights on the **file**; but `rename` itself then failed, so nothing was destroyed. The destructive
   proof, an unprivileged uid renaming its own file over another user's file in a sticky directory and
   replacing its bytes, was obtained on a **purpose-built third kernel** with the routing fixed and the
   oracle still blind, a combination that **never shipped**, because the single commit that made
   `rename` reach a volume is the same commit that fixed the gate. Same reasoning as the 2026-10-03
   pass's item 1. One further half of that change is a fail-CLOSED defect and so not an advisory
   either: a file an unprivileged user created on a volume got no ownership row and was judged root's,
   which **denied** the creator the right to remove their own file. A rule enforced against a user on
   the basis of a fact never recorded is a real bug, and it refuses rather than permits.
2. **`sys_stat` was blind on a volume and the library's fallback invented an answer** (`84c9a92c`).
   REAL, and **NOT an advisory.** The syscall returned exactly the one error value the userland
   library still treats as "maybe unimplemented", so the library fell back to opening the path,
   seeking to the end, and **fabricating** a mode of regular-file-0644 unconditionally. A directory on
   a stick therefore came back to every application as an ordinary file, with a success return. The
   security consequence is a caller being led into a wrong destructive action rather than any access
   or disclosure, nothing leaks, and the fabrication grants nothing. It is also **not measured in the
   direction that matters**: the after-state has a transcript, the before-state does not, so "every
   directory was reported as an ordinary file" is a sound code-read chain and not a shown
   measurement, and this entry will not restate it as one. **The fabricating fallback is still in the
   shipping library**; the kernel fix stops this input reaching it. Escalated below.
3. **Two permission gates decided "creating or overwriting?" by asking whether the file existed, got
   "no" on a volume, and authorised against the folder instead of the file** (`84c9a92c`, the package
   write and the shell output redirection). Identical shape to item 1 and **NOT an advisory for the
   identical reason, which this time is MEASURED rather than reasoned.** The `[provetwo]` work found
   that the same commit made each gate's **decision** volume-aware and left the **writer** a few lines
   below still aimed at the root handle, so neither route could put a byte on a stick for anybody. The
   transcript shows the deny arms passing **and the matching allow arms and the usability arm failing
   too**, and the new runtime detector named the cause unprompted in the same boot. So "refused" was
   the answer to every question and the deny arms measured nothing. A reverted-oracle control arm
   proves the gate is load-bearing **once the writers work**, and that combination **never shipped**,
   because the oracle fix landed before the writer routing. The lead I was handed asked whether the
   shipped state was exploitable or merely unverified; it was **unverified, and in fact unprovable**,
   which is a stronger statement than the commit itself made.
4. **`[provetwo]` itself** (`fe26fc59`, `63eae1ca`, `dad8e287`, `6510d37d`). **NOT an advisory.** It is
   the evidence that closes item 3's caveat, plus a distinct wrong-disk-write correctness defect: a
   path on a volume satisfied the "is this on the root filesystem" test for every absolute path except
   two, so the bytes were written to the **system disk at the same spelling**, and the streaming
   variant of the package write **reported success** while doing it. That is lost data and wasted
   space rather than an escalation, because the permission database is keyed on the absolute path, so
   the row consulted and the row governing the shadowed file are the same row. That reasoning is
   sound and it is **INFERRED, not measured**; both provers used payloads below the threshold that
   selects the streaming path, so the one variant that silently succeeded in the wrong place is the
   one still not measured. Escalated below. Note also that routing those writers **creates write
   ability on a volume that did not exist before**, so the gates in item 3 became load-bearing for the
   first time in build 2576; they are now proven deny-and-allow.
5. **The boot gate "could not judge current images"** (`[bootgate2]`: `baae5bd3`, `3e949ef7`,
   `2eceeb1c`, `4ef70822`). **NOT A DEFECT, and recorded here precisely because the CHANGELOG reads
   like one.** A later reader meeting the phrase `FATAL: segment ... overlaps NON-claimable memory`
   will take it for a security finding. It is not. The gate in `dev` boots the current golden and exits
   0; the report came from a **month-stale copy** of the whole tool tree sitting at the path this
   project's own guidance names, and a stale tool at a plausible path does not error, it returns a
   WRONG ANSWER. The previously recorded root cause was wrong in **both halves**, which is the durable
   lesson: the memory type it named is mapped to the **allowed** class by the bootloader, so the named
   culprit could never have produced that message; and because the diagnosis was never narrowed, the
   cure became a cargo-cult rule to copy a list of fourteen devices verbatim, when the actual cause is
   **one** emulator argument, isolated in both directions. The structural fixes are real: the gate now
   refuses to issue a verdict unless it can name its own commit, and the stale copies are refusing
   stubs. **A fix that works is not evidence the diagnosis was right.**
6. **Nothing in the tree checked an exemption's stated reason against the code it described**
   (`23489246`). **NOT an advisory**, and the most useful meta-finding of the day. A sweep of 31
   exemption and baseline lists, 1604 rows with 739 read against the code, found 19 rows carrying a
   false clause, 11 of them false about **who calls the site**. The motivating row was marked as
   deliberate and correct, telling the next reader not to look at a write it described as a fixed
   internal path with no caller-supplied component, when the buffer was in fact filled from Ring 3
   fifty-eight lines above. No behaviour changed and no exploit is claimed, so no row. **The reason
   this register should carry it:** a baseline-list justification is not evidence, and any advisory or
   assessment that leaned on one is now known to rest on something with a measured error rate of about
   one in forty. Nothing in `0001`-`0049` does, as far as I can tell, but the next reviewer should not
   assume otherwise. `fe26fc59` separately deleted one such row whose justification was false in every
   clause and which had been parking the exact site that made the fix above it unprovable.
7. **The rest, briefly, all assurance or performance and none a defect.** The in-kernel boot
   self-test census (`be62736a`, `78cf6d65`, `2cfda82d`, `cb218788`): 108 self-tests run on every boot
   and nothing counted them, so one could stop running with every gate still green; proven
   destructively by stopping one and watching the boot gate exit 0 anyway, then fixed with a census
   that goes red. It is strong supporting evidence for the `0044` criticism that a self-test is not a
   control, and it means the self-tests that passed inside this day's own red arms were exactly as
   untrustworthy as that criticism said. The build-number and commit columns in the build queue
   (`752d6b2e`, `fd85ce3b`, `7979d7db`, `b15ef758`): provenance, and the commit column had been empty
   on 1298 consecutive rows including every golden since 2026-08-27 because it was computed inside a
   branch only the unused invocation path takes; it is what let this review confirm golden 2577's
   commit without trusting a filename. The browser script-path work (`3404b58c`, `ce2fb272`):
   performance, the path is linear and the residual cost was two thread-id system calls inside every
   mutex acquisition. The harness false-RED (`dad8e287`): an asynchronous log flush spliced a required
   line; the direction is safe, it yields false reds and never false greens, but **four pre-existing
   suites share the exposure** and that is live test-infrastructure debt. `6510d37d`, `045354ee`,
   `86a933cf`, `349bfed8`, `06a091f1`, `9b7a38f5`, `e283d51f`, `67623ec7`, `69916f50`, `89ae6e71`'s
   siblings and the generated `blame-INDEX.md` regenerations are scaffolding, baselining and
   bookkeeping. Two 2026-10-03 commits in range, `[smbreconn]` and the `[noaccount]` set, were already
   assessed by the previous pass and are **not** reassessed here.

#### THREE RECORD ERRORS FOUND IN THE DAY'S OWN WRITE-UPS, so they are not quoted later as fact

- **The `[usbescrow]` commit subject says four routes "failed open"; three destroyed data.** The four
  it counts are four **source functions** carrying the blind lookup, which is a different set from the
  four **tested arms**, and the two sets overlap in only two members. The rename arm destroyed nothing
  (rename could not reach a volume at all), and it **passes in both the red and the green run**, so it
  cannot distinguish a fixed kernel from a broken one and must not be counted as one of the proven
  routes. The package-write route was fixed in code and **never tested on a volume**. The honest
  sentence is: three routes demonstrably destroyed data with no copy kept, four code paths carried the
  blind lookup, and one of the five was never exercised on the medium.
- **The `[usbescrow]` CHANGELOG and `blame.md` both say neither the partial-write hook nor the
  `ftruncate` hook called the existence test. That is FALSE for the `ftruncate` hook**, and the diff
  removes exactly that line from it; `blame.md` contradicts itself three paragraphs apart. The
  substance is unaffected, because that route failed through both mechanisms, but the prose is wrong.
- **"89 SMB2 messages, all signed"** in the `[smbcrypt]` entry is loose: 73 of 89 carry the signed
  flag, and the 16 others are the pre-key negotiation and session setup, which cannot be signed, plus
  one asynchronous interim response, which is legitimately unsigned and corroborates the documented
  exemption rather than contradicting it. True of everything signable; not true as written.
  Separately, `[openperm]`'s "all 26 arms pass" counts the summary line as an arm; there are 25.
  And `[openperm]`'s "no golden was produced by this work, and nothing here has been promoted" went
  stale **38 minutes** after it was written, when golden 2575 was built from the commit that carries
  the fix. For bracketing, use the archive, never an entry's own sentence about its own freshness.

### ESCALATED 2026-10-04: live, unfixed, and deliberately NOT published with a reproduction

None of the following is on the public page with a recipe. Where the page needs to admit a gap it does
so in one sentence that says a gap exists and not how to drive it. Items 1 and 3 of the 2026-10-03
list are **closed** by `0046` and by the `0044` amendment respectively; items 2, 4 and 5 of that list
are unchanged and still live, and are not repeated here.

1. **LIVE, and it replaces `0044`'s old residual: a server can act as one account regardless of who
   signs in, and the protocol gives the client nothing to check.** A share configured that way
   authenticates the session as the real named user, returns success, leaves the guest flag **clear**,
   and still performs every read and write as the anonymous account. `0045`'s new guard reads the only
   identity the negotiated dialect exposes, so it cannot see this. MEASURED against a real server.
   Verifying the **session** identity is not verifying the **effective** one, and the distinction
   should be written into whatever the user is eventually shown. Named on the public page as an open
   gap, without a reproduction.
2. **LIVE: there is no pre-authentication integrity for SMB at all.** MEASURED by grep over `dev`:
   **zero** occurrences of the validate-negotiate request or any pre-authentication hash. The
   negotiation and session-setup exchange is unsigned, unencrypted and **never retroactively
   validated**, so an attacker in the path can rewrite the server's advertised dialect, security mode,
   capabilities and maximum transfer sizes. The running hash that fixes this is a 3.1.1 feature and
   3.1.1 is not implemented. **What is actually blocking a downgrade today is the ENCRYPTION policy,
   not the signing one**: require-encryption refuses any dialect other than 3.0 or 3.0.2, while
   require-signing accepts 2.x happily. So relaxing the encryption knob silently removes the dialect
   protection as well, which is a coupling nobody would guess from the knob's name. This is the one I
   would fix first.
3. **LIVE, low, and found by this review rather than by the change: the new identity guard's
   discriminator is a user-name string, not "were credentials configured".** A real, password-protected
   account literally named `guest`, `nobody` or `anonymous` folds to "the caller asked for anonymity"
   and gets **no** downgrade protection. The false-alarm trade is deliberate and documented; this
   consequence of it is not.
4. **LIVE: the fabricating `stat()` fallback is still in the shipping userland library.** On the one
   error value it treats as "maybe unimplemented" it opens the path, seeks to the end, and asserts
   regular-file-0644. The kernel no longer feeds it this input (item 2 of the not-filed list), so
   nothing currently reaches it, but the next syscall that returns that value for a path that is a
   directory will produce a confident wrong answer again. The fix is to stop inventing and return the
   error.
5. **LIVE and UNMEASURED: the streaming package-write guard is INFERRED only, and it is the one
   variant that silently succeeded in the wrong place.** Both provers used payloads below the
   threshold that selects that path, so it has never been exercised on a volume. One arm settles it.
6. **LIVE and NOT MEASURED FOR REACHABILITY: a large acknowledged backlog of volume-blind call
   sites.** The new lint's ledger carries, at `dev`, 51 rows marked as debt and 88 bulk-baselined
   without individual review. The largest single cluster is the kernel file browser, 17 sites, which
   means it cannot list, create, delete, rename, copy or move anything on a removable volume, and it
   is reachable from the desktop's own removable-media icon. The change states plainly that its
   reachability on a current golden is **not measured**. The later exemption sweep added further rows
   on Ring-3-influenced paths, including home-directory creation and the scheduled-task program path.
   The runtime detector **reports and does not refuse**, and the build lint claims only that a call
   hands the root handle a path it cannot prove is a fixed root path, not that the call is a bug.
7. **LIVE, infrastructure, and it undercuts future evidence: the boot gate currently only ever meets
   its own pinned fixture on this host.** The two new encryption self-test rows could not be derived
   the documented way because the gate cannot boot a current image there, measured with a stock golden
   as the control, so it predates the work that found it. The rows were spliced verbatim through the
   gate's own normaliser from a real passing boot, with the provenance recorded in the policy header.
   Every line is one a kernel really printed, but the fixture is no longer one capture. Note this sits
   beside the separate, resolved stale-copy story in item 5 of the not-filed list and must not be
   confused with it.
8. **A RECORD-KEEPING RISK, not a defect: the strongest evidence behind `0045`, `0047` and `0049`
   lives only on the build host and is already partly gone.** The packet captures, tampering proxies
   and serial transcripts are retained outside git, and the signing-era proxy has since been
   **overwritten** by the encryption one, so the exact arm that cleared the identity flag is no longer
   reproducible from a retained script, only from the serial logs, which do independently show the
   outcome. The `0044` entry already records one evidence claim whose artifact was destroyed. If these
   rows are meant to be auditable later, the artifacts need a durable home now rather than after the
   next cleanup.
9. **HOUSEKEEPING, not security: four orphaned shell loops are waiting for an `mwt land` that will
   never finish.** Each matches its own pattern, which is the documented false positive, so each will
   wait for ever. They are from earlier sessions, they are harmless, and this review did not kill
   another session's processes. Worth one sweep by whoever owns the host.


### 2026-10-06 daily review: the nine commits after the previous review. NO ADVISORY, AND THE REGISTER WAS THE THING THAT NEEDED FIXING.

SCOPE, stated so it can be re-derived. `git log e10a9ba9..dev` is **11 commits** (dev head
`98e22a6d`). Two of them are the previous review's own `[sechist7]` commits, so the new material is
**nine commits**, all tagged `[gatefixture]`. Five are `CHANGELOG.md`, `blame.md` and
`blame-INDEX.md` only. Four touch files, and **all six files touched are host-side build and release
shell scripts**: `tools/release-gate/release-secret-gate.sh`, `server/appstore/publish-guard.sh`
(a pinned hash only), `build/invariant-gate.sh`, `build/launched-app-gate.sh`,
`tools/release/stage-public-image.sh`, `tools/release/scaffold/stage-disk.sh`.

**ZERO kernel files and ZERO userland files changed, and none of the six ships in an image.**
MEASURED rather than assumed: `build/build-golden.sh` contains no line that stages anything from
`build/` or `tools/` onto the image mount, and none of the six scripts is installed onto it. That is
not a presentational point. **Every row in this register names an affected build range and a patched
build.** For a file that is in no image, that range is empty, so there is nothing an advisory row
could honestly say in either of those two columns. A finding about the build host is not a finding
about MayteraOS.

#### The one lead that deserved a real decision: a security gate that could not be shown to work

THE LEAD, and it is a fair one. `sgdisk` calls `sync(2)`, which waits for writeback on **every**
mounted filesystem rather than on the file it just wrote. A hung NFS mount on the build host turned
one such call into a **3.14 hour** uninterruptible wait that outlived the SIGKILL sent to it at five
minutes, because a task in uninterruptible sleep runs no signal handlers. The visible consequence was
that `tools/release-gate/release-secret-gate.sh --self-test` could not finish, so **for a period
nobody could demonstrate that the credential gate still goes red on a real secret.** That gate is the
control that exists because of the 2026-07-11 credential leak. "We cannot currently show that our
credential gate works" is a serious sentence and it was not dismissed on the strength of the brief
that offered it.

**ASSESSED AND NOT FILED. The reason is a measurement, not a judgement call about severity.**

1. **The production scanning path never contained the defect.** MEASURED by reading the gate as it
   stood at `3f3df420^`: every `sgdisk` invocation in that file is inside `make_image_canary()` or
   the hybrid-ISO canary builder, and both are reached **only** from `self_test()`. `scan_artifact()`
   and `scan_artifact_body()`, the functions that scan the artifact you are about to upload, contain
   no `sgdisk` call at all. So what was unavailable was the gate's **self-demonstration**, not the
   gate. The distinction is the whole decision: an advisory would have claimed the control was
   broken, and the control was not.
2. **It failed closed, in the direction that costs you a land and not a leak.** A wedged self-test
   returns 124 or does not return; `build/selftest-census.sh` then read that as a hang and **refused
   lands**. Nothing was permitted that would otherwise have been blocked.
3. **There is no crafted input and no attacker position.** The availability class in this file
   (`0024`-`0029`, `0039`-`0041`) requires a crafted input causing grossly disproportionate work in
   the shipped system, with a MEASURED before-and-after on a running machine. Here the trigger was a
   hung local NFS mount on one build host. Nothing a remote party supplies reaches it, and no
   `0024`-class before-and-after can be written for a path that is not in the product.
4. **MEASURED 2026-10-06 BY THIS REVIEW, not quoted from the change that claims it: the assurance
   gap is closed now.** `tools/release-gate/release-secret-gate.sh --self-test` in the build container with
   `TMPDIR` on tmpfs: **exit 0, 32 seconds wall, 30/30 correct**, with the gzipped-binary byte canary
   firing, which is the exact shape of the 2026-07-11 leak. The loop-mount checks correctly report
   themselves SKIPPED in a container that has no `/dev/loop-control`, and the gate says so rather
   than scoring them. A gate that can be demonstrated in half a minute is a gate people will
   actually run.

NOT FILED, and recorded here so the decision is auditable rather than invisible. If the same mechanism
is ever found on a path that **does** ship, or is ever shown to have let an artifact through, that is
a different finding and this note is not a precedent for waving it away.

#### Also assessed and not filed

- **The `sfdisk` near-miss on the public-image staging script.** Swapping `sgdisk` for `sfdisk` in
  `tools/release/stage-public-image.sh` would, on sfdisk's defaults, have silently shrunk the shipped
  root partition by about a megabyte while both tools exited 0 and wrote a valid table: the default
  1 MiB grain rounds a partition end down, and omitting a size stops one sector short of the last
  usable LBA. **It never shipped.** It was caught before landing by building the image both ways and
  diffing the artifacts, and MEASURED in the landed script: p2's size is stated explicitly, its value
  is read back from sfdisk's own reported `last-lba:` in a first pass rather than computed from a
  constant, and the table is read back and the build **refused** unless the type GUIDs, the names and
  both offsets are what were asked for. A defect that was caught before landing has no affected build
  and is not an advisory. It is noted here only so that the next reviewer does not file it off the
  CHANGELOG, which describes it at length precisely because the method that caught it is worth
  copying.
- **The two new `blame.md` entries** are process lessons (prove the artifacts are equivalent rather
  than that both commands exited 0; a lesson recorded is not a lesson reachable). No defect.
- **Two claims in the day's CHANGELOG were tested rather than accepted, and both hold.** "Zero
  `sgdisk` WRITE invocations in any `*.sh` in the repo": TRUE, the four remaining matches are a
  `command -v` probe, a dependency declaration and two message strings. "Zero bare `sync` under
  `build/` or `tools/release/`": TRUE as written. The five remaining matches in
  `build/build-golden.sh` are `sync -f <path>`, which syncs one filesystem and is the correct
  targeted form, not the whole-system call that caused the wedge.

#### What this review changed, and it was all in this file

The public page needed **no edit**: it already holds 42 rows, its prose already says 42, and the
reconciliation above closes exactly. **The drift was in this register.** The 2026-10-05 pass issued
and published `0050` correctly and then left behind three statements that had become false, which
this pass corrected:

1. the header's **next-free-ID sentence still read `0050`** after `0050` had been issued. That is the
   single most load-bearing sentence in the file and the one whose staleness minted a six-way
   collision on 2026-10-02. It now reads `0051`, re-derived three ways, and now says in terms to run
   the gate instead of trusting the prose;
2. the register/page agreement paragraph still described a page with **41 rows, a summary box and a
   Notes section**, and still instructed the next reviewer to recount a box and read a note that had
   been deleted on 2026-10-05 at the owner's instruction;
3. the Summary still referred a reader to that same deleted summary box for the published row count.

**The lesson is about mechanism, not about one pass being careless.** The ID sentence has a gate
behind it and the gate was right while the prose was wrong, which is why the sentence now points at
the gate. The page-shape paragraph has no gate behind it, and it went stale the moment the page was
rewritten by a different piece of work. **Any prose in this file that describes the SHAPE of the
public page is a standing staleness risk**, and the only reliable instruction is the one now written
into it: parse both files, do not inherit the paragraph.

#### Carried-forward live items

Nothing new was escalated by this review. The items under **ESCALATED 2026-10-04** and the
still-live items from **ESCALATED 2026-10-03** are unchanged, were not re-measured here, and are not
on the public page with a reproduction. Separately, and deliberately NOT a security finding: the
build host's NFS mount to the off-host backup target is hung, several processes are stuck on it
unkillably, and that is the infrastructure fault whose `sync(2)` interaction produced this whole
day's work. It is operational, the owner has it, and it is recorded in the CHANGELOG and `blame.md`
rather than here.


### 2026-10-07 daily review: the ten commits after `cf20e1f3`. ONE FILED (`0051`), THREE ASSESSED AND NOT FILED, ONE ESCALATED.

**SCOPE.** `git log cf20e1f3..dev` is ten commits, head `9fdd68da`. Three are generated
`blame-INDEX` regenerations and one is the previous review's own commit, so the new material is
**six commits on four branches**: `[smbpreauth]` (two code commits plus a policy pointer),
`[censuscpu]`, `[escrowreal]` and `[pubfailhard]`.

**FILED: `0051`**, the SMB pre-authentication finding, from `1a96ca71` and its follow-up `ca1996b4`.

#### 0051: how the build range was established, and the limits

**AFFECTED AND PATCHED WERE ESTABLISHED BY ANCESTRY, NOT BY DATE.** `git merge-base --is-ancestor`
was run for BOTH fix commits against each candidate golden's own stamped commit: `1a96ca71` and
`ca1996b4` are **absent** from golden 2580 (`833d43f8`) and **present** in golden 2581 (`9fdd68da`).
Both must be present for the fix to be usable, because `ca1996b4` corrects a freeze whose reset was
in the wrong function and which made any RECONNECT fail outright, so the first patched build is the
first golden containing the LATER of the two. Hence `<= 2580` affected and **2581** patched. Build
2579 never existed as a golden: its number was burned by a failed build, which is the mechanism
added after the b2481 collision and is working as intended.

**A STAMP IS NOT A PASS, so 2581 was checked rather than assumed.** Its buildq record is exit 0;
`build/invariant-gate.sh` passed AND its own self-test then proved it can still go RED on a broken
image; `build/repo-guard.sh` passed; and the shipped kernel rebuilds byte-identically from its
stamped commit. a test VM was refreshed to it.

**WHAT THIS ROW DOES NOT CLAIM.**

1. **Only one server implementation was ever exercised** (Samba 4.17.12). No Windows, macOS or
   Azure Files server was tested, by the change's own admission and not discovered here.
2. **AES-128-GCM and AES-GMAC are not implemented**, so a server that requires either is now
   REFUSED rather than downgraded. That is the safe direction and it is still a compatibility loss,
   and it is the most likely cost of the new `min_dialect=3.1.1` default.
3. **No default-configuration compromise was demonstrated.** Every tamper arm that produced a
   plaintext mount ran with `encrypt=enabled`. At the shipped default the pre-fix kernel refused.
   The severity is set from that, not from the loudest arm.
4. The pre-fix condition is INFERRED for dialects the arms did not negotiate: the arms measured
   0x0302, and the claim that 2.0.2 and 2.1 are equally unauthenticated rests on reading the
   protocol and the client's dialect list, not on an arm per dialect.

#### Assessed and NOT filed

1. **The shared key-derivation routine's missing bounds check** (`rustkern/smbsign.rs` `kdf_128`,
   widened 48 to 128 bytes and made to return `bool` in `1a96ca71`). **NOT AN ADVISORY, and the
   reason is a measurement, not a judgement call.** MEASURED on the pre-fix tree: there are exactly
   **two call sites in the whole kernel**, both in `rustkern/smbcrypt.rs`, both passing
   **compile-time constant** lengths of `label_len=11` and `context_len=10`, which is
   `4 + 11 + 1 + 10 + 4 = 30` bytes into a 48-byte scratch. Nothing about either length is
   attacker-influenced or even run-time variable, so **no shipped build could reach the condition at
   all**, and the exported entry point `smbsign_kdf128_shared()` ALREADY carried an explicit
   `> 48` size check; what it lacked was a way to report having used it. **AND THE CONSEQUENCE IS
   OVERSTATED WHERE IT IS DESCRIBED.** The change's own note says a longer label "would have written
   past the end". It would not: `buf[n] = ...` on a `[u8; 48]` in safe Rust is bounds-checked by the
   compiler, so the outcome would be a PANIC, not an out-of-bounds write. The defect that was real
   is the silent `void` return, which left the caller's buffer untouched and therefore
   indistinguishable from success; that is a genuine latent trap for the NEXT caller and it is
   correctly fixed. It is defence-in-depth on code that was correct for its actual callers, and by
   this file's own rule that does not earn a row.
2. **The two escrow routes named in the review brief** (rename-over, and shrinking an open file to
   nothing) **are not new material and are already covered.** No commit in this window touches
   escrow: the only `[escrowreal]` commit, `af9c892d`, changes `kernel/crypto/run-all-kats.sh` and
   `CHANGELOG.md` and nothing else. The truncating-open and `ftruncate` shrink routes on a removable
   volume are `0046`; rename-over on the root filesystem is recorded closed above (`3076f208`); and
   rename-over on a removable volume was assessed by the 2026-10-04 pass and deliberately not filed,
   because `rename` could not reach a volume at all before the same commit that fixed the gate, so
   nothing was destroyed. Nothing to add and nothing to duplicate.
3. **The census CPU-time classifier, the recovered image-mode self-test arm, and the KAT runner's
   failure reporting** (`5da5549c`, `af9c892d`). Assurance and tooling. No defect, no affected build
   range, and none of the touched files ships in an image. Same reasoning the 2026-10-06 pass applied
   to the `sync(2)` family, and it still holds.

**AND ONE CLAIM FROM A PREVIOUS DAY WAS RE-CHECKED RATHER THAN CARRIED.** `62751002` records, with a
measurement, that `tools/release-gate/boot-gate.sh` could not boot ANY image on the build host: every
candidate stopped at the same bootloader memory-overlap refusal. **That is no longer true.** MEASURED
in golden 2581's own build log: the boot gate ran for 115.78 s, booted its green fixture, drove the
first-boot wizard, logged in and returned `DESKTOP_LIVE` with the compositor presenting frames. The
gate was failing closed while it was broken, so nothing passed falsely in the interval, and the
assurance gap is closed now. No advisory: no shipped code was involved.

#### ESCALATED 2026-10-07: a LIVE self-test regression in the shipping kernel. NOT published, and NOT a vulnerability

**The SMB message-signing boot self-test FAILS on every boot of the current golden, and it has been
failing since the fix above landed.** This is an ASSURANCE defect, not a security hole: the
production code is correct and the test's expectation is stale. It is escalated rather than filed
because a self-test that is red is a control that is not reporting, and because it has a second
effect that is easy to miss.

**MEASURED three independent ways.**

1. By reading the landed tree at `9fdd68da`: `rustkern/smbsign.rs` case 25 asserts
   `smbsign_begin(st, 0x0311, key) != R_DIALECT` where `R_DIALECT` is 6, while `1a96ca71` changed
   that same function's 0x0311 arm to return the NEW `R_DIALECT_311_WRONG_ENTRY`, which is 11. The
   assertion can therefore never hold.
2. By reading the diff: `1a96ca71` introduced the new code and changed the return, and did not
   touch the case that checks it. The only later change to that file is a console string.
3. On serial, from the change's own experiment logs: **all nine** fixed-kernel arms print
   `[SMBSIGN] SMB2 message-signing selftest: FAIL (first failing case 25)`, and **both** stock-kernel
   arms print `PASS`.

**THE SECOND EFFECT, which matters more than the red line itself.** The self-test returns at the
FIRST failing case, and the file's cases run to **71**. So cases 26 through 71, which include the
known-answer vectors and the inbound-signature-verification cases that are advisory `0047`'s
demonstration, **have not executed on any boot since this landed**. The project currently cannot
show from a boot that its SMB signing is correct. INFERRED, not measured: I did not build a kernel
to confirm the early return, having read it rather than run it.

**A PREDICTED CONSEQUENCE, so it is not a surprise when it happens.**
`build/kselftest-boot.policy` carries an `ALWAYS` row for `SMBSIGN` whose passing regex requires
`PASS|OK|MATCH`, and `ca1996b4` recalibrated that policy to build 2581. Golden 2581 was never judged
against it, because the boot gate's arms that day ran against a pinned build-2554 fixture and the
policy correctly declines to judge an older artifact. **The next golden built at 2581 or above should
therefore go RED on this row.** That is the gate working. The fix is a one-character-class change to
a test expectation and belongs to whoever owns that code; this reviewer does not touch code.

#### Carried-forward live items

The items under **ESCALATED 2026-10-04** and the still-live items from **ESCALATED 2026-10-03** are
unchanged, were not re-measured here, and are not on the public page with a reproduction.

## Summary

**SCOPE: this Summary covers the MEMORY-SAFETY class only (`0001`-`0014`).** It is not a summary of
the file. The access-control (`0030`-`0037`), availability (`0024`-`0029`) and July (`0015`-`0017`)
classes have their own sections and their own counts. (CORRECTED 2026-10-06: this clause used to say
"the public page's summary box counts all 41 published rows". **The page has had no summary box since
it was rewritten on 2026-10-05**, and the count is now stated in the page's prose, where it reads
`42`. The correction history that follows is kept because it is the record of how this number went
wrong four times. CORRECTED 2026-10-03: this read `30`, which was true only for the few hours between
publishing `0030`-`0037` and publishing `0039`-`0041` on the same evening; the header block at the top
of this file has said `33` since, so the file disagreed with itself and the page was the correct side.
CORRECTED AGAIN 2026-10-04: `36` after `0042`-`0044` went up, then `41` after `0045`-`0049`. This
number goes stale every time a row is published, which is why the header block and not this line is
the place to read it). Stated because an unqualified "## Summary" sitting under the memory-safety sections is
exactly how its numbers came to be read as the whole file's and to disagree with the page's.

**COUNT CORRECTED 2026-10-02: this section said 12 where its own table holds 14 rows, and every
breakdown below was two short.** `0013` (JPEG `build_huffman`) and `0014` (IPv4 `ip_handle`) were
appended by the 2026-07-16 drift audit, and nothing in this section was revisited when they were. The
public page's summary box has counted all 14 for months (7 out-of-bounds writes, 7 reads plus
null-deref), so the register was the side that was wrong. The counts below are now the table's.

- **14 reachable memory-safety vulnerabilities found and closed**, all during work on the Rust port
  (#404). Precisely: **12 were found by the port itself** (its differential harness or the reading it
  forced) and **2 by the follow-on drift audit** that re-witnessed the port's output, `0013` and
  `0014`. **10 are closed by a Rust seam.** **FOUR are closed by a plain-C fix** and the Rust is not
  load-bearing for any of them: **0001** (the #476 ext2 guards, which landed before the fold; the
  seam mirrors them), **0012** (the `nfs.c` `data_len` clamp, which the source-bounded XDR seam does
  not cover), **0013** (the canonicality check and bound in `build_huffman`, which the shipped
  `jpeg_dht_rs` seam had **accepted**) and **0014** (the one-line `total_length < ihl` return in
  `ip_handle`, in a file the port never touched). "Closed by the Rust port" was once stated for all of
  them; that over-credited the Rust on four, not two, and the earlier correction of this bullet fixed
  only the two it knew about. See **Corrections**.
- By class: **7 out-of-bounds writes** (0003 ELF, 0004 FAT, 0005 PNG, 0006 JPEG, 0008 HTTP-chunked,
  0012 NFS, 0013 JPEG `build_huffman`). NOTE: 0006's DHT write is out of bounds of `huff_vals[]` but
  stays **inside** the enclosing allocation in the pre-port original (intra-object corruption,
  ASan-silent); the other six leave their allocation. **6 out-of-bounds reads** (0001 ext2, 0002 DHCP,
  0007 TLS, 0009 AAC/M4A, 0011 xattr, 0014 IPv4 `ip_handle`), and
  **1 NULL-pointer dereference / OOB read** (0010 HTTP/2, CWE-476 rooted in CWE-125). 0003/0005/0006/0008
  pair an integer overflow (CWE-190) with the write; 0014 pairs an integer UNDERFLOW (CWE-191) with
  the read. 7 + 6 + 1 = 14, which is the table's row count.
- By reachability: **5 remote** (0007 TLS pre-auth, 0008 HTTP-chunked, 0010 HTTP/2 pre-auth, 0012 NFS -
  a malicious NFS server the client mounts + reads - and 0014, unauthenticated from ANY host on the
  LAN with no IP address of its own), 1 network-LAN (0002 DHCP), 3 untrusted-image
  (0005 PNG, 0006 JPEG, 0013 JPEG), 5 local file/disk (0001 ext2, 0003 ELF, 0004 FAT, 0009 AAC/M4A,
  0011 xattr - 0004/0009/0011 reachable from Ring-3 via a syscall: readdir, play-audio, and
  get/listxattr). 5 + 1 + 3 + 5 = 14. This agrees with the public page, which counts 0014 among its
  remote advisories and 0013 among its untrusted-image ones.
- Every one is witnessed against the C reference and confined by the fix; each has a C hardening
  ticket. The witness is ASan for most, a **guard page** for 0006 claim 2 (ASan cannot see it), a
  **page-fault error code** for 0008's write claim (ASan reports only the read range), a NULL-deref
  for 0010 (the live `payload` is NULL when `flen==0`), and, for **0001 only**, a **labelled
  reconstruction**, because no artifact of the pre-#476 code survives. See **Corrections**.
  NOTE: 0012 is NOT a Rust-seam confinement - it is a DESTINATION over-write in `nfs.c` that the
  (source-bounded) XDR Rust seam does not cover, so it is fixed directly in C (clamp `data_len` to
  the destination `count`).
- 0001-0008 are fixed as of **build 817**, **0009 in build 819**, **0010 in build 822**,
  **0011 in build 823**, **0012 in build 824** (folded to golden in the build-824 fold), and
  **0013 and 0014 both in build 826**. 0013/0014 were missing from this list as well. "The current
  golden (build 817)" is struck: 817 was current when this bullet was written and is nearly 1,800
  builds stale, and a register that names a stale build as current invites a reader to conclude they
  are patched when the only thing that answers that is the per-advisory "Patched in" column. Users on
  a build below that column for a given advisory should update.
- Defense-in-depth ports that are NOT advisories but still remove a bug class by construction now
  include the JPEG dequant+IDCT seam (build 822, carries a reachable CWE-190 signed-overflow UB the
  Rust well-defines; plain-C hardening = #507), the theme-file line tokenizer (build 822), the
  WAV/RIFF header parse (build 823), the PEM base64 decoder (build 823, whose Rust wrapping-u32
  accumulator additionally well-defines a benign CWE-190-class signed-shift UB), and the batch-3
  ed25519 point-decode + XDR decode primitives (build 824). Batch-3 also assessed an SSH framing
  underflow as **not reachable** (dead scaffolding `ssh_transport.c`; the live `ssh2.c` already rejects
  it) -> no advisory, no seam integrated. **Build 824 is the last parser-tier Rust batch;** the kernel
  Rust port stops at the parser tier after this fold.
