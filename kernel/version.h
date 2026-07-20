// version.h - MayteraOS Version Information
#ifndef VERSION_H
#define VERSION_H

// Version format: MAJOR.MINOR.PATCH (e.g., 1.8.2)
#define MAYTERA_VERSION_MAJOR    1
#define MAYTERA_VERSION_MINOR    95
#define MAYTERA_VERSION_PATCH    0

// Build number (increment for each build)
// #418 iMac debug/test kernel: crash-dialog CR3 fix + /STAGE.TXT + /PANIC.TXT
// breadcrumb instrumentation + PCI-claim /DEVLOG.TXT logging + hotplug_init()
// wiring (FAKE-audit fix). Bumped to a clearly-new build so the next iMac
// boot's /DEVLOG.TXT and /PANIC.TXT unambiguously identify THIS kernel
// (resolves the earlier b644-vs-b654 stale-stick confusion).
// #427 FAKE-audit CRITICAL fix: /dev/urandom was a deterministic xorshift64
// seeded once from RDTSC (predictable, write()s ignored). Replaced with a
// real CSPRNG (crypto/csprng.c: HMAC-DRBG seeded from RDSEED/RDRAND+RDTSC
// jitter+PIT ticks, periodic reseed, write() now stirs entropy). Also adds
// /dev/random (same backing, non-blocking, modern-Linux semantics). Bumped
// again to 665 to include a boot-time CSPRNG self-test (two samples logged
// + differ/non-zero check) for on-device verification.
// #fix-tls-certverify CRITICAL fix: TLS never actually validated server
// certificates. cert_store.c's rsa_verify_pkcs1()/ECDSA path both
// unconditionally `return 0` (success) without doing any real crypto math;
// tls.c/tls13.c logged "Received server certificate" and threw the bytes
// away; https.c hardcoded tls_set_verify(0,1) ("don't verify for now"); the
// TLS 1.3 Finished verify_data was never checked; the TLS 1.2 path sent
// random bytes as a fake "encrypted premaster secret". Fixed: real RSA
// (crypto/rsa.c, already used by SSH) + new real ECDSA P-256/P-384
// (crypto/ecdsa.c) signature verification; full chain-to-trusted-root +
// notBefore/notAfter (real RTC clock, not a hardcoded date) + hostname/SAN
// checks wired into both the TLS 1.2 and TLS 1.3 Certificate-received
// paths; a bundled CA trust store (/CONFIG/CACERTS.PEM, ~121 Mozilla roots)
// loaded via cert_add_trusted at https_init(); TLS 1.3 server Finished
// verify_data now actually checked (aborts on mismatch); TLS 1.2's
// non-functional static-RSA key exchange removed outright (fails closed -
// TLS 1.3 is the supported/working path); https.c now verify=1 by default.
// #428 FAKE-audit AHCI fixes (drivers/ahci.c): CRITICAL - NCQ read/write
// (FPDMA QUEUED) polled PxCI for completion, which the HBA clears as soon as
// the command FIS is accepted, long before the DMA transfer finishes ->
// silent data corruption. Now polls the issued tag's bit in PxSACT instead
// (cleared by the device's Set Device Bits FIS); the non-NCQ DMA path keeps
// polling PxCI (still correct there, proven no-regression). HIGH -
// wait_cmd_complete detected PxIS errors but never RW1C-cleared PxIS/PxSERR
// or restarted PxCMD.ST, so one drive error permanently wedged the port;
// added ahci_port_recover() (stop engine, clear IS/SERR, drop stuck CI/SACT
// bits, CLO if BSY/DRQ, restart engine). HIGH - command timeout was a fixed
// loop-iteration count with zero real-time relationship; now bounded by an
// RDTSC cycle budget (~5s, conservatively assuming an 8 GHz ceiling so the
// bound is never shorter than intended on any real CPU) - deliberately not
// timer_ticks/g_timer_hz, since AHCI init runs before sti() and timer_ticks
// is frozen that early, and deliberately not an io_wait()-based busy-wait
// (ahci_delay()), whose real cost under virtualization (confirmed by
// booting an early version of this fix on a real q35+AHCI VM) turned a
// microsecond poll into a multi-minute stall once called on every command.
// MED - 4Kn logical sector size read IDENTIFY words 118/119 instead of the
// correct 117/118. Added ahci_selftest_ncq() (drives ahci_read_ncq/
// ahci_write_ncq directly, separate scratch LBA from ahci_selftest()),
// called from ata_init(). VERIFIED on VM 2500 (q35, two virtio SATA/AHCI
// disks): both selftests PASS on real write+readback round-trips -
// "[AHCI] selftest: write+readback of LBA 2047992 -> PASS" (non-NCQ, no
// regression) and "[AHCI] selftest_ncq: NCQ write+readback of LBA 2047984
// -> PASS" (NCQ, the fix). Full desktop boot with networking/TLS confirmed
// afterward.
// #427 FAKE-audit CRITICAL fix (exec/elf.c): the PIE/dynamic ELF loader
// applied ZERO relocations - it only added a flat address shift to PT_LOAD
// segments and the entry point, never touching PT_DYNAMIC. Added a real
// relocation engine: parses PT_DYNAMIC (DT_RELA/DT_REL/DT_JMPREL +
// DT_SYMTAB/DT_STRTAB/DT_RELACOUNT) and applies R_X86_64_RELATIVE, GLOB_DAT,
// JUMP_SLOT, R_X86_64_64, PC32/PLT32, 32/32S, and best-effort TLS
// (DTPMOD64/DTPOFF64; TPOFF64 logged-not-applied, no TLS block yet) - in user
// address spaces via the same temporary CR3-switch technique already used
// for segment copies (elf_load_user, the path proc_create_user() actually
// uses), and against kernel-owned memory for the unused elf_load/
// elf_load_full APIs. Also fixes the PIE load base itself: USER_SPACE_START
// (4MB) sat inside the deep-copied PML4[0]'s pre-existing kernel mappings, so
// the very first write into a freshly "allocated" PIE segment silently page-
// faulted (CR2 in the 0xBFC0xxxx range) before any relocation code ever ran;
// PIE images now load at PIE_USER_BASE (0x90000000), inside the same 2-3GB
// PDPT[2] window every fixed-base app already uses successfully. Verified
// with a hand-built -shared/no-dynamic-linker "static PIE" test ELF (real
// PT_DYNAMIC + DT_RELA, R_X86_64_RELATIVE fixups for a global string-pointer
// table) that prints the correct strings only if the fixups were applied;
// confirmed no regression to existing fixed-base (non-PIE) app loading. This
// is the prerequisite for a real dlopen()-style loader and CPython C
// extension loading (#359).
// #fix-tls-certverify (#232/#427): bumped to 669 for the final, clean-built,
// LIVE-VERIFIED TLS certificate-validation kernel. Valid public HTTPS sites
// (coingecko/yahoo/wttr.in/musicbrainz/archive.org) verify OK + fetch 200;
// a self-signed and a wrong-host cert are both REJECTED with a certificate
// error (proven on VM 2299 against an openssl s_server bad-cert endpoint).
// #433 xHCI HID enumeration race fix: mark-port-enumerated-on-success +
// bounded retry, warm-reboot per-port PP off->on power-cycle, CONFIG_EP return
// check + retry for HID interrupt-IN, and a periodic port re-scan worker.
// #71 / Cirrus CS4208 audio: /AUDIOLOG.TXT diagnostic (codec identity + output-
// path EAPD/amp/GPIO + output-stream DMA state), plus the CS4208 speaker-amp
// enable (EAPD on the speaker pins + GPIO0 mask/dir/data on the AFG node,
// mirroring Linux patch_cirrus.c). fresh5 diagnostic image.
// #444 ATA DMA data-integrity/stability fix: drivers/ata.c's global I/O lock
// (g_ata_io_lock) and fs/ext2.c's block-cache lock (g_e2c_lock) both used a
// non-atomic lazy "if (!ready) { spinlock_init(); ready = 1; }" pattern. Under
// concurrent disk load (CPython loading many stdlib modules from ext2 at
// boot) two threads could race that check, letting a re-init stomp the lock
// state out from under an in-flight holder - producing concurrent DMA
// transfers that share the single global dma_buffer/PRD table (zero-filled
// read holes) and, in the ext2 cache, silently discarding just-cached valid
// blocks. Escalated occasionally to a GPF via a corrupted PRD physical
// address. Fixed by statically initializing both locks (SPINLOCK_INIT, same
// pattern as net/net.c and sync/futex.c) so there is no runtime init window
// at all, plus proper double-checked locking around the ext2 cache's
// one-time slot allocation. Added memory/write barriers around the DMA
// buffer copy as defense-in-depth for completion coherence.
// #444 part 2 (found by hammer-testing the part-1 fix): proc/syscall.c's
// legacy fd table (fd_used[]/e2fd[]/smbfd[], only 16 system-wide slots) let
// sys_open() scan for a free slot and populate it with ZERO locking. Two
// processes opening DIFFERENT files concurrently could both claim the same
// slot; whichever finished populating it last won, so the loser's process
// silently read (and, on close, freed) the WINNER's file content instead of
// its own. Proven live: 4 independent processes each reading a different
// known-content file, 3 of them received the 4th process's exact bytes.
// This is likely the dominant real-world cause of the CPython "corrupt
// reads" symptom (many concurrent opens of different stdlib .py files).
// Fixed by claiming the slot (fd_used[i]=1) atomically under a new
// g_legacy_fd_lock in the same scan that finds it, with every failure path
// releasing the slot again (FD_FAIL macro / legacy_fd_release()) so it can't
// leak. The lock is only held for the tiny scan+claim, never across the
// (possibly slow/network-blocking for SMB+NFS) population work that follows.
// #fix-ssrf-contentlength FAKE-audit fixes (net/https.c, net/wget.c):
// HIGH - https_get()/wget's redirect-following (http_resolve_url +
// wget_fetch/wget_execute_with_redirects) previously followed a 3xx
// Location header with NO scheme or host validation at all, letting a
// remote page redirect the kernel HTTP client at internal targets
// (192.168.x.x, 127.0.0.1, 169.254.x.x/cloud-metadata). Added
// https_host_is_private()/wget_host_is_private() (resolve + check
// 10/8,172.16/12,192.168/16,127/8,169.254/16,0/8) gating every redirect
// site in both clients; a public-origin request can never be redirected
// onto a private/loopback/link-local host, unless the ORIGINAL request was
// already to one (e.g. local Home Assistant). https.c additionally refuses
// an https->http scheme downgrade (https_parse_url only accepts https://);
// wget.c gets an explicit wget_redirect_allowed() downgrade check too.
// wget_fetch() previously had NO redirect-count cap at all (unbounded
// recursion on an attacker redirect loop) - now shares HTTP_MAX_REDIRECTS
// with wget_execute_with_redirects() via a new orig_is_private-threading
// wget_fetch_internal().
// MED - neither client checked a received body's length against its
// advertised Content-Length, so a short/truncated response was returned as
// SUCCESS with a partial body (corrupting HA/api-states and pip-metadata
// JSON). Both https_get()/https_post() and wget's http_request() now treat
// body_len < Content-Length as an error (HTTPS_ERR_TRUNCATED /
// WGET_ERR_TRUNCATED), not success. Chunked-transfer "completion" was
// detected by scanning for the raw substring "0\r\n"/"0\r\n\r\n" anywhere in
// the received buffer (false-triggers on binary/text bodies that legitimately
// contain that byte sequence mid-payload, truncating them) - replaced with a
// real chunk-framing walk (https_chunked_is_complete/wget_chunked_is_complete:
// hex size + CRLF + payload + CRLF, repeated to the 0-size terminator + its
// trailer-end blank line) in both clients, plus wget.c's http_decode_chunked()
// itself was rewritten to be strictly bounds-checked and to treat "ran out of
// data" as an error instead of silently treating it as a valid terminator.
// #71: iMac14,4 real-hardware fixes - HDA MSI interrupt (falls back to an
// LPIB-poll worker when no MSI capability exists) + tightened USB HID
// interrupt-IN poll cadence (usb_hid.c) and compositor input-sampling
// decoupled from the render frame rate (compositor/main.c), to cut real
// mouse-pointer lag. See CHANGELOG.md / blame.md #71 for the full writeup.
// #699 FIX: build 698's hda_start_poll_worker() call (inside hda_init(),
// called from audio_init() long before proc_init() runs) called
// proc_create() before the process subsystem existed, corrupting the
// scheduler's ready queue for every process created afterward whenever a
// real HDA controller was present at boot. This silently starved sshd's
// listener thread before it could ever call tcp_listen() - TCP itself still
// worked (net_init() runs independently), so a connect attempt got a plain
// refusal/drop with zero bytes and no banner. Fixed by deferring the poll
// worker's proc_create() to hda_start_poll_worker_deferred(), which main.c
// now calls right after proc_init(), plus a defensive reset of the ready
// queue head/tail pointers inside proc_init() itself. Audio + MSI + mouse
// fixes from #71/#698 are unchanged. See CHANGELOG.md / blame.md #699.
// #476 SECURITY (MEDIUM) fix: the ext2 directory-entry parsers walked
// disk-controlled rec_len/name_len with no bounds checks, so a crafted or
// corrupt ext2 image made any path lookup read past the kmalloc(block_size)
// buffer (kernel-heap over-read / info-leak oracle + possible page-fault DoS).
// ext2_lookup() had zero guards; ext2_readdir_ino/insert/unlink read the
// entry header BEFORE their partial guard. Added consistent guards to all
// four fs/ext2.c dir walkers: the 8-byte header must fit (off + 8 <=
// block_size) before it is read, rec_len must be >= 8 and within the block,
// and the name field must fit before the compare. No behavior change on
// well-formed directories (independently proven byte-identical over 516k
// well-formed blocks by the Rust-port differential); surfaced by that
// Rust-migration PoC audit (#404). See CHANGELOG.md / blame.md #476.
// b794 (#404 / #485 Phase C): ext2 directory-block parser folded through Rust
// (ext2_dirblock_find_rs) behind -DRUST_EXT2_DIRFIND, guards aligned byte-for-
// byte with the #476-hardened C; boot-time [RUST-DIFF] ext2_dir differential
// self-test over valid + malformed blocks. Strangler, trivial rollback.
// b795 (#404 / #486 Phase D): IP-layer transport checksums folded through Rust.
// tcp_checksum LIVE via tcp_checksum_rs under -DRUST_TCP_CHECKSUM (kept
// tcp_checksum_c for rollback); udp_checksum_rs added + proven but STAGED
// (udp_send still emits checksum 0, unchanged wire behavior) under
// -DRUST_UDP_CHECKSUM. Boot-time [RUST-DIFF] tcp/udp differential self-tests.
// b796 (#404 / #487 Phase E): SHA-256 block compression core folded to Rust
// (sha256_transform_rs, rustkern.rs) live under -DRUST_SHA256; sha256_ctx_t
// stays C. Boot-time [RUST-DIFF] sha256 self-test (NIST KAT via live sha256()
// API + 20000-vector transform_rs vs transform_c differential).
// b797 (#404 / #488 Phase F): SHA-512 block compression core folded to Rust
// (sha512_transform_rs, rustkern.rs) live under -DRUST_SHA512; sha512_ctx_t
// stays C (init/update/final pass ctx->state, struct never crosses FFI). Boot-
// time [RUST-DIFF] sha512 self-test (NIST KAT via live sha512 path + 20000-vector
// transform_rs vs transform_c differential) + [RUST-PERF] sha512 RDTSC bench.
// b798 (#404 / #489 Phase G): MD5 block compression core folded to Rust
// (md5_transform_rs, rustkern.rs) live under -DRUST_MD5; md5_ctx_t stays C
// (init/update/final pass ctx->state, struct never crosses FFI). MD5 decodes its
// message words LITTLE-endian (unlike the big-endian SHA schedule); the Rust
// port matches the C x[i] decode exactly. Boot-time [RUST-DIFF] md5 self-test
// (RFC 1321 KAT via live md5() API + 20000-vector transform_rs vs transform_c
// differential) + [RUST-PERF] md5 RDTSC bench.
// b800 (#404 / #491 Phase I): ChaCha20 block core folded to Rust
// (chacha20_block_rs, rustkern.rs) live under -DRUST_CHACHA20; chacha20_ctx_t
// stays C (init/setkey/counter/XOR-stream pass ctx->state, struct never crosses
// FFI). The 20-round (10 double-round) quarter-round core + LITTLE-endian 64-byte
// serialization match chacha20_block_c exactly. Boot-time [RUST-DIFF] chacha20
// self-test (RFC 8439 section 2.3.2 KAT via the live chacha20 API + 20000-vector
// chacha20_block_rs vs chacha20_block_c differential) + [RUST-PERF] chacha20 RDTSC
// bench. ChaCha20 backs TLS ChaCha20-Poly1305.
// b802 (#404/#493 Phase K): HMAC construction (RFC 2104) ported to Rust behind
// -DRUST_HMAC. The live one-shots hmac_sha256/hmac_sha384/hmac_md5 route to
// hmac_sha256_rs/hmac_sha384_rs/hmac_md5_rs (rustkern.rs); the RFC 2104 ipad/opad
// wrapper is the new Rust logic, reaching the already-Rust hash cores via the C
// hash glue (opaque bounded ctx buffer, no C struct duplicated in Rust). Boot-time
// [RUST-DIFF] hmac (RFC 4231 SHA-256/384 + RFC 2202 MD5 KAT via the live API +
// 21006-vector rs-vs-c differential over all 3 variants) + [RUST-PERF] hmac RDTSC
// bench. hmac_sha256 backs TLS 1.3 Finished key schedule + CSPRNG HMAC-DRBG. This
// COMPLETES the crypto tier (sha256/sha512/md5/md4/chacha20/aes/hmac all Rust).
// b803 (#404/#494 Phase L): FIRST Tier-2 untrusted-wire-input parser folded to
// Rust. The incoming ICMP packet parse/validate of net/icmp.c (icmp_parse) routes
// to icmp_parse_rs (rustkern.rs) under -DRUST_ICMP; icmp_parse_c kept verbatim for
// rollback + the differential. The reply-send / echo-reply construction stays in
// C. The Rust removes two weaknesses of the verbatim C BY CONSTRUCTION: (1) the
// missing UPPER bound on the attacker-controlled length (C fed len unchecked into
// ip_checksum(buf,len) + a uint8_t reply[length] kernel-stack VLA; Rust rejects
// len>1500=MTU before any deref), and (2) the in-place mutation of the const
// untrusted RX buffer during checksum verify (Rust folds over an immutable slice,
// zero writes). Boot-time [RUST-DIFF] icmp (valid echo req/reply every len 8..1500
// + too-short + bad-checksum, rs==c) + [RUST-SEC] icmp (oversize confinement) +
// [RUST-PERF] icmp RDTSC bench. Starts Tier 2; net/arp.c is next.
// b804 (#404/#495 Phase M): SECOND Tier-2 untrusted-wire parser - incoming ARP
// frame parse/validate folded to Rust (arp_parse_rs, live under -DRUST_ARP). Pure
// parse extracted out of arp_handle(); DAD/cache-add/reply-send stay in C. The C
// reference is already length-gated + fixed-offset + read-only (no reachable OOB),
// so this is HONEST defense-in-depth: Rust removes the unchecked-wire-pointer-
// arithmetic CLASS by construction. Boot-time [RUST-DIFF] arp (~512 vectors:
// well-formed req/reply + too-short + bad hw/proto/hlen/plen, rs==c) + [RUST-SEC]
// arp (malformed verdicts identical, zero divergence) + [RUST-PERF] arp RDTSC.
// b806 (#404/#497 Phase O): FOURTH Tier-2 untrusted-wire parser - net/dhcp.c DHCP
// reply option-TLV parse ported to Rust (dhcp_parse_rs, live under -DRUST_DHCP);
// dhcp_parse_c kept verbatim for the differential + rollback. First reachable
// over-read confined (the C option walk ran over the fixed 308-byte options[]
// ignoring len). [RUST-DIFF] dhcp 512 vectors rs==c + [RUST-SEC] dhcp (crafted
// runt OFFERs the C over-reads, Rust confines) + [RUST-PERF] dhcp RDTSC.
// b809 (#404 / Phase R): exec/pe.c PE32 pre-map validation ported to Rust behind
// -DRUST_PE (pe_validate_full_rs). Pure DOS+PE+COFF+OptionalHeader validation +
// section-table raw/virtual-bounds walk; pe_load section-copy/import/reloc stay C.
// pe_validate_full_c kept verbatim (differential + rollback). Confines 3 LATENT
// OOB classes (section-table OOB read; uint32 raw-bound overflow -> memcpy OOB
// read; uint32 vaddr-bound overflow -> memcpy OOB write); pe_load unwired (#288).
// [RUST-DIFF] pe 256 vectors rs==c + [RUST-SEC] pe + [RUST-PERF] pe RDTSC.
// b810 (#404 Phase S): fs/fat.c VFAT dir-entry + LFN parse -> Rust (-DRUST_FAT).
// fat_dir_step_rs live (fat_readdir_inner routes to it); fat_dir_step_c verbatim
// reference. REACHABLE win: the C emits up to 259-char LFN names that
// fat_readdir_inner strcpy()s into 256/64/16-byte caller buffers (SYS_READDIR /
// rmdir / dosexec) - a crafted-FAT stack/heap overflow; Rust confines the name
// to <=255. Plain-C fix: fat_delete name[64]->[256]. [RUST-DIFF] fat rs==c on
// well-formed dirs + [RUST-SEC] fat + [RUST-PERF] fat RDTSC.
// b813 (#404 Phase V): gui/png.c PNG parse seams ported to Rust behind
// -DRUST_PNG. png_parse_ihdr_rs (IHDR validate + CHECKED u64 size math) +
// png_defilter_rs (bounds-checked None/Sub/Up/Average/Paeth reconstruction).
// REACHABLE integer-overflow OOB (read+WRITE) in the C reference confined by
// construction; zlib/inflate stays C. [RUST-DIFF]/[RUST-SEC]/[RUST-PERF] png.
// #492 Stage 1a (b818): kernel self-update (OTA) brick-safe write path.
// New proc/selfupdate.c: kernel_selfupdate_apply() verifies sha256(image)==
// expected + validates it is an x86-64 kernel ELF, backs up the current
// /boot/kernel.elf to /boot/kernel.elf.bak (kept), writes the new image to all
// four ESP paths, reads each back and re-verifies its sha256, and only on full
// success writes /CONFIG/PENDING_UPDATE.TXT. Any write/verify failure restores
// every path from the in-RAM backup and returns an error WITHOUT rebooting.
// kernel_selfupdate_reboot() flushes + ACPI-reboots. New SYS_KERNEL_SELFUPDATE
// (313). A Stage-1a isolation harness (selfupdate_boot_check, driven by
// /SELFUPD.REQ) exercises it without a network/UI. Security now: sha256 + ELF
// validation (integrity, not authentication); signature/pubkey verify is the
// clearly-marked Stage-1b hook. Isolation build only; not for golden.
// #492 Stage 1b (b820): authenticated, golden-ready OTA self-update.
// kernel_selfupdate_apply() now takes a detached RSA PKCS#1 v1.5 signature over
// sha256(image) and verifies it against a baked-in RSA-2048 public key
// (proc/ota_pubkey.h) - an unsigned/tampered/wrong-key image is REFUSED
// (SELFUPD_ERR_SIGNATURE) before any write. SYS_KERNEL_SELFUPDATE (313) is
// privilege-gated: only a registered service holding SVC_PERM_SELFUPDATE (new
// capability) may call it; arbitrary Ring-3 apps get SELFUPD_ERR_PERM. The
// Stage-1a /SELFUPD.REQ boot harness is now compile-gated behind
// SELFUPDATE_ISOLATION_HARNESS (NOT defined for golden), so no boot-time updater
// scaffold ships. The live trigger is the userland OTA daemon (/APPS/OTAUPD).
// USB-root TO-RAM writes are already write-through (fs/blockdev.c blk_write), so
// an applied kernel persists to the backing device across reboot.
// b826 (2026-07-16): serial integration of a large pre-verified dev batch.
// SECURITY (live, Ring-3 reachable): gui/jpeg.c build_huffman ~259KB heap OOB
// WRITE via a crafted JPEG DHT (MAYTERA-SEC-2026-0013). This is a PRE-EXISTING
// ORIGINAL bug, NOT Rust-extraction drift: bh_vuln.inc and bh_orig.inc are
// md5-identical, and b814 only moved the callsite and NARROWED its reach. The
// guard is flag-independent (fixed C + old Rust = no fault). Validated against
// the Kraft inequality as an independent oracle (300k vectors, C-vs-Kraft 0,
// Rust-vs-Kraft 0). Also: net/ip.c:381 total_length-ihl underflow guard (a
// 60-byte frame delivered len=65496 to icmp/udp/tcp; UDP/TCP propagation RUN,
// not theorised); gui/png.c #500 u64 math in png_parse_ihdr_c (183,321
// wrap-accepts -> 0); net/https.c #504 scope gap (gate now sz > len - p, real
// 22-byte hang witness); net/icmp.c ones-complement negative-zero fix (the
// original sends 0 echo replies, the shipped Rust sent 1) + the self-test now
// reports negzero coverage so a blind spot reads PASS(BLIND), not PASS.
// DRIFT: fs/xattr_entry_c.c clamp (86,742/600k -> 0); net/dhcp.c + rustkern.rs
// (264,738 -> 0 over 3M) + a widened >548-byte generator; #489 exec/elf.c
// check_overflow_add now (b > max) || (a > max - b), proven zero-regression vs
// a 128-bit oracle, with check_overflow_add_verbatim() keeping the reference.
// NEW RUST SEAMS (defense-in-depth, no advisory): -DRUST_PARTTBL (fs/ext2.c
// GPT+MBR partition-table parse, #365 root discovery, untrusted disk/USB) and
// -DRUST_USB_DESC (drivers/usb_audio.c uac_parse_config, untrusted USB config
// descriptor). Both strangler-flagged for one-line rollback.
// RING-0 EXIT phase 1: new drivers/audio_pcm.c + hda_space_wq() + syscalls
// 315/316/317 (SYS_AUDIO_PCM_OPEN/WRITE/CLOSE) + userland MUSICPLR/wavdec.c.
// Additive: sys_play_wav byte-identical, every failure path falls back.
// WAITQ: new sync/waitq.c __wait_event_wait_deadline/_timeout + waitq.h macros
// + sync/waitq_test.c ([WAITQ] self-test: 3/3 PASS), spawned from main.c's LATE
// worker cluster (blame.md:724 - an early-late_init worker is never scheduled).
// WAIT PHASE 1: fs/pipe.c #511 (EOF wake ordered BEFORE pipe_maybe_free, else
// UAF on read_wq); proc/signal.c #513 sys_alarm implemented + sys_pause ->
// g_pause_wq; proc/process.c/.h alarm_time + sweep folded into
// wake_sleeping_procs; proc/syscall.c #512 sys_ntp_sync (units, self-pumping,
// no core burn) + the job worker on g_post_job_wq; sync/futex.c duplicate
// ms_to_ticks merged (the timed park deliberately stays separate).
// #515 (ipc/msg.c wq conversion) and #514 (audio_drain dead poll) are
// deliberately NOT done here; see the internal wait-migration tickets.
// b831 (#497 net/TLS): TLS_MAX_RECORD_SIZE bounded the CIPHERTEXT record with
// the PLAINTEXT limit (2^14), so every full-size TLS record was rejected and
// every HTTPS response over ~16 KB failed (h2 "status=200 len=0"; #333's "no
// status"). Corrected to 2^14 + 2048 in BOTH the C and Rust arms of the
// tls_parse seam. Also: http->https redirects now re-dispatch to the TLS
// client instead of replaying plaintext at port 443, and the #333 net
// self-test is actually CALLED at boot for the first time (it was dead code).
#define MAYTERA_BUILD_NUMBER 851

