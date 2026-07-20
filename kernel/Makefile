# Makefile for MayteraOS 64-bit Kernel v1.8.0

# Compiler and tools
CC = gcc
AS = nasm
LD = ld
OBJCOPY = objcopy

# Target
TARGET = kernel.elf

# Directories
SRCDIR = .
OBJDIR = obj
MM_DIR = mm
CPU_DIR = cpu
VIDEO_DIR = video
DRIVERS_DIR = drivers
NET_DIR = net
NET_TLS_DIR = net/tls
FS_DIR = fs
SHELL_DIR = shell
GUI_DIR = gui
GUI_SETTINGS_DIR = gui/settings
EXEC_DIR = exec
DOS_DIR = dos
PROC_DIR = proc
IO_DIR = io
SECURITY_DIR = security
IPC_DIR = ipc
SYNC_DIR = sync
CRYPTO_DIR = crypto
GAMES_DIR = games
DOOM_DIR = games/doom
VFS_DIR = vfs
APPS_DIR = apps
BT_DIR = bt
WIFI_DIR = drivers/net/wifi

# GCC include path for freestanding headers (stdarg.h, etc.)
GCC_INCLUDE = $(shell $(CC) -print-file-name=include)

# Compiler flags for freestanding 64-bit kernel
CFLAGS = -ffreestanding \
         -fno-stack-protector \
         -fno-pic \
         -mno-red-zone \
         -mcmodel=kernel \
         -mno-mmx \
         -mno-sse \
         -mno-sse2 \
         -nostdlib \
         -nostdinc \
         -fno-builtin \
         -Wall \
         -Wextra \
         -Werror \
         -O2 \
         -g \
         -DRUST_IP_CHECKSUM \
         -DRUST_EXT2_DIRFIND \
         -DRUST_TCP_CHECKSUM \
         -DRUST_UDP_CHECKSUM \
         -DRUST_SHA256 \
         -DRUST_SHA512 \
         -DRUST_MD5 \
         -DRUST_MD4 \
         -DRUST_CHACHA20 \
         -DRUST_AES \
         -DRUST_HMAC \
         -DRUST_ICMP \
         -DRUST_ARP \
         -DRUST_DNS \
         -DRUST_DHCP \
         -DRUST_URL \
         -DRUST_ELF \
         -DRUST_PE \
         -DRUST_FAT \
         -DRUST_EXFAT \
         -DRUST_BMP \
         -DRUST_PNG \
         -DRUST_JPEG \
         -DRUST_INFLATE \
         -DRUST_TLS_PARSE \
         -DRUST_HTTP_PARSE \
         -DRUST_MP4 \
         -DRUST_JPEG_ENTROPY \
         -DRUST_HTTP2_FRAME \
         -DRUST_THEME_PARSE \
         -DRUST_WAV_PARSE \
         -DRUST_CERT_B64 \
         -DRUST_XATTR \
         -DRUST_ED25519_DECODE \
         -DRUST_XDR \
         -DRUST_TASKMGR_CORE \
         -DRUST_PARTTBL \
         -DRUST_USB_DESC \
         -DRUST_PROC_MEM \
         -DRUST_VFS_PATH \
         -DRUST_CONN_OWNER \
         -DRUST_PROCINFO \
         -I$(SRCDIR) \
         -isystem $(GCC_INCLUDE)

# #500/#503: `make SECTEST=1` dispatches SYS_SECTEST (the negative-control
# battery). DEBUG ONLY - never set for a golden/shipped kernel. Appended to the
# real CFLAGS rather than passed as `make CFLAGS+=...` on the command line,
# because that form REPLACES CFLAGS wholesale (blame.md) and would silently
# build the whole kernel without -ffreestanding.
ifeq ($(SECTEST),1)
CFLAGS += -DSECTEST_SYSCALL
endif

# #510 verification builds. DEBUG ONLY - never set for a golden/shipped kernel.
# Same append-to-CFLAGS form and same reason as SECTEST above.
#   make CVTEST=1    - drive a REAL TLS 1.3 handshake to each real host at boot
#                      and log the CertificateVerify verdict ([CV-FETCH]).
#   make CVTAMPER=1  - the NEGATIVE CONTROL: as CVTEST, but flip one bit of the
#                      server's CertificateVerify signature before checking it.
#                      Every fetch MUST abort. A check that never rejects
#                      anything is indistinguishable from no check, so this
#                      build is the only thing that proves #510 is load-bearing
#                      rather than decorative (#232 closed on a check that
#                      looked finished and proved nothing about possession).
ifeq ($(CVTEST),1)
CFLAGS += -DSECTEST_CV_FETCH
endif
ifeq ($(CVTAMPER),1)
CFLAGS += -DSECTEST_CV_FETCH -DSECTEST_CV_TAMPER
endif
#   make CVBASELINE=1 - the EXONERATION control: as CVTEST, but CertificateVerify
#                      reverts to the pre-#510 parse-and-discard. Lets any host
#                      that fails under CVTEST be attributed to #510 or cleared
#                      on an otherwise identical build.
ifeq ($(CVBASELINE),1)
CFLAGS += -DSECTEST_CV_FETCH -DSECTEST_CV_BASELINE
endif

# -DRUST_IP_CHECKSUM (#404 / #479 Phase B): route the live ip_checksum() symbol
# in net/ip.c to the Rust port ip_checksum_rs (rustkern.rs) instead of the
# original C (kept as ip_checksum_c). This is the strangler flag; REMOVE this
# one line and rebuild to roll straight back to the C implementation. The
# boot-time [RUST-DIFF] self-test in net/ip.c compares the two regardless.

# -DRUST_EXT2_DIRFIND (#404 / #485 Phase C): route the live ext2 directory-block
# entry scan (ext2_dirblock_find in fs/ext2.c, called per block by ext2_lookup)
# to the Rust port ext2_dirblock_find_rs (rustkern.rs) instead of the #476-
# hardened C (kept as ext2_dirblock_find_c). This is the strangler flag; REMOVE
# this one line and rebuild to roll straight back to C. The boot-time
# [RUST-DIFF] ext2_dir self-test in fs/ext2.c compares the two regardless.

# -DRUST_TCP_CHECKSUM (#404 / #486 Phase D): route the live tcp_checksum() symbol
# in net/tcp.c to the Rust port tcp_checksum_rs (rustkern.rs) instead of the
# original C (kept as tcp_checksum_c). Strangler flag; REMOVE this one line and
# rebuild to roll straight back to C. The boot-time [RUST-DIFF] tcp_checksum
# self-test in net/tcp.c compares the two regardless.

# -DRUST_UDP_CHECKSUM (#404 / #486 Phase D): route the udp_checksum() dispatcher
# in net/udp.c to the Rust port udp_checksum_rs (rustkern.rs) instead of the C
# reference (udp_checksum_c). STAGED: udp_checksum() is proven by the boot-time
# [RUST-DIFF] udp_checksum self-test but is NOT yet on the live udp_send() path
# (UDP checksums stay 0 / IPv4-optional, unchanged wire behavior). REMOVE this
# one line to roll back to the C reference.

# -DRUST_SHA256 (#404 / #487 Phase E): route the live sha256_transform() block
# compression dispatcher in crypto/sha256.c to the Rust port sha256_transform_rs
# (rustkern.rs) instead of the original C (kept as sha256_transform_c). The leaf
# takes only state[8] + a 64-byte block, so sha256_ctx_t never crosses FFI.
# Strangler flag; REMOVE this one line and rebuild to roll straight back to C.
# The boot-time [RUST-DIFF] sha256 self-test in crypto/sha256.c proves rs == c
# (NIST KAT via the live API + 20000 random state/block vectors) regardless.

# -DRUST_SHA512 (#404 / #488 Phase F): route the live sha512_transform() block
# compression dispatcher in crypto/sha512.c to the Rust port sha512_transform_rs
# (rustkern.rs) instead of the original C (kept as sha512_transform_c). The leaf
# takes only state[8] (uint64_t) + a 128-byte block, so sha512_ctx_t never
# crosses FFI. Strangler flag; REMOVE this one line and rebuild to roll straight
# back to C. The boot-time [RUST-DIFF] sha512 self-test in crypto/sha512.c proves
# rs == c (NIST KAT via the live sha512 path + 20000 random state/block vectors)
# and logs a [RUST-PERF] sha512 RDTSC micro-benchmark, regardless of the flag.

# -DRUST_MD5 (#404 / #489 Phase G): route the live md5_transform() block
# compression dispatcher in crypto/md5.c to the Rust port md5_transform_rs
# (rustkern.rs) instead of the original C (kept as md5_transform_c). The leaf
# takes only state[4] (uint32_t) + a 64-byte block, so md5_ctx_t never crosses
# FFI. MD5 decodes its message words LITTLE-endian (unlike the big-endian SHA
# schedule); the Rust port matches the C x[i] decode exactly. Strangler flag;
# REMOVE this one line and rebuild to roll straight back to C. The boot-time
# [RUST-DIFF] md5 self-test in crypto/md5.c proves rs == c (RFC 1321 KAT via the
# live md5() API + 20000 random state/block vectors) and logs a [RUST-PERF] md5
# RDTSC micro-benchmark, regardless of the flag.

