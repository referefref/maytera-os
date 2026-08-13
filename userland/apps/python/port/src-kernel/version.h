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
// error (proven on a test VM against an openssl s_server bad-cert endpoint).
// #433 xHCI HID enumeration race fix: mark-port-enumerated-on-success +
// bounded retry, warm-reboot per-port PP off->on power-cycle, CONFIG_EP return
// check + retry for HID interrupt-IN, and a periodic port re-scan worker.
// #71 / Cirrus CS4208 audio: /AUDIOLOG.TXT diagnostic (codec identity + output-
// path EAPD/amp/GPIO + output-stream DMA state), plus the CS4208 speaker-amp
// enable (EAPD on the speaker pins + GPIO0 mask/dir/data on the AFG node,
// mirroring Linux patch_cirrus.c). fresh5 diagnostic image.
#define MAYTERA_BUILD_NUMBER 686

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