// Version string helper macros
#define STRINGIFY(x) STRINGIFY_HELPER(x)
#define STRINGIFY_HELPER(x) #x

#define MAYTERA_VERSION_STRING "1.95.0"

// Build date (set at compile time)
#define MAYTERA_BUILD_DATE       __DATE__
#define MAYTERA_BUILD_TIME       __TIME__

// Full version string
#define MAYTERA_FULL_VERSION     "MayteraOS v" MAYTERA_VERSION_STRING

// Changelog for this version
#define MAYTERA_CHANGELOG \
    "v1.95.0 - July 2026\n" \
    "- #418: crash-dialog CR3 fix (gui/crashhandler.c mirrors sys_fb_flip's\n" \
    "  kernel-CR3 switch around fb_swap_buffers())\n" \
    "- #418: /STAGE.TXT late-boot breadcrumb ring + /PANIC.TXT on-fault record\n" \
    "  (fs/panic.c), both fixed-size + raw-sector-overwrite, never delete+recreate\n" \
    "- #418: /DEVLOG.TXT PCI-claimed-by-driver column\n" \
    "- #418: hotplug_init() now called from main.c (was implemented but never\n" \
    "  invoked - USB hotplug never worked on real hardware)\n" \
    "- #427: /dev/urandom replaced fake xorshift64 PRNG with a real CSPRNG\n" \
    "  (crypto/csprng.c HMAC-DRBG, RDSEED/RDRAND+RDTSC jitter+PIT ticks,\n" \
    "  periodic reseed, write() stirs entropy); added /dev/random (same CSPRNG)\n" \
    "- #427: boot-time CSPRNG self-test logs two samples + differ/non-zero check\n" \
    "- #fix-tls-certverify: TLS now actually validates server certificates -\n" \
    "  real RSA + new ECDSA P-256/P-384 verify (crypto/ecdsa.c), full chain-to-\n" \
    "  trusted-root + RTC-clock validity + hostname/SAN checks, bundled CA\n" \
    "  trust store (/CONFIG/CACERTS.PEM), TLS1.3 Finished verify_data checked,\n" \
    "  non-functional TLS1.2 static-RSA key exchange removed, https.c verify=1\n" \
    "- #427: exec/elf.c real PT_DYNAMIC relocation engine (RELATIVE/GLOB_DAT/\n" \
    "  JUMP_SLOT/64/PC32/PLT32/32/32S + best-effort TLS) + PIE_USER_BASE fix\n" \
    "  (PIE loader previously applied zero relocations AND used an unsafe load\n" \
    "  base); foundation for dlopen/CPython C-ext (#359)\n" \
    "v1.57.0 - July 2026\n" \
    "- Persistent /BOOTLOG.TXT boot log (fs/bootlog.c), flush-on-write\n" \
    "- xHCI delay calibrated via PIT channel 0 (real-hardware timing fix)\n" \
    "- PASSWD/SHADOW/GROUP boot-time reads: bounded retry + safety-net defaults\n" \
    "v1.9.0 - May 2026\n" \
    "- IPC subsystem: message passing syscalls (160-166)\n" \
    "- IPC subsystem: shared memory syscalls (170-174)\n" \
    "- IPC name service for process discovery\n" \
    "- Framebuffer syscalls wired (200-213)\n" \
    "- Window manager query API (SYS_WM_GET_WINDOWS)\n" \
    "- Userland compositor infrastructure (Phase 1, idle)\n" \
    "- Exclusive mode support for compositor takeover\n" \
    "- Mouse state dedup to prevent drain loops\n" \
    "- Phase 2 compositor: real user-mode rendering\n" \
    "- Compositor renders background, taskbar, clock, cursor\n" \
    "- SYS_COMPOSITOR_RENDER_WINDOWS syscall (156)\n" \
    "- Direct framebuffer mode for compositor\n" \
    "- Input event extraction for compositor syscall\n"

#endif // VERSION_H