# -DRUST_MD4 (#404 / #490 Phase H): route the live md4_transform() block
# compression dispatcher in crypto/md4.c to the Rust port md4_transform_rs
# (rustkern.rs) instead of the original C (kept as md4_transform_c). The leaf
# takes only state[4] (uint32_t) + a 64-byte block, so md4_ctx_t never crosses
# FFI. MD4 has 3 rounds (48 steps) and decodes its message words LITTLE-endian;
# the Rust port unrolls the exact FF/GG/HH sequence and matches the C x[i]
# decode. MD4 backs NTLM auth (net/smb.c) and is a cold path. Strangler flag;
# REMOVE this one line and rebuild to roll straight back to C. The boot-time
# [RUST-DIFF] md4 self-test in crypto/md4.c proves rs == c (RFC 1320 KAT via the
# live md4() API + 20000 random state/block vectors) and logs a [RUST-PERF] md4
# RDTSC micro-benchmark, regardless of the flag.
#
# -DRUST_CHACHA20 (#404 / #491 Phase I): route the live ChaCha20 block-core
# dispatcher in crypto/chacha20.c to the Rust port chacha20_block_rs (rustkern.rs)
# instead of the original C (kept as chacha20_block_c). The leaf takes only the
# 16-word (uint32_t) input state + a 64-byte keystream buffer, so chacha20_ctx_t
# never crosses FFI (init/setkey/counter/XOR-stream stay C and pass ctx->state).
# The 20-round (10 double-round) quarter-round core is reproduced exactly (column
# then diagonal quarter-rounds, wrapping adds, rotate_left 16/12/8/7) and the 16
# result words are serialized LITTLE-endian to the 64 output bytes. ChaCha20 backs
# TLS ChaCha20-Poly1305. Strangler flag; REMOVE this one line and rebuild to roll
# straight back to C. The boot-time [RUST-DIFF] chacha20 self-test in
# crypto/chacha20.c proves rs == c (RFC 8439 section 2.3.2 KAT via the live
# chacha20 API + 20000 random-state block vectors) and logs a [RUST-PERF] chacha20
# RDTSC micro-benchmark, regardless of the flag.

# -DRUST_AES (#404 / #492 Phase J): route the live aes_encrypt_block() and
# aes_decrypt_block() dispatchers in crypto/aes.c to the Rust ports
# aes_encrypt_block_rs / aes_decrypt_block_rs (rustkern.rs) instead of the C cores
# (kept as aes_encrypt_block_c / aes_decrypt_block_c). The leaves take only the
# expanded round-key array (ctx->rk, big-endian uint32 words) + the round count
# ctx->nr + the 16-byte in/out blocks, so aes_ctx_t never crosses FFI and the key
# schedule (aes_set_encrypt_key) stays in C. The plain S-box round form (Sub/Shift/
# Mix/AddRoundKey, forward + inverse) is reproduced byte-for-byte (same S-boxes,
# GF(2^8) xtime/gf_mul, column-major state, Nr=10/12/14). AES backs TLS AES-GCM
# (crypto/crypto.c) and SSH aes-ctr (net/ssh/*). Strangler flag; REMOVE this one
# line and rebuild to roll straight back to C. The boot-time [RUST-DIFF] aes self-
# test in crypto/aes.c proves rs == c (FIPS-197 AES-128 + AES-256 KAT via the live
# AES API + 20000 random (state,round-key,Nr) vectors for both encrypt and decrypt)
# and logs a [RUST-PERF] aes RDTSC micro-benchmark, regardless of the flag.

# -DRUST_HMAC (#404 / #493 Phase K): route the live one-shot HMAC symbols
# hmac_sha256() (crypto/sha256.c), hmac_sha384() (crypto/sha512.c) and hmac_md5()
# (crypto/hmac.c) to the Rust constructions hmac_sha256_rs / hmac_sha384_rs /
# hmac_md5_rs (rustkern.rs) instead of the original C (kept as hmac_*_c). HMAC is
# a CONSTRUCTION wrapping a hash, so the Rust legitimately calls back into the C
# hash init/update/final glue (which under -DRUST_SHA256/512/MD5 already runs the
# compression in Rust); the NEW Rust logic is the RFC 2104 outer wrapper (K' pad,
# ipad/opad XOR, H(opad || H(ipad||msg))) parameterized by block/digest size. The
# hash context crosses FFI only as an OPAQUE 16-aligned bounded byte buffer (no C
# struct duplicated in Rust; sizeof asserted <=256 in crypto/hmac.c). The kernel's
# INCREMENTAL streaming HMAC (hmac_*_init/update/final, used by the CSPRNG DRBG
# per-field update) stays C this phase; only the one-shot public symbols cross.
# hmac_sha256 backs the TLS 1.3 Finished key schedule (net/tls/tls.c) + the CSPRNG
# HMAC-DRBG one-shot (crypto/csprng.c); hmac_sha384 the TLS SHA-384 suite; hmac_md5
# NTLM/SMB auth (net/smb.c). Strangler flag; REMOVE this one line and rebuild to
# roll straight back to C. The boot-time [RUST-DIFF] hmac self-test in crypto/hmac.c
# proves rs == c (RFC 4231 SHA-256/384 + RFC 2202 MD5 KAT via the live hmac_* API +
# 21006 random (key,msg) vectors across all three variants) and logs a [RUST-PERF]
# hmac RDTSC micro-benchmark, regardless of the flag.
#
# -DRUST_ICMP (#404 / #494 Phase L): FIRST Tier-2 untrusted-wire-input parser
# folded to Rust. Routes the live incoming-ICMP parse in net/icmp.c (icmp_parse)
# to the Rust port icmp_parse_rs (rustkern.rs) instead of the verbatim reference
# icmp_parse_c (kept for the differential + rollback). The seam is the PURE
# parse/validate extracted out of icmp_handle(); the reply-send / echo-reply
# construction stays in C. The Rust removes two weaknesses of the verbatim C by
# construction: (1) the missing UPPER bound on the attacker length (C fed len
# unchecked into ip_checksum(buf,len) + a uint8_t reply[length] kernel-stack
# VLA; Rust rejects len > 1500 = MTU before any deref), and (2) the in-place
# mutation of the const untrusted RX buffer to verify the checksum (Rust folds
# over an immutable slice, zero writes). Strangler flag; REMOVE this one line and
# rebuild to roll straight back to C. The boot-time [RUST-DIFF] icmp self-test in
# net/icmp.c proves rs == c on the live agreement domain (valid echo request/
# reply every length 8..1500 + too-short + bad-checksum), logs a [RUST-SEC] icmp
# line reporting the oversize confinement (C accepts N/N unbounded, Rust confines
# N/N), and a [RUST-PERF] icmp RDTSC micro-benchmark, regardless of the flag.

# -DRUST_ARP (#404 / #495 Phase M): SECOND Tier-2 untrusted-wire-input parser.
# Routes the live incoming-ARP parse in net/arp.c (arp_parse) to the Rust port
# arp_parse_rs (rustkern.rs) instead of the verbatim reference arp_parse_c (kept
# for the differential + rollback). The seam is the PURE parse/validate extracted
# out of arp_handle(); the DAD check, cache-add, and reply-send stay in C. Unlike
# ICMP, the C reference here is ALREADY memory-safe (it length-gates len >= 28
# before any read, reads every field at a fixed offset inside those 28 bytes, and
# never mutates the buffer), so there is NO reachable OOB; this port is HONEST
# defense-in-depth that removes the unchecked-wire-pointer-arithmetic CLASS by
# construction (bounds-checked slice indexing). Strangler flag; REMOVE this one
# line and rebuild to roll straight back to C. The boot-time [RUST-DIFF] arp self-
# test in net/arp.c proves rs == c on the live agreement domain (~512 vectors of
# well-formed Ethernet/IPv4 ARP request/reply + too-short + bad hw/proto type +
# bad hlen/plen), logs a [RUST-SEC] arp line reporting that C and Rust reject the
# malformed corpus IDENTICALLY (zero divergence, no live OOB to fix), and a
# [RUST-PERF] arp RDTSC micro-benchmark, regardless of the flag.

# -DRUST_DNS (#404 / #496 Phase N): THIRD Tier-2 untrusted-wire-input parser (the
# strongest real memory-safety target on paper: DNS name-compression). Routes the
# live incoming DNS RESPONSE-message parse in net/dns.c (dns_parse_response) to the
# Rust port dns_parse_response_rs (rustkern.rs) instead of the verbatim reference
# dns_parse_response_c (kept for the differential + rollback). The seam is the PURE
# parse extracted out of dns_handle_response(): QR/rcode/ancount validation, the
# compression-pointer-aware question-name skip, and the answer-record walk for the
# first A record; the UDP receive, transaction-id/QR match, cache-put, and
# dns_query bookkeeping stay in C. HONEST security posture (like ARP, not a live
# fix): the C reference is ALREADY bounded - dns_skip_name cannot loop or read past
# msglen (pos strictly increases; a compression pointer returns immediately and is
# never FOLLOWED) and every record-field read is length-gated, so there is NO
# reachable OOB/hang even on pointer loops, OOB pointers, or oversized rdlength.
# The Rust removes the unchecked-wire-pointer-arithmetic class by construction AND
# pre-confines the classic pointer-FOLLOW loop/OOB class that becomes reachable the
# instant the parser is extended to DECODE names. Strangler flag; REMOVE this one
# line and rebuild to roll straight back to C. The boot-time dns_rust_selftest in
# net/dns.c proves rs == c on ~512 vectors (well-formed A / CNAME+A / AAAA-only /
# ancount=0 / rcode-error / not-a-response + malformed: truncated header, truncated
# mid-answer, compression-pointer loop, OOB pointer, oversized rdlength, label-
# past-end, random), logs a [RUST-SEC] dns line (zero divergence, no live OOB to
# fix) and a [RUST-PERF] dns RDTSC micro-benchmark, regardless of the flag.

