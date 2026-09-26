# MayteraOS Security Advisories

TWO CLASSES OF FINDING LIVE IN THIS FILE, and they are not interchangeable.

1. **Rust-port memory-safety findings** (`MAYTERA-SEC-2026-0001` to `0014`), the original scope:
   reachable bounds defects found and closed during the incremental Rust kernel port (#404). The
   rules for that class are immediately below and are unchanged.
2. **Access-control and privilege-boundary findings** (`0015` onward), added 2026-09-04 with the
   capability API's Stage 0. These corrupt nothing; each is a missing authorization check on a
   correctly-written syscall. They have their own section, their own severity basis and their own
   evidence rules, because a memory-safety rulebook does not describe them: there is no crafted
   input, no ASan witness and no crash, only a caller doing something it should never have been
   able to ask for.

Numbering is one shared sequence in discovery order across both classes.


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

## Access-control and privilege-boundary advisories (capability API Stage 0)

**This section is a DIFFERENT CLASS from the table above, and the distinction matters when reading
it.** Everything above is a memory-safety defect found during the Rust port: a bounds bug that
corrupts or over-reads memory. Nothing below corrupts anything. Every one of these five is a
**missing access-control check on a correctly-written syscall**, and each was reachable by an
ordinary unprivileged Ring-3 application with **no capability, no consent, no audit record and no
visible indicator**. A fuzzer would never find them, because nothing crashes: the kernel does
exactly what it was asked, for a caller that should never have been able to ask.

Discovered by the inventory in `docs/SYSTEM_CAPABILITY_API.md` section 1 (code reading, 2026-09-04,
dev @ `bbebc5a0`), re-verified against `dev` @ `e860a883` before the fix, and **demonstrated on a
booted VM** by `userland/apps/caphole` (see "Evidence" below). Closed by that document's Stage 0.

Severity here is weighted by what the defect lets one application do to **another application or to
the user**, not by memory corruption. "Local" means a Ring-3 process already running on the machine:
that is the whole precondition, and on a desktop OS it is a low bar (any app the user installs, any
app the AI layer is asked to run).

| ID | Component (file) | Class (CWE) | Severity | Reachability / attack vector | Fixed by | Affected builds | Patched in |
|---|---|---|---|---|---|---|---|
| MAYTERA-SEC-2026-0015 | `SYS_GET_KEYBOARD` (195) dispatch (`proc/syscall.c`) | CWE-862 missing authorization -> CWE-522 credential exposure | **High** | **Local, and it defeats the only kernel-enforced consent gate the system has.** The syscall is a **destructive drain** of the global cooked key ring and was gated on nothing but `g_win16_owns_screen`. Any Ring-3 process could both **read and steal every keystroke in the system**. #745's entire trust story (`proc/elevate.h`) is that a requesting app "never draws anything, never receives a keystroke and never learns the password", because the **compositor** draws the elevation prompt; but the compositor reads that prompt's keys through this syscall (`compositor/main.c:825` -> `elevate_handle_key`), so **the password was readable by the very app that raised the prompt**. The lock screen uses the same loop and was exposed identically. `compositor/main.c:854` already said "a trusted prompt whose keystrokes are also delivered to the app that raised it is worth nothing"; that check was in the compositor, and an app simply does not go through the compositor | Dispatcher gate to the compositor principal via `capgate_is_compositor_rs()` (`rustkern/capgate.rs`). Refuses with **-13 (EACCES)**, deliberately distinct from the ordinary "ring empty" -1, so the refusal is observable from Ring 3 | <= build 2391 | see Evidence |
| MAYTERA-SEC-2026-0016 | `SYS_INJECT_KEY` (197) dispatch (`proc/syscall.c`) | CWE-862 missing authorization -> CWE-1021 UI redress | **High** | **Local.** Posts a synthetic `EVENT_KEY_DOWN` to the focused window with no privilege, ownership or capability check, while its **matched pair** `SYS_INJECT_MOUSE` (214) has been gated with `is_compositor()` since it was written (`gui/fb_syscall.c:1064`) and the header describes **both** as compositor-only. Two consequences: (a) it **forges elevation input credit**, because `sys_elev_request()` refuses with `ELEV_ENOINPUT` unless the WM recently delivered a **real** input event to a window the requester owns, and that stamp is written for `EVENT_KEY_DOWN`; an app that focuses its own window could inject a key, stamp its own credit, and **raise a password prompt the user never asked for**; (b) the event lands on whatever window has focus, so it drives **other applications' UI** | Same dispatcher gate. Refuses with -1, which is unambiguous here because the pre-fix case had **no failure path at all** and always returned 0 | <= build 2391 | see Evidence |
| MAYTERA-SEC-2026-0017 | legacy raw-index TCP family (`net/tcp.c` `tcp_get_conn`, dispatched via `proc/syscall.c` `tcp_*_kcr3`) | CWE-863 incorrect authorization | **High** | **Local, cross-process confidentiality AND integrity break.** `tcp_get_conn()` checked only `0 <= sock < 64` and `conn->active`. `owner_pid` was stamped on every connection (`tcp.c:447`) and read by **nothing except the Task Manager listing**. So a Ring-3 process could pass any index 0..63 to `SYS_SEND` (62), `SYS_RECV` (63), `SYS_TCP_CLOSE` (64), `SYS_CONNECT` (61), `SYS_TCP_STATE` (65), `SYS_LISTEN` (303) or `SYS_ACCEPT` (304) and **read, write or tear down another process's TCP connection, the in-kernel sshd's included**. A 64-entry table is a loop, not a guess. The #524 BSD family (`SYS_SOCK_*`) is per-process by fd, but wraps the same 64-slot table, so the raw-index family **bypassed that fd table entirely** | Ownership guard at the **Ring-3 chokepoint only** (the `static` `tcp_*_kcr3` wrappers, which the dispatcher alone reaches), using `capgate_owner_ok_rs()`. Deliberately **not** inside `tcp_get_conn()`, whose 13 callers include in-kernel consumers (`net/https.c`, `net/ftp.c`, sshd) that run where `proc_current()` is an unrelated victim process. New `owner_tgid` field so a sibling pthread of the opener is admitted. Refuses with a bare **-1**, deliberately NOT a distinct code, because a distinct code would be an oracle ("slot N is live but not yours") | <= build 2391 | see Evidence |
| MAYTERA-SEC-2026-0018 | `sys_shm_map()` (`ipc/shm.c`) | CWE-862 missing authorization | **High** | **Local, cross-process memory disclosure.** The map path checked range, not-free, not-already-mapped-by-me and `SHM_FLAG_EXCLUSIVE`, and **no credential of any kind**. Any Ring-3 process could map any allocated region 0..63 and **read another process's memory**, and write it too unless `SHM_FLAG_READONLY` was set (which only downgrades non-creators to `VMM_USER_RO`). Again a 64-entry table is a loop | Creator-thread-group check via the same `capgate_owner_ok_rs()`, plus a `creator_tgid` stamp. **No share list was added**: a complete census found the only `shm_map` callers in the tree (`libc/compositor_client.c`, `apps/ipc_test`) each map a region they created themselves, so there is no cross-process consumer to serve, and principle 7 of the design ("a capability with no consumer becomes fiction") applies | <= build 2391 | see Evidence |
| MAYTERA-SEC-2026-0019 | `/dev/` node open ordering (`proc/fdlayer.c` `sys_open_k`) | CWE-696 incorrect behavior order -> CWE-862 | **Medium** | **Local.** The `/dev/<name>` prefix was handled **before** the permission block and **returned**, so opening any device node **bypassed `perms_check()` entirely**. This was not a missing rule but a **structural exemption**, and it applied to every node the system registers now or later, not only to the USB serial adapter where it was noticed: `/dev/ttyACM0` (real read and write fops), `/dev/ptmx`, `/dev/pts/0..7`, `/dev/console`, `/dev/tty`, `/dev/null`, `/dev/zero`, `/dev/random`, `/dev/urandom`. An operator entry in `/CONFIG/PERMS.DB` for a device node parsed, stored, listed, and was **never consulted** | The `/dev` branch moved **after** `perms_check()`, with a signpost comment where it used to be. Paired with `perms_dev_node_seed[]` (`fs/perms.c`), a third seed table whose modes **preserve today's effective access** so no shipping app regresses | <= build 2391 | see Evidence |
| MAYTERA-SEC-2026-0020 | `/SCREENSHOT.REQ` file-drop trigger (`userland/apps/compositor/screenshot.c` `screenshot_poll`) | CWE-862 missing authorization -> CWE-200 information exposure | **High** | **Local, whole-screen capture with no consent.** `screenshot_poll()` ran once per compositor frame, opened `/SCREENSHOT.REQ`, read an **attacker-supplied absolute output path** from the body, and captured the compositor's whole composited backbuffer, which contains **every window on screen** (any open document, any password field mid-entry), to that path. There was **no uid check, no capability, no consent, no audit record and no toast**. The trigger was a file any Ring-3 app could create in `/`, so any app could screenshot the entire desktop to a path of its choosing. The file's own header even noted the identical risk for `testhook.c` and gated `testhook.c` at **compile time** (`compositor/Makefile`, `ifdef TESTHOOK`); the screenshot trigger had **no equivalent gate and shipped in every build**, including the golden | Stage 1 of the capability API. The `/SCREENSHOT.REQ` file poll is **deleted** (not merely checked): `screenshot_poll()` now dequeues from a **kernel-mediated queue** (`SYS_SCREENSHOT_POLL`, compositor-only via `fb_owner_is`). The only Ring-3 way into that queue is `SYS_SCREENSHOT_REQUEST`, which the **dispatcher capability chokepoint** (`syscall_cap_check`) gates on a live `screen.capture` grant, and whose handler (`gui/shotq.c`) then binds the request to the **exact granted path** and re-applies `perms_check(W_OK)`. A `screen.capture` grant is obtained only through the compositor-drawn consent prompt (`sys_cap_resolve`, compositor-only) and is journalled as a `GFSJ_OP_EDGE_ADD` edge. Removing the ambient path, not just adding a check, is design rule 4: a check can be forgotten, a deleted trigger cannot | <= build 2392 | see Evidence |
| MAYTERA-SEC-2026-0021 | Ambient USB-serial access via `/dev/ttyACM0` (`kernel/fs/perms.c` `perms_dev_node_seed[]`, seed `0666`) | CWE-862 missing authorization -> CWE-668 exposure of resource to wrong sphere | **Medium** | **Local, unmediated serial-adapter access.** After Stage 0 gave `/dev/` nodes a consulted permission policy, `/DEV/TTYACM0` was deliberately left seeded `0666` so no shipping app regressed, which means **any Ring-3 app could open and drive an attached USB CDC-ACM serial adapter** (a 3D printer, a microcontroller, a modem) with no capability, no consent and no audit. Serial is a bidirectional physical channel: read is data exfiltration from whatever is attached, write is arbitrary command of it. The kernel exposed the node with real read/write fops (`drivers/usb_cdc_acm.c`) and no ownership or capability check gated it | Stage 2 of the capability API. The seed is tightened to root-owned **`0600`** so the desktop uid (1000) is refused the raw open, and serial is re-issued only through a **mediated gateway** (`drivers/serialport.c`): `SYS_SERIAL_LIST` names published ports (a name + class, never a base address or `/dev` path), and `SYS_SERIAL_OPEN` opens one, gated by a **`serial.port`** grant at the dispatcher chokepoint (`syscall_cap_check`) and bound to the exact granted port name. A `serial.port` grant is obtained only through the compositor-drawn consent prompt and journalled as a `GFSJ_OP_EDGE_ADD` edge. Removing the ambient path (tightening the seed), not just adding a check, is design rule 4 | <= build 2396 | see Evidence |
| MAYTERA-SEC-2026-0022 | Unasserted `/TESTINPUT.TXT` marker (`drivers/testinput.c`; `build/invariant-gate.sh`) | CWE-489 active debug code / CWE-1188 insecure default -> CWE-862 missing authorization | **Low** (build-time defense-in-depth, not a Ring-3 hole in a normal golden) | **The host->guest debug injection channel could be silently armable in a shipped image.** `drivers/testinput.c` injects synthetic keyboard/mouse directly into the hardware input paths (so injected input counts as REAL, stamping the elevation input credit), and it arms whenever `/TESTINPUT.TXT` is present on the ESP. That marker is absent from a normal golden, so the channel is off; but NOTHING asserted its absence (docs/SYSTEM_CAPABILITY_API.md 1.2), so a golden that accidentally carried the marker (a stray file on the asset base, a bad overlay) would ship an ungated injection channel with hypervisor-serial reach, sitting beside the new capability-gated input.inject. The channel is not reachable by an in-guest Ring-3 app (the kernel owns COM1; apps have no port I/O), which is why this is a build-integrity gap rather than a direct Ring-3 escalation | Stage 3 of the capability API. `build/invariant-gate.sh` now FAILS (RED) any image whose ESP carries `/TESTINPUT.TXT` (factored `check_no_testinput`, with `--self-test-testinput` proving it goes RED on a marker image and GREEN without). The golden cannot be produced without the gate passing, so the marker's absence is now enforced by construction, not by hope. Independently, Stage 3's input provenance (`INPUT_SRC_HW`/`INPUT_SRC_SYNTHETIC`) ensures even the LEGITIMATE input.inject path cannot manufacture input credit: a synthetic event never stamps `elev_last_input_ms`, so an app holding input.inject cannot inject the keystroke that approves its own next grant or elevation | <= build 2397 | see Evidence |

**MAYTERA-SEC-2026-0020 evidence (Stage 1, measured on a booted VM, build 2394 / commit `e4d2c03e`).** `userland/apps/captest` (unshipped) demonstrates both arms on the running kernel. RED (ordinary app, no window, no user intent): `sys_cap_request` -> `CAP_ENOINPUT`, `sys_screenshot_request` -> `CAP_EDENIED`, `sys_cap_resolve(APPROVE)` from the app itself -> `CAP_EPERM` (an app cannot grant itself), `sys_cap_view` -> `CAP_EPERM`, query `held=0` throughout: "RED SUITE PASS: a grant cannot be obtained without consent." GREEN (window with real input credit -> the compositor's consent prompt approved with a real Enter delivered over the testinput channel): the kernel logged `[CAP] GRANTED: pid=34 uid=1000 screen.capture scope=/CAPTEST.BMP expires=171849ms edge=17` (grant issued on the uid-1000 requester, time-bounded, recorded as GraphFS journal edge 17 = `GFSJ_OP_EDGE_ADD`); the app's `sys_cap_query` returned `held=1`; the post-grant `sys_screenshot_request` passed the capability chokepoint (it reached `perms_check`, returning `-13 EACCES` on the write path, NOT `-3 CAP_EDENIED`); then `[CAP] REVOKED: pid=34 screen.capture edge=17` (`GFSJ_OP_EDGE_REVOKE`) and the next `sys_screenshot_request` was refused `CAP_EDENIED` at the chokepoint (revocation bit in flight). The boot self-test `[CAP] selftest OK` asserts the whole grant model + consent state machine on every boot; `[CAPGATE] selftest OK`; `[STAGE] DESKTOP_READY`; zero `[WQBLOCK]`; no kernel panic. The scope resolved to `/CAPTEST.BMP` (the harness's session home resolved to `/`), which is why the write hit `perms_check`; that is a filesystem-permission detail orthogonal to the capability gate, which opened correctly.

**CORRECTION (#permcreate / #shotwriter, 2026-09-24): that `-13` was NOT an orthogonal detail, and reading it as one cost two more months.** `perms_check(path, W_OK)` on a path that does not exist yet was answered from `perms_check_leaf()`'s no-entry default (root-owned 0755), which denies `W_OK` to **every** non-root uid. So `screen.capture` could never produce a file for a normal user, **whatever** capability the person granted: the gate opened onto a door that was nailed shut. The same `-13` reappeared verbatim on golden 2472 as `visiongame: FAIL screenshot request refused (-13 ...)` for uid 1000 writing into its own home. Two defects, both now fixed: (a) `rustkern/permpath.rs` decides a `W_OK` on a row-less name from the **parent directory**, which is what POSIX requires to create a file, strictly as an additional allow after the ordinary check has already denied; (b) `gui/shotq.c` additionally checks the **compositor's** identity, because the compositor is what actually writes the file, and returns `-14` (compositor cannot write there) or `-15` (no framebuffer owner) instead of letting a permission refusal surface as a 12-second timeout. An error code recorded in an evidence block as "orthogonal" is a finding nobody owns.

**MAYTERA-SEC-2026-0021 evidence (Stage 2, measured on a booted VM, build 2397 / commit `b083cd73`).** `userland/apps/sertest` (unshipped) demonstrates both arms and the ambient closure. **Ambient closed:** as the uid-1000 desktop principal, `open("/dev/ttyACM0", O_RDWR)` returned `-13` (EACCES) where the `0666` seed had let it succeed; the `/dev/` perms gate fired. **RED** (ordinary app, no window, no intent): `sys_serial_list` named 2 ports (`ttyACM0` class 2, `ttyS1` class 1 = COM2, a name + class only); `sys_serial_open("ttyS1")` with no grant -> `-3 CAP_EDENIED` at the dispatcher chokepoint; a spontaneous `sys_cap_request(serial.port,ttyS1)` -> `-4 CAP_ENOINPUT`; `sys_cap_request(serial.port,"nosuchport")` -> `-6 CAP_ESCOPE` (the kernel-owned enumeration check refuses a port it does not publish); a PATH-kind scope on a serial cap -> `-6 CAP_ESCOPE`; `sys_cap_resolve(APPROVE)` from the app itself -> `-10 CAP_EPERM`; query `held=0` throughout: "RED SUITE PASS". **GREEN** (window with real input credit -> the compositor's consent prompt approved with a real Enter over the testinput channel): the kernel logged `[CAP] GRANTED: pid=36 uid=1000 serial.port scope=ttyS1 ... edge=20` (issued on the uid-1000 requester, time-bounded, GraphFS journal edge 20 = `GFSJ_OP_EDGE_ADD`); `sys_cap_query` -> `held=1 scope=ttyS1`; `sys_serial_open("ttyS1")` -> fd `3` (the gate OPENED and installed a real fd); `sys_write(fd, 23 bytes)` -> **the 23 bytes `MAYTERA-SERIAL-CAP-OK\r\n` arrived on the VM's SECOND serial (COM2/ttyS1), captured on that socket** = positive proof the open reached hardware, not merely a return code; then revoke and the next `sys_serial_open("ttyS1")` -> `-3 CAP_EDENIED` (revocation bit in flight). The publish itself is measured: `[SERIALPORT] published 'ttyS1' (class 1, 16550 @ 0x2f8)` and `... 'ttyACM0' (class 2, USB CDC-ACM)` on every boot. **Boot regression: clean at 1, 2, 4 and 8 cores** - each reached `[STAGE] DESKTOP_READY` and `[CAP] selftest OK` (now asserting the serial cases too), with 0 `[WQBLOCK]` and 0 kernel panics. Stage 1's `captest` (screen.capture) still passes red-then-green on the same image (no regression): grant edge 31 issued, `sys_screenshot_request` reached `perms_check` (not the chokepoint), revoke refused it again.

### What Stage 0 does NOT claim

Stated here because the honest limits are part of the advisory, and because 0019 in particular is
easy to over-read.

- **0019 does not take serial (or any device node) out of ambient reach.** The seeded modes are
  permissive on purpose: landing the ordering fix with no seed rows would have broken the **shipping
  Terminal** (`/dev/ptmx` and `/dev/pts/N` opened `O_RDWR`, EACCES at uid 1000) and **`/APPS/PRINT3D`**
  (`/dev/ttyACM0` `O_RDWR`), and a security change whose first effect is that the Terminal stops
  working is a change that gets reverted, taking the hole with it. What 0019 delivers is the
  **mechanism**: a permission entry on a device node is now consulted, where before it was
  structurally impossible for it to matter. Tightening the policy is Stage 4's `serial.port`
  capability.
- **0015/0016 gate to the compositor; they do not introduce a capability.** An app that legitimately
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
  session is uid 0. 0019 only bites below root, so this measurement is load-bearing for it.

### Evidence

Unusually for this register, these five were **demonstrated on a booted VM, not only read from code**,
because `docs/SYSTEM_CAPABILITY_API.md` section 14 flagged that its own inventory was "read from code,
not demonstrated on a VM ... one grade weaker than a run".

`userland/apps/caphole` is the demonstrator: one unprivileged Ring-3 binary, unchanged between arms,
that prints `OPEN` per defect on a pre-fix kernel and `CLOSED` on a post-fix one. The cross-process
probes (0017, 0018) are genuinely two processes with distinct pids **and** tgids: the owner creates
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

0019's RED arm depends on the rig planting `/DEV/ZERO:0:0:0644` into `/CONFIG/PERMS.DB` before boot;
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

0017 and 0018 report IMPACT there, not a return code: a separate process with a distinct pid AND
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

**0019's decisive evidence is the kernel's, not the probe's.** The probe initially reported 0019
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
because 0018's demonstration is what tripped it.

**"Patched in" is deliberately a commit, not a build number.** These landed on `dev` as `f1f3dc02`
(the gates) and `7fc087f5` (the demonstration and the ledger). The first golden to carry them should
be recorded here once it is built; until then a build number would be a guess.


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


**MAYTERA-SEC-2026-0022 evidence (Stage 3, input.inject, measured on a booted VM, golden build 2398 kernel / commit `6de46840`, VM 2599 booting the golden over USB, driven by the testinput channel armed only on this throwaway image).** `userland/apps/injtest` (unshipped) demonstrates all three properties, and captest/sertest are re-run for regression. **RED** (ordinary uid-1000 app): `sys_cap_inject_key`/`sys_cap_inject_mouse` with no grant -> `-3 CAP_EDENIED` at the dispatcher chokepoint; the raw compositor-only `SYS_INJECT_KEY` (197) -> `-1` (no ambient inject path for a non-compositor); `sys_cap_request(input.inject, PATH kind)` and `(input.inject, WINDOW "other")` -> `-6 CAP_ESCOPE`; spontaneous `sys_cap_request(input.inject, self)` -> `-4 CAP_ENOINPUT` (with the durable `[CAP] REFUSED spontaneous request: pid=35 input.inject (no input in 10000ms)`); `sys_cap_resolve(APPROVE)` from the app -> `-10 CAP_EPERM`: "RED SUITE PASS". **GREEN** (window with real input credit -> compositor consent approved with a real Enter over testinput): `[CAP] GRANTED: pid=35 uid=1000 input.inject` (scope=self, journalled edge); `sys_cap_query -> held=1 scope=self`; a synthetic key injected into the app's OWN window was read back as `EVENT_KEY_DOWN` keycode 0x5A (`gotk=1`, injection WORKS); after `sys_cap_revoke` the next `sys_cap_inject_key` -> `-3 CAP_EDENIED` (revocation bit in flight). **THE SELF-ESCALATION PROOF** (the point of the stage): after the grant, the app waits a TSC-wall-clock-guaranteed >=18s so the real input credit expires (a BASELINE `sys_cap_request(screen.capture)` with NO injection then returns `-4 CAP_ENOINPUT`, confirming the credit is gone), then injects 5 synthetic keys into its own window and immediately requests another capability: `sys_cap_request(screen.capture) = -4 CAP_ENOINPUT` and `sys_elev_request = -8 ELEV_ENOINPUT` -> "SELF-ESCALATION PASS". A synthetic event does not stamp `elev_last_input_ms` (the one stamp site, `syscall.c` `user_window_queue_event`, is gated on `INPUT_SRC_HW`), so an app holding input.inject cannot inject the keystroke that credits its own next grant or elevation. Separately, while the BASELINE prompt was open the follow-on injection returned `-2 CAP_EBUSY` (consent-surface guard: an app cannot inject while a prompt is up), and the app cannot inject the Allow for its OWN input.inject prompt (it holds no grant yet). **Boot regression: clean at 1, 2, 4 and 8 cores** - each reached `[STAGE] DESKTOP_READY`, `[CAP] selftest OK`, `[CAPGATE] selftest OK`, `N of N cores online`, with 0 `[WQBLOCK]` and no kernel panic (the 6 `PANIC` log lines are the benign `PANIC_LOG_ARM` pre-allocation of `/boot/PANIC.TXT`, not a crash). **No regression:** captest (screen.capture) passes RED+GREEN at 1 core (grant edge issued, `sys_screenshot_request` reached `perms_check` returning `-13`, revoke refused it `-3`); its R5 was updated from input.inject (now issuable in Stage 3) to net.connect (still `-7 CAP_EPOLICY`). sertest (serial.port) passes RED+GREEN at 2 cores (ambient `/dev/ttyACM0` -> `-13`; grant edge issued; `sys_serial_open(ttyS1) -> fd 3`; 23 bytes `MAYTERA-SERIAL-CAP-OK` written; revoke refused it `-3`). The `/TESTINPUT.TXT` invariant-gate check is proven with `build/invariant-gate.sh --self-test-testinput` (RED on an ESP carrying the marker, GREEN without), and the shipped golden carries no marker.


**MAYTERA-SEC-2026-0023 (Stage 4, CAP_SCOPE_WINDOW_TARGET, consented CROSS-APP input.inject).** Extends
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