# -DRUST_DHCP (#404 / #497 Phase O): FOURTH Tier-2 untrusted-wire-input parser -
# and the FIRST with a genuinely REACHABLE over-read in its C reference (stronger
# than arp/dns's latent cases). Routes the live incoming DHCP reply parse in
# net/dhcp.c (dhcp_parse) to the Rust port dhcp_parse_rs (rustkern.rs) instead of
# the verbatim reference dhcp_parse_c (kept for the differential + rollback). The
# seam is the PURE parse extracted out of dhcp_handle(): the BOOTP-header check
# (op==2 reply + magic cookie) + the option Type-Length-Value walk that extracts
# message-type(53)/subnet(1)/router(3)/dns(6)/lease(51)/server-id(54)/yiaddr; the
# socket / state machine (xid match, DISCOVER/REQUEST send, DAD, bind) stays in C.
# SECURITY (honest, reachable - NOT latent like arp/dns): the C option walk runs
# over the FIXED 308-byte options[] array driven only by the attacker's option
# LENGTH bytes and NEVER consults `len` (dhcp_handle only gates the 240-byte
# header), so a runt / non-END-terminated / lying-length OFFER (attacker-spoofable
# on the LAN) reads PAST the received bytes. Live it usually lands inside the
# >=1514-byte RX buffer (reads STALE/adjacent bytes, deriving config from
# non-packet memory) rather than faulting; with a tight buffer the tail reads past
# the allocation (offline ASan proves it). The Rust CONFINES it: slice of exactly
# len bytes, option walk bounded to [240,len), every step bounds-checked so a
# length past the packet terminates the walk. Strangler flag; REMOVE this one line
# and rebuild to roll straight back to C. The boot-time dhcp_rust_selftest in
# net/dhcp.c proves rs == c on ~512 agreement vectors (well-formed OFFER/ACK/NAK +
# too-short + bad-op + bad-magic), logs a [RUST-SEC] dhcp line (the count of
# crafted runt OFFERs the C over-reads where the Rust confines) and a [RUST-PERF]
# dhcp RDTSC micro-benchmark, regardless of the flag.

# -DRUST_URL (#404 / #498 Phase P): FIFTH Tier-2 untrusted-input parser. Routes
# the live URL-string parse in net/url.c (url_parse) to the Rust port url_parse_rs
# (rustkern.rs) instead of the verbatim reference url_parse_c (kept for the
# differential + rollback). The whole url_parse is the seam: it is already PURE
# (scheme/userinfo/host/port/path/query/fragment split; no DNS/connect - those
# stay in the browser/net layer). The untrusted input is the URL STRING, which
# comes from attacker-controllable redirect Location headers + the address bar.
# SECURITY (HONEST - defense-in-depth, NOT a reachable overflow like dhcp/ext2):
# the C reference ALREADY caps every out-field copy (explicit `>= sizeof(field)`
# reject before each of scheme/userinfo/host/path/query/fragment + safe_copy on
# the mailto path), so a long host/path does NOT overflow the fixed url_t buffers
# (offline ASan+UBSan over 300k NUL-terminated vectors is clean). The ONE genuine
# gap is the input scan's missing upper bound: the C trusts NUL-termination with
# no cap (offline ASan proves a 1-byte over-read walking off a non-terminated
# buffer). The Rust removes the class BY CONSTRUCTION (every out write is a
# bounds-checked slice index) and caps the input scan at URL_MAX_INPUT (8192).
# Live callers (gui/browser.c) pass a NUL-terminated fixed char buffer, so the
# over-read is latent, not reachable. Strangler flag; REMOVE this one line and
# rebuild to roll straight back to C. The boot-time url_rust_selftest in
# net/url.c proves rs == c on ~512 agreement vectors (curated valid http/https/
# ftp/ws/file/mailto/tel + IPv6 literals + reject cases + random http + fuzz),
# logs a [RUST-SEC] url line (over-long fields rejected identically by both) and
# a [RUST-PERF] url RDTSC micro-benchmark, regardless of the flag.

# -DRUST_ELF (#404 / #499 Phase Q): SIXTH Tier-2 untrusted-input parser and the
# STRONGEST security win of the port series. Routes the live exec/elf.c
# elf_validate() (the ELF64 loader's PURE pre-map header + program-header
# validation) to the Rust port elf_validate_full_rs (rustkern.rs) instead of the
# verbatim reference elf_validate_full_c (kept for the differential + rollback).
# The seam is PURE: e_ident/type/machine checks + phdr-table bounds + a PT_LOAD
# file-bounds walk; it never maps/allocates/CR3-switches/copies. The segment copy
# + mmap + PT_DYNAMIC relocation engine STAY in C (they are entangled with
# paging). The untrusted input is an on-disk/loaded ELF image (a downloaded or
# dropped app, any /apps/* binary - fully attacker-controllable). SECURITY
# (HONEST + REACHABLE, unlike arp/dns/url): the verbatim C has THREE gaps the
# Rust closes by construction: (1) calculate_load_bounds gates each PT_LOAD with
# check_overflow_add(p_offset, p_filesz, size) == (p_offset > size - p_filesz),
# which UNDERFLOWS when p_filesz > size, so an oversized p_filesz PASSES and
# elf_load()'s memcpy(dest, elf_data+p_offset, p_filesz) then OOB-reads the file
# buffer AND OOB-writes past the load allocation (ASan-proven heap-buffer-overflow
# READ of 4216 bytes offline); (2) elf_validate never checks e_phentsize >=
# sizeof(Elf64_Phdr), so elf_get_phdr's 56-byte read over-reads for a small
# e_phentsize (ASan-proven 8-byte over-read); (3) no p_memsz >= p_filesz check
# (segment write-overflow, latent). (1) and (2) are REACHABLE in the live loader
# today; -DRUST_ELF confines them (elf_load bails on the Rust reject before any
# map/copy). Candidate plain-C fix too (flagged in blame.md). On WELL-FORMED ELFs
# (every real /apps binary: e_phentsize==56, in-bounds segments, memsz>=filesz)
# the Rust returns SUCCESS byte-identically - a full desktop proves it accepted
# the compositor + every app ELF. Strangler flag; REMOVE this one line and
# rebuild to roll straight back to C. The boot-time elf_rust_selftest in
# exec/elf.c proves rs == c on ~256 agreement vectors, logs a [RUST-SEC] elf line
# (the three reachable/latent OOB classes, C-accepted + Rust-confined) and a
# [RUST-PERF] elf RDTSC micro-benchmark, regardless of the flag. The offline
# pre-flight ran 2,000,000 differential vectors (0 mism) over 7 real /apps ELFs +
# synthetic images, and ASan-proved the two reachable OOBs above.

# -DRUST_PE (#404 / Phase R): SEVENTH Tier-2 untrusted-input parser. Routes the
# live exec/pe.c pe_validate() (the PE/PE32 loader's PURE DOS + PE-signature +
# COFF + OptionalHeader validation plus the section-table raw/virtual-bounds walk)
# to the Rust port pe_validate_full_rs (rustkern.rs) instead of the verbatim
# reference pe_validate_full_c (kept for the differential + rollback). The seam is
# PURE: it never maps/allocates/copies. The section copy + import resolution +
# relocation STAY in C (pe_load/pe_execute). The untrusted input is an on-disk PE
# image (DOS/PE header offsets, NumberOfSections, section RVAs/sizes - fully
# attacker-controllable). SECURITY (HONEST, LATENT): the verbatim C has THREE OOB
# classes the Rust closes by construction: (1) NO section-table bounds check -
# pe_get_sections() locates the table at &OptionalHeader + SizeOfOptionalHeader
# and pe_load reads sections[i] blind, so a bogus SizeOfOptionalHeader/oversized
# NumberOfSections OOB-reads the table (ASan-provable offline); (2) the per-section
# PointerToRawData+SizeOfRawData > size bound is uint32 - an overflowing sum wraps
# below size and passes, then pe_load's memcpy OOB-READs the file buffer; (3) the
# per-section VirtualAddress+SizeOfRawData > SizeOfImage bound is likewise uint32 -
# an overflowing sum passes, then pe_load's memcpy OOB-WRITES past the image
# allocation (ELF-p_filesz class). UNLIKE elf/ext2/dhcp these are LATENT not
# reachable-from-boot: pe_load has NO live caller yet (Win32 #288 unimplemented),
# so the seam is a PRE-EMPTIVE confinement for when #288 wires PE loading. On
# WELL-FORMED PE32 images the Rust returns SUCCESS byte-identically. Strangler
# flag; REMOVE this one line and rebuild to roll straight back to C. The boot-time
# pe_rust_selftest in exec/pe.c proves rs == c on ~256 agreement vectors, logs a
# [RUST-SEC] pe line (the three latent OOB classes, C-accepted + Rust-confined)
# and a [RUST-PERF] pe RDTSC micro-benchmark, regardless of the flag.

# -DRUST_EXFAT (#404 / Phase T): EIGHTH Tier-2 untrusted-input parser. Routes the
# exFAT directory entry-set decode (fs/exfat.c parse_entry_set) to the Rust port
# exfat_dir_step_rs (rustkern.rs) instead of the verbatim reference
# exfat_dir_step_c (kept for the differential + rollback). The seam is PURE: it
# decodes one File(0x85)+Stream(0xC0)+Name(0xC1) entry set (SecondaryCount,
# NameLength, UTF-16 name chars - all attacker-supplyable on a hot-plugged USB
# exFAT stick) and never does disk I/O or FS mutation; the cluster-chain walk +
# cluster read stay in C (exfat_readdir). REACHABILITY (HONEST): exfat_mount is
# reachable via USB hot-plug (drivers/hotplug.c), but the entry-set decode path
# (exfat_readdir) has NO live caller today - nothing lists an exFAT directory on
# the FAT32-root golden - so like pe.c it is REACHABLE-but-LATENT (a pre-emptive
# confinement for when an exFAT dir browser is wired). SECURITY (HONEST): unlike
# the FAT b810 LFN case this C does NOT overflow - it bounds SecondaryCount
# (offset+bytes_used>buf_size -> reject) and caps the name reassembly
# (name_offset+j<255 into name_buf[256]). The residual defect the Rust closes is
# the UNINITIALIZED name_buf: a crafted NameLength larger than the File Name
# entries present makes the C decode read stale stack (a non-deterministic stack
# info-leak into the filename), never an OOB; the Rust zero-inits + bounds every
# disk-controlled field access through a slice, so it decodes exactly the present
# chars. On WELL-FORMED entries it is byte-identical. Strangler flag; REMOVE this
# one line and rebuild to roll straight back to C. The boot-time
# exfat_rust_selftest in fs/exfat.c proves rs == c on ~256 agreement vectors,
# logs a [RUST-SEC] exfat line (the stale-stack leak, C-leaked + Rust-confined)
# and a [RUST-PERF] exfat RDTSC micro-benchmark, regardless of the flag.

# -DRUST_JPEG (#404 / Phase W): TWELFTH Tier-2 untrusted-input parser and the
# THIRD image decoder (after bmp U / png V). Routes the pure JPEG header-parse
# seam (gui/jpeg.c jpeg_parse_headers) to the Rust port jpeg_parse_headers_rs
# (rustkern.rs) instead of the verbatim reference jpeg_parse_headers_c (kept for
# the differential + rollback). The seam is the SOI..SOS marker/segment walk
# (APPn/COM/DQT/DHT/SOF0/SOS): segment-length bounds, DQT/DHT table counts, SOF0
# dims + sampling, SOS table selectors - all attacker-supplyable (album art in
# the music player, Image Viewer, browser <img>, Files previews all feed
# image_load_jpeg). The entropy / Huffman-build / IDCT / MCU-upsample / color-
# convert decode stays C (jpeg_decode_scan), GATED behind this parse. SECURITY
# (HONEST, REACHABLE - ext2/fat/png class, NOT latent): the C reference has
# THREE reachable OOBs the Rust confines: (A) SOF0 comp_qt UNVALIDATED -> the C
# decode_block indexes quant[comp_qt][k] (quant is [4][64]); comp_qt is a full
# byte (0..255) -> past-allocation heap OOB READ; (B) SOS comp_dc/comp_ac
# UNVALIDATED (0..15) -> decode_huff indexes huff_fast[dc][comp_dc] ([2] deep) ->
# past-allocation heap OOB READ; (C) DHT sum-of-16-count-bytes (up to 4080)
# written into huff_vals[.][.][256] with NO clamp -> heap OOB WRITE (the severe
# one, ASan-proven OFFLINE). Under -DRUST_JPEG the seam REJECTS all three before
# any decode/alloc runs = a LIVE security improvement; plain-C hardening filed as
# #501. On WELL-FORMED and every generic-malformed (truncation/bad-order/bad-sig)
# input the Rust is byte-identical to the C (identical return + jpeg_hdr_t).
# comp_h/comp_v stay unvalidated to match C exactly (garbage sampling is NOT an
# OOB in the C decoder - block loops are `bx<h`/`by<v` gated, writes bounded).
# Strangler flag; REMOVE this one line and rebuild to roll straight back to C.
# The boot-time jpeg_rust_selftest in gui/jpeg.c proves rs == c on the real
# 435-byte baseline JPEG + ~64 truncations, logs a [RUST-SEC] jpeg line (the
# three confinements) and a [RUST-PERF] jpeg RDTSC bench, regardless of the flag.

# -DRUST_INFLATE (#404 / #502 Phase X): route the live DEFLATE/inflate core in
# gui/png.c (the inflate() dispatcher, called by image_load_png to decompress the
# PNG IDAT stream) to the Rust port inflate_rs (rustkern.rs) instead of the
# verbatim reference inflate_c (kept for the differential + rollback). This is
# the canonical decompression OOB seam shared by the PNG image path, the gzip/
# tar.gz/zip archiver (#321) and gzip content-encoding: a bad DISTANCE code would
# read before the output window start (OOB READ), a bad LENGTH would write past
# the output buffer (OOB WRITE), bad Huffman code-length tables would overrun the
# symbol tables. SECURITY (HONEST - LATENT / defense-in-depth, NOT reachable):
# UNLIKE ext2/fat/elf/jpeg, the C inflate_c is a puff-style decoder that ALREADY
# validates every reachable OOB surface - the LZ77 back-reference copy bounds
# BOTH ends (dist > out_pos AND out_pos+len > dst_cap), the literal write checks
# out_pos >= dst_cap, the stored block checks input+output bounds, the dynamic
# code-length RLE guards every repeat with n < hlit+hdist, the canonical Huffman
# decode index stays in [0,total) by construction, and input exhaustion returns
# -1. OFFLINE ASan/UBSan over 3,000,000+ hostile vectors (distance-before-window,
# length-past-cap, truncation, bad HLIT/HDIST/HCLEN, over-long RLE, bad block
# type, fuzz) + 4 hand-crafted classic attacks found ZERO OOB in inflate_c, and
# the Rust is byte-identical to C on 393 well-formed streams (incl. 22 REAL PNG
# IDAT streams) == the zlib original. So this is a defense-in-depth port (the
# Rust confines the whole class by construction: every window read/output write/
# input bit-read is a bounds-checked slice that HALTS loudly rather than corrupt),
# NOT a live-OOB fix - no plain-C hardening task is warranted. Strangler flag;
# REMOVE this one line and rebuild to roll straight back to C. The boot-time
# inflate_rust_selftest in gui/png.c proves rs == c on 27 embedded RAW-DEFLATE
# vectors (~350 with mutations) + 4 crafted hostile attacks, logs [RUST-SEC]
# inflate + [RUST-PERF] inflate, regardless of the flag.

# -DRUST_HTTP_PARSE (#404 / #504 Phase Y): route the HTTP client response length-
# parse seam to Rust (rustkern.rs) instead of the verbatim C references (kept as
# https_*_c / *_c for the differential + a flag-off rollback). HTTP responses are
# remote/attacker-influenced on EVERY fetch (browser http+https, Kimi/LLM API,
# update check, netinfo/weather/crypto widgets, music metadata). The seam covers
# the three classic HTTP-parser OOB surfaces: the chunked chunk-size hex parse
# (net/https.c https_dechunk -> https_dechunk_rs; net/wget.c http_decode_chunked
# -> http_decode_chunked_rs + the framing gate https_chunked_is_complete ->
# https_chunk_complete_rs), the header-block CRLFCRLF framing (find_header_end /
# https_find_header_end -> http_find_header_end_rs), and the Content-Length digit
# parse (https_content_length -> https_content_length_rs; parse_uint ->
# http_parse_uint_rs). SECURITY: this port removes a REACHABLE remote OOB -
# MAYTERA-SEC-2026-0008 (CWE-190 -> CWE-787/CWE-125): https_dechunk's clamp
# `if (in + sz > len) sz = len - in;` is u32 and WRAPS for a near-2^32 hex chunk
# size, skipping the clamp so the subsequent memmove over-copies ~4 GiB out of the
# kmalloc'd body (OOB read + WRITE). It is reachable behind https_chunked_is_
# complete (the crafted body "FFFFFFFE\r\n0\r\n\r\n" makes that gate return 1),
# i.e. a malicious/compromised HTTPS origin the browser visits or a MITM/compromised
# API/update endpoint. ASan-proven OFFLINE (the C over-copies; the Rust confines
# by construction - checked_add detects the wrap and takes the clamp). The C-
# fallback hardening of the same bound (so a flag-off rollback stays safe) is task
# #504. The other seams are already correctly bounded (LATENT/defense-in-depth).
# Strangler flag; REMOVE this one line and rebuild to roll straight back to C. The
# boot-time http_parse_rust_selftest in net/https.c proves rs == c on well-formed
# responses and logs [RUST-DIFF]/[RUST-SEC]/[RUST-PERF] http_parse, regardless of
# the flag. A FOLD IS WARRANTED.

# -DRUST_MP4 (#404 / #505 Phase Z): route the untrusted-.m4a ISO-BMFF / MP4
# sample-table parse in media/aac.c (mp4_parse) to the Rust port mp4_parse_rs
# (rustkern.rs) instead of the verbatim reference mp4_parse_c (kept for the
# differential + a flag-off rollback). Reachable from Ring-3: the music player
# plays an audio file BY PATH via SYS_PLAY_WAV -> sys_play_wav ->
# audio_play_file_async -> audio_play_file (fat_read_file: attacker-controlled
# bytes off disk / a download) -> audio_decode_open -> aac_create -> mp4_parse.
# The seam is the pure atom descent (moov/trak/mdia/minf/stbl -> stsd/mp4a/esds
# ASC) + the stsz/stco(co64)/stsc sample-table walk that builds the (offset,size)
# frame table; faad2 decode + ADTS + the codec plumbing stay C. Two-phase (NULL
# probe for the frame count, then a fill pass with that exact capacity) mirrors
# the C's kmalloc(nsamp) then fill; mp4_parse_result_t/aac_frame_t layouts are
# _Static_assert-locked (16 / 8 bytes) so the FFI can never silently drift.
# SECURITY: this port removes a REACHABLE local-file OOB - MAYTERA-SEC-2026-0009
# (CWE-125, class of ext2/fat/elf): mp4_parse reads the sample-table COUNTS
# (nsamp from stsz, nchunks from stco/co64, nstsc from stsc) as trusted lengths
# and then walks stsz_tab/stco_tab/stsc_tab by those counts with NO per-entry
# bound against the buffer (nstsc has NO clamp; nsamp/nchunks are clamped only to
# <= 10,000,000). A crafted .m4a whose declared counts far exceed the tables
# actually present drives be32(d + tab + i*stride) far past the kmalloc'd file
# buffer -> heap OOB READ; a runt stsz/stco/stsc atom at end-of-buffer over-reads
# its own count field. ASan-proven OFFLINE (the C over-reads; the Rust confines
# by construction - a slice of exactly buf_size + bounds-checked reads make an
# over-long count terminate the walk). The C-fallback hardening of the same
# bounds (so a flag-off rollback stays safe) is task #505. Strangler flag; REMOVE
# this one line and rebuild to roll straight back to C. The boot-time
# mp4_rust_selftest in media/aac.c proves rs == c on ~256 well-formed .m4a atom
# trees + reject cases and logs [RUST-DIFF]/[RUST-SEC]/[RUST-PERF] mp4, regardless
# of the flag. A FOLD IS WARRANTED.

# -DRUST_JPEG_ENTROPY (#404 batch-1): route the live JPEG dequant+IDCT decode
# seam (gui/jpeg.c decode_block tail = zigzag-scatter dequantize + integer
# inverse DCT, stb_image idct_block) to the Rust port jpeg_dequant_idct_rs
# (rustkern.rs) instead of the verbatim reference jpeg_dequant_idct_c (kept for
# rollback + the boot differential). NO reachable memory OOB (ASan-clean over
# 3.2M offline vectors: both C and Rust confine every write to the 64-entry
# block -> defense-in-depth). The REAL fix is CWE-190: the C IDCT has REACHABLE
# signed-integer-overflow UNDEFINED BEHAVIOR on large coefficient products (a
# crafted JPEG carries ac_val up to +-32767 * quant up to 255); on gcc -O2
# x86-64 it wraps benignly but is UB the compiler may miscompile. The Rust
# wrapping_* ops make it well-defined and byte-identical to the observed wrap.
# Plain-C hardening (i64 intermediates) is task #507. Drop the flag + rebuild to
# roll back. Boot jpeg_entropy_rust_selftest proves rs==c and logs
# [RUST-DIFF]/[RUST-SEC]/[RUST-PERF] jpeg_idct regardless of the flag.

# -DRUST_HTTP2_FRAME (#404 batch-1 / MAYTERA-SEC-2026-0010): route the live
# HTTP/2 frame framing in net/http2.c http2_get() to the Rust port
# http2_frame_next_rs (rustkern.rs) instead of the verbatim reference
# http2_frame_next_c (net/http2_frame_c.c, kept for rollback + differential).
# The seam validates the 9-byte frame header, bounds the 24-bit length against
# the buffer, and computes the per-type meaningful-payload extent (DATA/HEADERS
# padding strip + HEADERS PRIORITY strip) with EVERY derived read bounds-checked.
# SECURITY: this removes a REACHABLE REMOTE pre-auth defect - a malicious HTTPS
# site the browser visits negotiates h2 and sends one crafted zero-length PADDED
# DATA/HEADERS frame; the old inline code read payload[0]/pp[off] with payload==
# NULL (payload is only kmalloc'd when flen>0) -> kernel page fault -> whole-OS
# DoS (CWE-476 rooted in CWE-125). The seam REJECTS that frame. C ticket #506.
# Drop the flag + rebuild to roll back. Boot http2_frame_selftest proves rs==c
# and both reject the evil frame, logging [RUST-DIFF]/[RUST-SEC]/[RUST-PERF]
# http2_frame regardless of the flag.

# -DRUST_THEME_PARSE (#404 batch-1): route the live theme-file line tokenizer in
# gui/theme_parser.c (the untrusted-input atom theme_parse_ini repeats) to the
# Rust port theme_parse_line_rs (rustkern.rs) instead of the verbatim reference
# theme_parse_line_c. Themes are downloadable/editable user content (#141), so
# data[0..size] is UNTRUSTED. The C is already fully bounded (ASan-clean over
# 32.6M malformed vectors) so this is defense-in-depth (removes the raw-scan
# class by construction: slice of exactly len, every out field hard-capped). One
# rs/c divergence was found + fixed offline: an EMBEDDED NUL truncates the
# classified line in C (NUL-terminated helpers), which the Rust now mirrors by
# ending the classified view at the first interior 0x00. Drop the flag + rebuild
# to roll back. Boot theme_parse_rust_selftest proves rs==c and logs
# [RUST-DIFF]/[RUST-SEC]/[RUST-PERF] theme_parse regardless of the flag.

# NOTE (#404 batch-3): a SSH binary-packet FRAMING seam was prepared for
# net/ssh/ssh_transport.c ssh_recv_packet() (the size_t payload_len = packet_len
# - 1 - pad_len underflow). On integration it was found that ssh_transport.c is
# DEAD SCAFFOLDING (see NET_SSH_SOURCES below: only ssh2.c/ssh2_server.c compile),
# ssh_recv_packet has NO caller, and the LIVE ssh2.c ssh2_extract() already
# rejects the pad underflow via a signed-int `int payload_len = (int)packet_length
# - pad - 1; if (payload_len < 1) return -1;` in BOTH branches. So the underflow
# is NOT reachable in the shipping kernel -> NO -DRUST_SSH_FRAME flag, NO advisory
# (assessed-clean, dead code). Retargeting the seam to ssh2.c would be a new,
# unverified port and the batch-3 directive is to STOP porting; not done.

# -DRUST_ED25519_DECODE (#404 batch-3): route the live TweetNaCl unpack25519 in
# crypto/ed25519.c (32-byte compressed-point y-decode + sign-bit mask) to the Rust
# port unpack25519_rs (rustkern.rs) instead of the verbatim unpack25519_c. The
# curve sqrt / scalarmult / field math STAY in C. Defense-in-depth (fixed-size
# 32-byte read, NO reachable OOB); Rust removes the raw-index class by construction
# (slices of exactly 32/16). Boot ed25519_decode_selftest proves rs==c over edge +
# 20000 PRNG vectors. Drop the flag + rebuild to roll back to C.

# -DRUST_XDR (#404 batch-3, LAST parser-tier flag): route the DECODE branch of the
# net/rpc.c XDR primitives (xdr_uint32/uint64/opaque/bytes/string/string_len/skip/
# nfs_fh3) to the Rust ports xdr_decode_*_rs (rustkern.rs). The ENCODE branch stays
# C. The untrusted input is XDR-encoded RPC/NFS server replies (remotely mountable
# via fs/netfs.c nfs://host/export). The C decode is already SOURCE-bounded on this
# 64-bit target (NO reachable OOB); Rust removes the align round-up overflow class
# by construction (checked_add). Defense-in-depth (NO advisory for the seam). NOTE:
# the source-bounded XDR seam does NOT confine the SEPARATE net/nfs.c destination
# over-write (MAYTERA-SEC-2026-0012, fixed in nfs.c, ticket #509). Boot
# xdr_rust_selftest proves rs==c on a real NFS reply. `make clean && make`.

# -DRUST_TASKMGR_CORE (#404 / #487 / #349): route the Task Manager data core
# (perf-history ring aggregation + process-row sort) in gui/taskmanager.c to
# perf_ring_stats_rs / taskmgr_sort_rows_rs (rustkern.rs). Confines the naive
# C sort's count>cap OOB-write (CWE-787) and the ring's cap==0 div-by-zero.
# Defense-in-depth (new data core, no shipping C consumer feeds those, NO
# advisory). Drop the flag to fall back to the in-file C twins (trivial rollback).

# -DRUST_PARTTBL (#404 driver/block tier): routes the live MBR/GPT partition-table
# parse in fs/ext2.c (parttbl_gpt_hdr / parttbl_gpt_sec_scan / parttbl_mbr_find,
# the pure byte-walking of ext2_find_partition) to the Rust ports parttbl_*_rs
# (rustkern.rs) instead of the C twins. ext2_find_partition is the #365
# single-disk two-partition root discovery, called on EVERY boot from main.c once
# the FAT ESP is mounted; the input is an UNTRUSTED partition table off whatever
# disk or USB stick is inserted (the iMac target boots off USB). blk_read I/O and
# control flow stay in C; only the parse moves. Strangler flag; REMOVE this one
# line and rebuild to roll straight back to C. The boot-time [RUST-DIFF] parttbl
# self-test in fs/ext2.c proves rs == c on this exact build (24,745 vectors)
# regardless of the flag. DEFENSE-IN-DEPTH, no live bug, NO advisory: the C is
# genuinely bounded (ASan+UBSan clean over 40,921 vectors + an exhaustive
# 98,560-combination guard-legal probe), but its safety rests ENTIRELY on one
# load-bearing guard (esz >= 128 with per_sec derived as 512/esz); the Rust
# removes that dependency by construction (slice bounds + checked e*esz).

# -DRUST_USB_DESC (#404 driver tier): routes the live USB Audio Class config-
# descriptor parse in drivers/usb_audio.c (uac_parse_config, called from
# uac_probe on every USB audio device enumeration) to uac_parse_config_rs
# (rustkern.rs) instead of the in-file C twin. Pure parse only: the xHCI control
# transfers, SET_INTERFACE/SET_CONFIGURATION and the kprintf report stay in C.
# Drop the flag to fall back to uac_parse_config_c (trivial rollback). The
# boot-time [RUST-DIFF] usb_desc self-test proves rs == c on well-formed
# descriptors regardless of the flag.
# HONEST SCOPE: the C reads cfg[i+2], cfg[i+3], cfg[i+6] (INTERFACE) and
# cfg[i+2..i+6] (ENDPOINT) guarded ONLY by `blen >= 2`, so a crafted descriptor
# with a lying 2-byte INTERFACE/ENDPOINT over-reads up to 5 bytes past `len`.
# That over-read is REACHABLE (verified: a device reporting wTotalLength >= 512
# clamps to total = 512 = sizeof(cfg) in drivers/xhci.c, and a lying 2-byte
# INTERFACE at offset 510 makes the C read cfg[512], 1 byte past the buffer).
# It is NOT an advisory because the impact is negligible: cfg is a 512-byte
# STATIC .bss array, so the over-read lands in adjacent .bss (no page boundary,
# no fault), the bytes only influence which endpoint is selected plus one
# kprintf'd bInterval value, and it requires a malicious USB device physically
# attached. The Rust removes the class by construction (d.get() returns None
# exactly where the C would leave the buffer). NOTE for the record: the dev
# harness's "0 of them with len==512" is a property of its random corpus, NOT a
# proof of unreachability - a crafted vector reaches it.

# Assembler flags
ASFLAGS = -f elf64 -g

# Linker flags
# -z noexecstack (#404 Phase A): a Rust staticlib member can omit the
# .note.GNU-stack section; the GNU linker then conservatively marks the WHOLE
# image's stack executable (GNU_STACK -> RWE). Forcing noexecstack keeps the
# linked kernel.elf stack non-executable (GNU_STACK = RW). Verify with:
#   readelf -l kernel.elf | grep GNU_STACK   (must show RW, not RWE)
LDFLAGS = -nostdlib \
          -z max-page-size=0x1000 \
          -z noexecstack \
          -T linker.ld

# Source files (root level)
C_SOURCES = main.c \
            string.c \
            serial.c \
            stubs.c

ASM_SOURCES = entry.asm memcpy_fast.asm

# Memory management sources
MM_SOURCES = $(wildcard $(MM_DIR)/*.c)
MM_ASM = $(wildcard $(MM_DIR)/*.asm)

# CPU sources
CPU_SOURCES = $(wildcard $(CPU_DIR)/*.c)
CPU_ASM = $(wildcard $(CPU_DIR)/*.asm)

# Video sources
VIDEO_SOURCES = $(wildcard $(VIDEO_DIR)/*.c)

# Driver sources
DRIVER_SOURCES = $(wildcard $(DRIVERS_DIR)/*.c)

# Network sources
NET_SOURCES = $(wildcard $(NET_DIR)/*.c)

# Network TLS sources
NET_TLS_SOURCES = $(wildcard $(NET_TLS_DIR)/*.c)

# SSH sources (new ssh2 client + server; the legacy net/ssh/*.c are dead scaffolding)
NET_SSH_SOURCES = net/ssh/ssh2.c net/ssh/ssh2_server.c

# Filesystem sources
FS_SOURCES = $(wildcard $(FS_DIR)/*.c)

# Shell sources
SHELL_SOURCES = $(wildcard $(SHELL_DIR)/*.c)

# GUI sources
GUI_SOURCES = $(wildcard $(GUI_DIR)/*.c)

# GUI Settings panel sources
GUI_SETTINGS_SOURCES = $(wildcard $(GUI_SETTINGS_DIR)/*.c)

# Exec sources (ELF loader)
EXEC_SOURCES = $(wildcard $(EXEC_DIR)/*.c)

# DOS emulation sources
DOS_SOURCES = $(wildcard $(DOS_DIR)/*.c)

# Process management sources
PROC_SOURCES = $(wildcard $(PROC_DIR)/*.c)
PROC_ASM = $(wildcard $(PROC_DIR)/*.asm)

# Async I/O sources
IO_SOURCES = $(wildcard $(IO_DIR)/*.c)

# Security sources
SECURITY_SOURCES = $(wildcard $(SECURITY_DIR)/*.c)

# IPC sources
IPC_SOURCES = $(wildcard $(IPC_DIR)/*.c)

# Synchronization sources
SYNC_SOURCES = $(wildcard $(SYNC_DIR)/*.c)

# Crypto sources
CRYPTO_SOURCES = $(wildcard $(CRYPTO_DIR)/*.c)

# Games sources
GAMES_SOURCES = $(wildcard $(GAMES_DIR)/*.c)

# DOOM sources (compiled with relaxed flags: legacy code)
DOOM_SOURCES = $(filter-out $(DOOM_DIR)/i_video.c,$(wildcard $(DOOM_DIR)/*.c))
DOOM_CFLAGS = $(filter-out -Wall -Wextra -Werror,$(CFLAGS)) -w
DOOM_OBJECTS = $(patsubst %.c,$(OBJDIR)/%.o,$(DOOM_SOURCES))

# libmad MP3 decoder (vendored, integer FPM_64BIT, relaxed warnings + compat shims)
LIBMAD_SOURCES = $(wildcard media/libmad/*.c)
LIBMAD_CFLAGS = $(filter-out -Wall -Wextra -Werror,$(CFLAGS)) -w -DHAVE_CONFIG_H -Imedia/libmad/compat
LIBMAD_OBJECTS = $(patsubst %.c,$(OBJDIR)/%.o,$(LIBMAD_SOURCES))

# Tremor (libvorbisidec) integer OGG Vorbis decoder + libogg container
# (vendored, no FPU, relaxed warnings + compat shims). #236.
TREMOR_SOURCES = $(wildcard media/tremor/*.c)
TREMOR_CFLAGS = -Imedia/tremor/compat -Imedia/tremor $(filter-out -Wall -Wextra -Werror,$(CFLAGS)) -w
TREMOR_OBJECTS = $(patsubst %.c,$(OBJDIR)/%.o,$(TREMOR_SOURCES))

# dr_flac FLAC decoder (vendored, integer s16 path, NO floating point;
# relaxed warnings + compat shims). #331.
DRFLAC_SOURCES = $(wildcard media/dr_flac/*.c)
DRFLAC_CFLAGS = -Imedia/dr_flac/compat -Imedia/dr_flac $(filter-out -Wall -Wextra -Werror,$(CFLAGS)) -w
DRFLAC_OBJECTS = $(patsubst %.c,$(OBJDIR)/%.o,$(DRFLAC_SOURCES))

# Opus decoder (vendored libopus 1.3.1, integer FIXED_POINT decode subset, NO
# floating point; objdump-verified zero FP/SIMD. relaxed warnings + compat
# shims). The Ogg-Opus container reuses the libogg framing vendored for the
# OGG Vorbis (Tremor) port in media/tremor/. #331.
OPUS_SOURCES = $(wildcard media/opus/celt/*.c) $(wildcard media/opus/silk/*.c) $(wildcard media/opus/src/*.c) media/opus/opus_compat.c
OPUS_CFLAGS = -Imedia/opus/compat -Imedia/opus -Imedia/opus/include -Imedia/opus/celt -Imedia/opus/silk -Imedia/opus/silk/fixed -include media/opus/opus_config.h $(filter-out -Wall -Wextra -Werror,$(CFLAGS)) -w
OPUS_OBJECTS = $(patsubst %.c,$(OBJDIR)/%.o,$(OPUS_SOURCES))

# faad2 AAC/M4A decoder (vendored faad2 2.7 libfaad, integer FIXED_POINT LC-only
# decode subset, NO floating point; objdump-verified zero FP/SIMD. relaxed
# warnings + compat shims). GPLv2 (c) Nero AG. #331.
FAAD2_SOURCES = $(wildcard media/faad2/*.c)
FAAD2_CFLAGS = -Imedia/faad2/compat -Imedia/faad2 -Imedia/faad2/codebook -include media/faad2/faad_config.h $(filter-out -Wall -Wextra -Werror,$(CFLAGS)) -w
FAAD2_OBJECTS = $(patsubst %.c,$(OBJDIR)/%.o,$(FAAD2_SOURCES))

# VFS sources
VFS_SOURCES = $(wildcard $(VFS_DIR)/*.c)

# Apps sources
APPS_SOURCES = $(wildcard $(APPS_DIR)/*.c)

# Bluetooth stack sources (#372)
BT_SOURCES = $(wildcard $(BT_DIR)/*.c)
WIFI_SOURCES = $(wildcard $(WIFI_DIR)/*.c)

# All C sources
ALL_C_SOURCES = media/audio_decode.c media/mp3.c media/vorbis.c media/flac.c media/wav.c media/opus.c media/aac.c media/audio_selftest.c \
                $(LIBMAD_SOURCES) \
                $(TREMOR_SOURCES) \
                $(DRFLAC_SOURCES) \
                $(OPUS_SOURCES) \
                $(FAAD2_SOURCES) \
                $(C_SOURCES) \
                $(MM_SOURCES) \
                $(CPU_SOURCES) \
                $(VIDEO_SOURCES) \
                $(DRIVER_SOURCES) \
                $(NET_SOURCES) \
                $(NET_TLS_SOURCES) \
                $(NET_SSH_SOURCES) \
                $(FS_SOURCES) \
                $(SHELL_SOURCES) \
                $(GUI_SOURCES) \
                $(GUI_SETTINGS_SOURCES) \
                $(EXEC_SOURCES) \
                $(DOS_SOURCES) \
                $(PROC_SOURCES) \
                $(IO_SOURCES) \
                $(SECURITY_SOURCES) \
                $(IPC_SOURCES) \
                $(SYNC_SOURCES) \
                $(CRYPTO_SOURCES) \
                $(GAMES_SOURCES) \
                $(VFS_SOURCES) \
                $(APPS_SOURCES) \
                $(BT_SOURCES) \
                $(WIFI_SOURCES) \

# All ASM sources
ALL_ASM_SOURCES = $(ASM_SOURCES) \
                  $(MM_ASM) \
                  $(CPU_ASM) \
                  $(PROC_ASM)

# Object files
# Use _asm.o suffix for assembly to avoid collision with C files
C_OBJECTS = $(patsubst %.c,$(OBJDIR)/%.o,$(ALL_C_SOURCES))
ASM_OBJECTS = $(patsubst %.asm,$(OBJDIR)/%_asm.o,$(ALL_ASM_SOURCES))
OBJECTS = $(C_OBJECTS) $(ASM_OBJECTS) $(DOOM_OBJECTS)

# Default target
.PHONY: all
all: increment_build dirs $(TARGET)

# Create directories
.PHONY: dirs
dirs:
	@mkdir -p $(OBJDIR)
	@mkdir -p $(OBJDIR)/$(MM_DIR)
	@mkdir -p $(OBJDIR)/$(CPU_DIR)
	@mkdir -p $(OBJDIR)/$(VIDEO_DIR)
	@mkdir -p $(OBJDIR)/$(DRIVERS_DIR)
	@mkdir -p $(OBJDIR)/$(NET_DIR)
	@mkdir -p $(OBJDIR)/$(NET_TLS_DIR)
	@mkdir -p $(OBJDIR)/$(FS_DIR)
	@mkdir -p $(OBJDIR)/$(SHELL_DIR)
	@mkdir -p $(OBJDIR)/$(GUI_DIR)
	@mkdir -p $(OBJDIR)/$(GUI_SETTINGS_DIR)
	@mkdir -p $(OBJDIR)/$(EXEC_DIR)
	@mkdir -p $(OBJDIR)/$(DOS_DIR)
	@mkdir -p $(OBJDIR)/$(PROC_DIR)
	@mkdir -p $(OBJDIR)/$(IO_DIR)
	@mkdir -p $(OBJDIR)/$(SECURITY_DIR)
	@mkdir -p $(OBJDIR)/$(IPC_DIR)
	@mkdir -p $(OBJDIR)/$(SYNC_DIR)
	@mkdir -p $(OBJDIR)/$(CRYPTO_DIR)
	@mkdir -p $(OBJDIR)/$(GAMES_DIR)
	@mkdir -p $(OBJDIR)/$(DOOM_DIR)
	@mkdir -p $(OBJDIR)/$(VFS_DIR)
	@mkdir -p $(OBJDIR)/$(APPS_DIR)
	@mkdir -p $(OBJDIR)/$(BT_DIR)
	@mkdir -p $(OBJDIR)/$(WIFI_DIR)

# --- Rust-in-kernel (#404 / #478 Phase A: Rust present in the image, NO C
# behavior change) --------------------------------------------------------
# Toolchain is PINNED for reproducibility via rust-toolchain.toml (channel
# 1.97.0, target x86_64-unknown-none). The rustup shim reads that file when
# make runs here; the version gate below then FAILS the build if the active
# rustc is not the pinned version, so a `rustup update` can never silently
# change the golden build path. x86_64-unknown-none already matches the C
# kernel ABI (code-model=kernel, redzone off, -mmx/-sse/-sse2 + soft-float,
# panic=abort) - no custom target JSON needed.
RUST_LIB         = librustkern.a
RUSTC            = rustc
RUST_PIN_VERSION = 1.97.0
RUSTFLAGS = --edition 2021 --crate-type staticlib --target x86_64-unknown-none \
            -C opt-level=2 -C panic=abort

# rustkern.rs is the crate ROOT only (panic handler + probes + `mod`
# declarations). The subsystems live in rustkern/*.rs, one module per subsystem
# (#404 / #526), so two agents working on different subsystems no longer edit
# the same file. rustc compiles the whole crate from the root in ONE
# invocation, so EVERY module is a prerequisite: touching rustkern/mono.rs must
# rebuild the archive.
RUST_MODULES = $(wildcard rustkern/*.rs)

$(RUST_LIB): rustkern.rs $(RUST_MODULES) rust-toolchain.toml
	@echo "Compiling Rust static lib $(RUST_LIB) (pinned rustc $(RUST_PIN_VERSION))..."
	@ver=`$(RUSTC) --version | awk '{print $$2}'`; \
	 if [ "$$ver" != "$(RUST_PIN_VERSION)" ]; then \
	   echo "ERROR: active rustc $$ver != pinned $(RUST_PIN_VERSION) (reproducibility gate)."; \
	   echo "       Install the pin: rustup toolchain install $(RUST_PIN_VERSION) --profile minimal &&"; \
	   echo "       rustup target add --toolchain $(RUST_PIN_VERSION) x86_64-unknown-none"; \
	   exit 1; \
	 fi
	$(RUSTC) $(RUSTFLAGS) -o $@ rustkern.rs

# --- Rust FFI symbol gate (#404 / #526) ------------------------------------
# FAILS THE BUILD if rustkern stops exporting a symbol rust-symbols.manifest
# declares, or exports one it does not. A silently deleted export otherwise
# BUILDS CLEAN: most exports sit behind a -DRUST_* strangler flag, so no C
# caller is compiled in to fail the link. blame.md records two near-misses
# (a whole-file push about to delete #502's six live TLS 1.2 functions; a
# "clobber" whose only tell was nm vs grep disagreeing).
#
# The tool lives INSIDE kernel/ on purpose. `make lint` reaches for
# ../tools/concurrency-lint, which resolves to source/tools on the build
# container and DOES NOT EXIST there, so that lint cannot run where the kernel
# is actually built. A gate that silently skips is worse than no gate, so this
# one travels with the Makefile that invokes it.
RUST_SYMGATE  = tools/rust-symbol-gate/rust-symbol-gate
RUST_MANIFEST = rust-symbols.manifest

.PHONY: rust-symbol-gate
rust-symbol-gate: $(RUST_LIB)
	@if [ ! -f $(RUST_SYMGATE) ]; then \
	   echo "ERROR: $(RUST_SYMGATE) is missing. The Rust symbol gate cannot run,"; \
	   echo "       and a gate that skips silently is how a subsystem disappears."; \
	   exit 1; \
	 fi
	@python3 $(RUST_SYMGATE) --archive $(RUST_LIB) --manifest $(RUST_MANIFEST) --nm nm

# Report manifest drift without failing (for when you are knowingly adding an
# export and want the list of what to declare).
.PHONY: rust-symbol-update
rust-symbol-update: $(RUST_LIB)
	@python3 $(RUST_SYMGATE) --archive $(RUST_LIB) --manifest $(RUST_MANIFEST) --nm nm --update

# Link kernel
# librustkern.a is linked LAST (after all C objects) - the proven strangler
# link order from the PoC, so archive members are pulled to satisfy C
# references. In Phase A NO C code references Rust yet, so we force-retain the
# Rust translation unit with `-u rust_marker`: this pulls rustkern.rs's object
# (rust_probe + rust_marker + the #[panic_handler], which in turn resolves its
# `U rust_kernel_panic` against main.o) so REAL Rust machine code is genuinely
# in the image and the panic wiring is link-tested. NOTHING calls these symbols
# at runtime, so boot behavior is unchanged. `-u` must precede the archive so
# the pending undefined is satisfied when ld reaches it.
RUST_FORCE = -u rust_marker
# `| rust-symbol-gate` is an ORDER-ONLY prerequisite: the gate is .PHONY so it
# runs on every build (a normal prereq would be skipped once the archive is up
# to date, which is exactly the staleness hole that lets a deletion through),
# but being order-only it does not mark kernel.elf out of date, so it never
# forces a needless 15 MB relink. If the gate fails, make stops here and the
# kernel is not linked.
$(TARGET): $(OBJECTS) $(RUST_LIB) | rust-symbol-gate
	@echo "Linking $(TARGET)..."
	$(LD) $(LDFLAGS) -o $@ $(OBJECTS) $(RUST_FORCE) $(RUST_LIB)
	@echo "Kernel built successfully!"
	@ls -lh $(TARGET)
	@echo ""
	@echo "Kernel sections:"
	@size $(TARGET)


# --- CFLAGS stamp (#526): a -D change MUST rebuild -------------------------
# make decides what to recompile by comparing .o mtimes against .c mtimes. A
# changed -D never touches a .c, so with a different flag set every object is
# still "up to date" and the new flag compiles NOTHING. This is not theoretical:
# `make CVTEST=1` then `make CVTAMPER=1` produced two kernels with the IDENTICAL
# md5 (a71499c4), so the "tamper" run silently exercised the NORMAL kernel. A
# tamper build that compiles nothing passes vacuously, and that was 30 seconds
# from being recorded as a real result (blame.md false-PASS #10). The documented
# workaround was "make clean between every flag permutation", i.e. remember or
# be wrong; this makes the dependency real instead.
#
# The stamp's CONTENT is the fully-expanded CFLAGS (recipes expand it at run
# time, so every ifeq above is already folded in). It is rewritten ONLY when the
# flags actually differ, so its mtime moves only on a real change and a no-op
# make stays a no-op. Every C compile rule below depends on it.
#
# NOTE: all the derived profiles (TTF_CFLAGS, DOOM_CFLAGS, LIBMAD_CFLAGS, ...)
# are $(subst)-ed from CFLAGS, so this one stamp covers them all.
FLAGS_STAMP = $(OBJDIR)/.cflags-stamp

.PHONY: flags-stamp-force
flags-stamp-force:

$(FLAGS_STAMP): flags-stamp-force
	@mkdir -p $(OBJDIR)
	@printf '%s\n' '$(CFLAGS)' > $@.tmp
	@if cmp -s $@.tmp $@; then \
	   rm -f $@.tmp; \
	 else \
	   if [ -f $@ ]; then echo ">>> CFLAGS CHANGED - forcing a full C rebuild (#526)"; fi; \
	   mv $@.tmp $@; \
	 fi

# TTF/math files need SSE enabled for floating-point (stb_truetype uses double)
TTF_CFLAGS = $(subst -mno-sse ,,$(subst -mno-sse2 ,,$(subst -mno-mmx ,,$(CFLAGS)))) -msse -msse2
TTF_CFLAGS_RELAXED = $(subst -Werror,,$(TTF_CFLAGS)) -w

$(OBJDIR)/gui/math.o: gui/math.c $(FLAGS_STAMP)
	@echo "Compiling $< (SSE enabled)..."
	@mkdir -p $(dir $@)
	$(CC) $(TTF_CFLAGS) -c $< -o $@

$(OBJDIR)/gui/ttf.o: gui/ttf.c gui/stb_truetype.h gui/ttf.h $(FLAGS_STAMP)
	@echo "Compiling $< (SSE enabled, relaxed warnings)..."
	@mkdir -p $(dir $@)
	$(CC) $(TTF_CFLAGS_RELAXED) -c $< -o $@

# Compile DOOM files with relaxed warnings (legacy code)
$(OBJDIR)/$(DOOM_DIR)/%.o: $(DOOM_DIR)/%.c $(FLAGS_STAMP)
	@echo "Compiling $< (DOOM, relaxed)..."
	@mkdir -p $(dir $@)
	$(CC) $(DOOM_CFLAGS) -c $< -o $@

# Compile libmad core (integer fixed-point, relaxed warnings)
$(OBJDIR)/media/libmad/%.o: media/libmad/%.c $(FLAGS_STAMP)
	@echo "Compiling $< (libmad)..."
	@mkdir -p $(dir $@)
	$(CC) -Imedia/libmad/compat $(LIBMAD_CFLAGS) -c $< -o $@

# Compile MP3 wrapper (pulls libmad headers; keep warnings, drop -Werror)
$(OBJDIR)/media/mp3.o: media/mp3.c $(FLAGS_STAMP)
	@echo "Compiling $< (mp3 wrapper)..."
	@mkdir -p $(dir $@)
	$(CC) $(filter-out -Werror,$(CFLAGS)) -c $< -o $@

# Compile Tremor + libogg core (integer fixed-point, relaxed warnings)
$(OBJDIR)/media/tremor/%.o: media/tremor/%.c $(FLAGS_STAMP)
	@echo "Compiling $< (tremor)..."
	@mkdir -p $(dir $@)
	$(CC) $(TREMOR_CFLAGS) -c $< -o $@

# Compile OGG Vorbis wrapper (pulls tremor public headers + compat stdio)
$(OBJDIR)/media/vorbis.o: media/vorbis.c $(FLAGS_STAMP)
	@echo "Compiling $< (vorbis wrapper)..."
	@mkdir -p $(dir $@)
	$(CC) -Imedia/tremor/compat -Imedia/tremor $(filter-out -Werror,$(CFLAGS)) -c $< -o $@

# Compile dr_flac core (integer FLAC decoder, no FP; relaxed warnings) #331
$(OBJDIR)/media/dr_flac/%.o: media/dr_flac/%.c $(FLAGS_STAMP)
	@echo "Compiling $< (dr_flac)..."
	@mkdir -p $(dir $@)
	$(CC) $(DRFLAC_CFLAGS) -c $< -o $@

# Compile FLAC wrapper (pulls dr_flac public header + compat) #331
$(OBJDIR)/media/flac.o: media/flac.c $(FLAGS_STAMP)
	@echo "Compiling $< (flac wrapper)..."
	@mkdir -p $(dir $@)
	$(CC) -Imedia/dr_flac/compat -Imedia/dr_flac $(filter-out -Werror,$(CFLAGS)) -c $< -o $@

# Compile WAV decoder (own code; keep warnings, drop -Werror like other wrappers) #331
# Compile faad2 core (integer fixed-point AAC-LC decoder, no FP; relaxed) #331
$(OBJDIR)/media/faad2/%.o: media/faad2/%.c $(FLAGS_STAMP)
	@echo "Compiling $< (faad2)..."
	@mkdir -p $(dir $@)
	$(CC) $(FAAD2_CFLAGS) -c $< -o $@

# Compile AAC/M4A wrapper (pulls faad2 public header + own MP4 atom parser) #331
$(OBJDIR)/media/aac.o: media/aac.c $(FLAGS_STAMP)
	@echo "Compiling $< (aac wrapper)..."
	@mkdir -p $(dir $@)
	$(CC) -Imedia/faad2/compat -Imedia/faad2 $(filter-out -Werror,$(CFLAGS)) -c $< -o $@

$(OBJDIR)/media/wav.o: media/wav.c $(FLAGS_STAMP)
	@echo "Compiling $< (wav)..."
	@mkdir -p $(dir $@)
	$(CC) $(filter-out -Werror,$(CFLAGS)) -c $< -o $@

# Compile libopus core (integer fixed-point decode subset, no FP; relaxed) #331
$(OBJDIR)/media/opus/%.o: media/opus/%.c $(FLAGS_STAMP)
	@echo "Compiling $< (opus)..."
	@mkdir -p $(dir $@)
	$(CC) $(OPUS_CFLAGS) -c $< -o $@

# Compile Opus wrapper (pulls libopus public headers + libogg framing) #331
$(OBJDIR)/media/opus.o: media/opus.c $(FLAGS_STAMP)
	@echo "Compiling $< (opus wrapper)..."
	@mkdir -p $(dir $@)
	$(CC) -Imedia/opus/compat -Imedia/tremor -Imedia/opus -Imedia/opus/include $(filter-out -Werror,$(CFLAGS)) -c $< -o $@

# Compile C files
$(OBJDIR)/%.o: %.c $(FLAGS_STAMP)
	@echo "Compiling $<..."
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

# Compile ASM files (use _asm.o suffix to avoid collision with C files)
$(OBJDIR)/%_asm.o: %.asm
	@echo "Assembling $<..."
	@mkdir -p $(dir $@)
	$(AS) $(ASFLAGS) $< -o $@

# Clean
.PHONY: clean
clean:
	@echo "Cleaning..."
	rm -rf $(OBJDIR) $(TARGET) $(RUST_LIB)
	@echo "Clean complete."

# Show info
.PHONY: info
info:
	@echo "C Sources: $(ALL_C_SOURCES)"
	@echo "ASM Sources: $(ALL_ASM_SOURCES)"
	@echo "Objects: $(OBJECTS)"

# Concurrency lint (#426): fail on any NEW hand-rolled busy-wait / spin /
# poll loop that should use the shared wait-queue (sync/waitq.h) or futex
# (sync/futex.c). Scans kernel/ + userland/ from the source root. Does NOT
# depend on any build artifact, so it never triggers a kernel rebuild.
.PHONY: lint
lint:
	@python3 ../tools/concurrency-lint/concurrency-lint --root ..

# Re-baseline the concurrency-lint allowlist to the current tree (run only
# when knowingly accepting reviewed legacy sites; commit the allowlist).
.PHONY: lint-baseline
lint-baseline:
	@python3 ../tools/concurrency-lint/concurrency-lint --root .. --update-allowlist

# Full categorized backlog of every hit (allowlisted or not).
.PHONY: lint-report
lint-report:
	@python3 ../tools/concurrency-lint/concurrency-lint --root .. --report

# Install to UEFI boot directory
.PHONY: install
install: $(TARGET)
	@echo "Installing kernel to ../uefi/boot/"
	@mkdir -p ../uefi/boot
	cp $(TARGET) ../uefi/boot/

# Increment build number before building
increment_build:
	@./increment_build.sh
	@rm -f obj/main.o obj/video/graphics.o obj/proc/syscall.o obj/gui/desktop.o  # Force recompile of version-dependent files
