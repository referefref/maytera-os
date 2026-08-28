# Makefile for MayteraOS 64-bit Kernel v1.8.0

# Compiler and tools
CC = gcc
AS = nasm
LD = ld
OBJCOPY = objcopy

# Target
# #670 KERNEL ADDRESS DISCLOSURE. $(TARGET) is the SHIPPED kernel and is
# STRIPPED; $(DEBUG_TARGET) is the full unstripped link output and stays in the
# build tree, OFF the image. Before this, the ELF placed on the ESP (and thus at
# world-readable /KERNEL.ELF, /boot/kernel.elf, /EFI/BOOT/kernel.elf) carried
# ~10,340 .symtab entries plus 11 DWARF sections: the exact address of every
# kernel function, handed to any Ring-3 process that can open a file.
# Nothing in this tree reads symbols from the ELF at RUNTIME. Every addr2line
# workflow (mm/demand.c, sync/noblock.c, security/stack_guard.c) prints a raw
# address to SERIAL and is resolved on the BUILD host, so those now resolve
# against $(DEBUG_TARGET). The UEFI loader (uefi/bootloader.c) walks program
# headers only and never dereferences e_shoff, and strip leaves every PT_LOAD's
# file offset, vaddr, filesz, memsz and flags plus the entry point untouched
# (measured: the three LOAD segments are byte-identical pre/post strip;
# 15,227,808 -> 4,225,104 bytes, x3 boot paths off the 256 MiB ESP).
TARGET = kernel.elf
DEBUG_TARGET = kernel.dbg.elf

# Directories
SRCDIR = .
OBJDIR = obj
MM_DIR = mm
CPU_DIR = cpu
VIDEO_DIR = video
DRIVERS_DIR = drivers
VIRTIO_DIR = drivers/virtio
NET_DIR = net
NET_TLS_DIR = net/tls
FS_DIR = fs
GRAPHFS_DIR = fs/graphfs
SHELL_DIR = shell
GUI_DIR = gui
EXEC_DIR = exec
DOS_DIR = dos
PROC_DIR = proc
IO_DIR = io
SECURITY_DIR = security
IPC_DIR = ipc
SYNC_DIR = sync
CRYPTO_DIR = crypto
VFS_DIR = vfs
BT_DIR = bt
WIFI_DIR = drivers/net/wifi

# GCC include path for freestanding headers (stdarg.h, etc.)
GCC_INCLUDE = $(shell $(CC) -print-file-name=include)

# Compiler flags for freestanding 64-bit kernel
# --- Header dependency tracking (#600) -------------------------------------
# WITHOUT -MMD -MP make knows NOTHING about headers: an object depends only on
# its .c and the flags stamp, so editing a header and running an INCREMENTAL
# make recompiles NOTHING. Measured on this Makefile: editing net/tcp.h (a
# #define used by net/tcp.c) and re-running make produced a BYTE-IDENTICAL
# kernel.elf (md5 1206f09b...). That is not merely a stale build: if the header
# change moves a struct field, changes an enum value, or edits an inline
# function, the TUs that DO get rebuilt later are linked against a DIFFERENT
# definition than the ones that were not, i.e. a silent cross-TU ABI mismatch
# that presents as random memory corruption at runtime. It has been masked only
# because ship builds run `make clean` first.
#
# -MMD makes every compile emit a matching obj/<path>.d listing the headers it
# actually included (quoted/project headers only; -nostdinc means there are no
# system headers anyway). -MP adds a phony target for each header so DELETING a
# header does not break the build with "No rule to make target". Both are
# preprocessor-output-only: they do not change generated code (verified by md5
# against the pre-fix clean build). The `# #617 BUILD TRAP (the real cause of the long-standing "make succeeds in 0.1 s
# and produces a BYTE-IDENTICAL kernel.elf from modified sources", #614):
# `-include $(DEPFILES)` below is read BEFORE the `all:` rule is defined, so the
# FIRST target make sees is whatever happens to be first in the first .d file.
# That became the default goal, and a bare `make` silently "built" it, printed
# NOTHING, exited 0 and left the previous kernel.elf in place. It only looked
# correct after `make clean` (which deletes the .d files, so `all` wins again).
# Pin the default goal explicitly so a bare `make` can never be hijacked.
.DEFAULT_GOAL := all

-include $(DEPFILES)` below is what
# makes make actually READ them.
#
# Sibling of the #526 CFLAGS stamp: that made a -D change rebuild; this makes a
# header change rebuild. Neither is a substitute for the other.
# --- #618 REPRODUCIBLE BUILD ------------------------------------------------
# -ffile-prefix-map makes DWARF comp_dir/__FILE__ independent of WHERE the tree
# was unpacked, and REPRO_DATE/REPRO_TIME pin the __DATE__/__TIME__ strings that
# panic.c/devlog.c/settings.c embed. Without both, two builds of the SAME commit
# produce different bytes and no gate can prove a shipped kernel came from its
# stamped source. Leave REPRO_* empty for a normal dev build.
REPRO_DATE ?=
REPRO_TIME ?=
REPRO_DEFS =
ifneq ($(REPRO_DATE),)
REPRO_DEFS = -DMAYTERA_BUILD_DATE='"$(REPRO_DATE)"' -DMAYTERA_BUILD_TIME='"$(REPRO_TIME)"'
endif

# -DCONFIG_NO_SMAP (#645) IS NOT A LEFTOVER DEBUG SWITCH. DO NOT DELETE IT
# CASUALLY. SMAP is fully implemented and wired: the CR4.SMAP write with a
# readback (rustkern/seccore.rs), the stac/clac brackets on all four user-copy
# primitives including every fault-fixup path (mm/uaccess.asm), CLAC+CLD at the
# single interrupt chokepoint (cpu/idt.asm), AC and DF cleared on SYSCALL entry
# (proc/syscall.c IA32_FMASK, #668), a boot-time pre-flight probe that refuses
# to arm if the kernel's own memory is user-accessible, and a /NOSMAP.TXT ESP
# escape hatch. All of that is DELIBERATELY NOT ARMED, and this flag is what
# holds it off.
#
# WHY, REWRITTEN FROM MEASUREMENT AT #19 (2026-08-11). The three checks this
# comment used to list as prerequisites have now been RUN, on a machine, with
# CR4.SMAP actually set. Two of them are done and the third came back clean.
# What is left is ONE class, and it is the only thing holding this flag on.
#
#   1. DONE. The ledger's 125 entries were re-derived: about 35 were already
#      fixed and never delisted, about 25 were never user accesses at all, and
#      it MISSED several real ones. The genuine sites are converted, to
#      copy_*_user or to a uaccess_begin()/uaccess_end() bracket around the
#      ACCESS. See kernel/smap-uaccess.manifest for the whole accounting.
#   2. DONE. The manifest's UNDETECTABLE section (the VFS/driver `void *buf`
#      route the lint structurally cannot see) is converted and still listed,
#      as prose, because nothing in the build can notice if one is deleted.
#   3. CLEAN, MEASURED. An armed boot prints
#        [SECURITY] SMAP pre-flight: kernel .data/stack/text ... U/S=supervisor
#      so the firmware's adopted identity map does NOT make SMAP unusable here.
#      SMEP and SMAP both take the CR4 write (cr4=0x300668, read back).
#
# THE ONE REMAINING BLOCKER: a Ring-3 PATH STRING reaches the filesystem raw,
# and the FS walks it with plain Ring-0 loads (fat_read_file's /SMB and /NFS
# prefix tests, syscall_path.h's predicates, ext2, perms). Measured on an armed
# boot: #PF at RIP inside fat_read_file, CR2 in the user window, err=0x1. It is
# a class of hundreds of loads, so the fix is a dispatcher-level string bounce,
# not a list of brackets. That bounce was built and is NOT in the tree: it
# regressed the desktop for a reason that was not root-caused. The manifest's
# gap section carries the full writeup, including the two real bugs found in it
# and the leading unproven suspect. Read it before trying again.
#
# TWO THINGS THE ARMED BOOT ALSO PROVED, both now fixed and both worth knowing:
#   * CR4 is PER-CPU and cpu/smp.c's ap_entry() never set SMEP or SMAP, so on a
#     2-core box half the machine ran unprotected while the banner said ENABLED.
#   * `make SMAPTEST=1` arms a deliberate unsanctioned Ring-0 user read; with
#     SMAP armed it now #PFs (err=0x1 P|R|S, CR2 = the probed user VA). That
#     negative control is what caught the AP hole and a test-harness VM that had
#     no SMAP in CPUID at all, both of which had produced clean green boots.
#
# `make smap-uaccess-lint` refuses to build with this flag removed while the
# manifest carries a gap entry, so the ordering is enforced, not remembered.
CFLAGS = -ffile-prefix-map=$(CURDIR)=/maytera-kernel $(REPRO_DEFS) \
-ffreestanding \
         -fstack-protector-strong \
         -mstack-protector-guard=global \
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
         -MMD -MP \
         -DCONFIG_NO_SMAP \
         -DRUST_IP_CHECKSUM \
         -DRUST_EXT2_DIRFIND \
         -DRUST_EXT2_DIRADD \
         -DRUST_EXT2_DIRREPOINT \
         -DRUST_TCP_CHECKSUM \
         -DRUST_UDP_CHECKSUM \
         -DRUST_SHA256 \
         -DRUST_SHA512 \
         -DRUST_MD5 \
         -DRUST_MD4 \
         -DRUST_CHACHA20 \
         -DRUST_AES \
         -DRUST_GHASH \
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
         -DRUST_TLS_POOL \
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
         -DRUST_SECCORE \
         -DRUST_ISO9660 \
         -I$(SRCDIR) \
         -isystem $(GCC_INCLUDE)

# #189: SILENCE-ON-STARVE for the HD Audio output ring (rustkern/hdastarve.rs).
# ON by default, and it must stay on: without it a producer that stops feeding
# but leaves its stream open leaves the cyclic DMA engine replaying its last
# ring lap indefinitely, which is both the most audible defect on the real
# iMac14,4 and a way for a capture-based audio test to PASS a program that has
# already exited.
#
# The toggle exists ONLY so the two arms of that proof can be built from ONE
# commit: `make HDA_STARVE=0` reproduces the defect, `make` fixes it, and the
# two binaries then differ in nothing else. Never ship HDA_STARVE=0.
HDA_STARVE ?= 1
ifeq ($(HDA_STARVE),1)
CFLAGS += -DRUST_HDA_STARVE
endif

# #190: the ONE hardware output stream is shared by MAX_STREAMS software
# streams, and audio_stop() used to stop it unconditionally - so the boot chime
# finishing silenced a DOS guest's music PERMANENTLY and discarded 76-88% of its
# OPL2 register writes for the rest of the session (measured, #187). ON by
# default and it must stay on.
#
# Same reason as HDA_STARVE for the toggle: `make SHARED_ENGINE=0` reproduces
# the stall so both arms of the proof come from ONE commit. Never ship it off.
SHARED_ENGINE ?= 1
ifeq ($(SHARED_ENGINE),1)
CFLAGS += -DAUDIO_SHARED_ENGINE_FIX
endif

# #500/#503: `make SECTEST=1` dispatches SYS_SECTEST (the negative-control
# battery). DEBUG ONLY - never set for a golden/shipped kernel. Appended to the
# real CFLAGS rather than passed as `make CFLAGS+=...` on the command line,
# because that form REPLACES CFLAGS wholesale (blame.md) and would silently
# build the whole kernel without -ffreestanding.
ifeq ($(SECTEST),1)
CFLAGS += -DSECTEST_SYSCALL
endif

# (local 79) make FOCUSDIAG=1 arms the compiled-out focus-path probe in
# kernel/gui/window.c (see the comment above FOCUSDIAG_ENABLED there). DEBUG
# ONLY - never set for a golden/shipped kernel. The probe comment used to say
# `make CFLAGS_EXTRA=-DFOCUSDIAG_ENABLED=1`, which this Makefile never read
# (CFLAGS_EXTRA was not a variable referenced anywhere below); that
# instruction was untested and silently did nothing. Same append-to-CFLAGS
# form as SECTEST above.
ifeq ($(FOCUSDIAG),1)
CFLAGS += -DFOCUSDIAG_ENABLED=1
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

# ============================================================================
# #785: MAYTERA_SSHD - does the SSH SERVER ship in this kernel AT ALL?
#
# Usage:  make MAYTERA_SSHD=1
#
# THERE ARE TWO SWITCHES AND THEY ARE NOT THE SAME SWITCH. Which is which:
#
#   COMPILE TIME (this flag) decides whether the sshd CODE IS ON THE IMAGE.
#     Default OFF. With it off, net/ssh/ssh2_server.c is not compiled at all,
#     main.c contains no call to ssh_server_start(), and proc/services.c
#     registers no "sshd" service. There is no listener on the image to enable,
#     to misconfigure, or to regress. The SSH CLIENT (net/ssh/ssh2.c, used by
#     /APPS/SSH) is unaffected and always builds: connecting OUT is not the
#     same risk as accepting IN.
#
#   RUN TIME (/CONFIG/SSHD.CFG, enable=1) decides whether a kernel that HAS the
#     code actually LISTENS. Default OFF, and an absent or malformed config
#     means no listener (#697). This is changeable on a running system without
#     a rebuild, which is what makes "enable/disable as needed" true.
#
# MAYTERA_SSHD=1 ON ITS OWN DOES NOT OPEN A PORT. It ships a service an operator
# can then enable. Both switches have to say yes. That is deliberate: #697 was
# caused by exactly one condition (a host key exists) being treated as consent
# to expose a network service.
#
# NOTE: this flag CHANGES THE KERNEL BYTES, so build-golden.sh stamps its value
# into /BUILDINFO.TXT and invariant-gate.sh --verify-kernel replays it. A
# reproducibility check that rebuilt with a different flag would compare two
# different kernels and report drift that is not there.
#
# Appended to the real CFLAGS rather than passed as `make CFLAGS+=...`, because
# that form REPLACES CFLAGS wholesale (blame.md) and would silently build the
# whole kernel without -ffreestanding. Kept at TOP LEVEL, never nested inside
# another ifeq: #624 records a flag that was nested by accident and therefore
# built a byte-identical kernel while appearing to be set.
ifeq ($(MAYTERA_SSHD),1)
CFLAGS += -DMAYTERA_SSHD
endif

# #426 Phase 2 (wq_assert_may_block). DEBUG ONLY - never for a golden/shipped
# kernel. Same append-to-CFLAGS form and same reason as SECTEST above.
#   make NOBLOCKPANIC=1 - a no-block context that blocks kpanic()s on the FIRST
#                         offence instead of printing a [WQBLOCK] report and
#                         continuing. Use when hunting one.
#   make NOBLOCKTEST=1  - compile in sync/waitq_test.c's DELIBERATE VIOLATION
#                         (a real wait_event() issued with interrupts off), the
#                         negative control that PROVES the assert fires. An
#                         assertion nobody has watched go off is not one.
#   make SMAPTEST=1     - arm a DELIBERATE unsanctioned Ring-0 read of a user
#                         page (exec/elf.c), the negative control that PROVES
#                         CR4.SMAP traps. Never for a golden.
ifeq ($(NOBLOCKPANIC),1)
CFLAGS += -DWQ_NOBLOCK_PANIC
endif
ifeq ($(NOBLOCKTEST),1)
CFLAGS += -DWQ_NOBLOCK_SELFTEST
endif

# AUDLEAD: `make AUDSTARVETEST=1` arms a DELIBERATE producer stall in
# drivers/audio.c, two seconds into any file playback, long enough to drain the
# whole 743 ms HDA ring. Same argument and same placement as NOBLOCKTEST above:
# an underrun counter that has only ever been observed reading ZERO is not
# evidence that underruns are being counted, it is evidence that nothing has
# tested the counter. The shipped counter reads 0 on every clean run, which is
# exactly what a BROKEN counter also reads. NEVER for a golden.
ifeq ($(AUDSTARVETEST),1)
CFLAGS += -DAUDIO_STARVE_SELFTEST
endif

# #62: `make XHCIEPTEST=1` / `=2` arm the destructive control-endpoint recovery
# proof in drivers/xhci.c. Same argument and same top-level placement as
# NOBLOCKTEST / CANARYTEST / SMAPTEST above: a recovery path nobody has watched
# repair a REAL halted endpoint is a comment, not a fix. NEVER for a golden.
#   =1  GREEN arm: recovery compiled in. Expect [EPTEST] VERDICT ... PASS.
#   =2  RED arm:   -DXHCI_EPTEST_NORECOVER removes the recovery call, restoring
#                  the pre-fix behaviour. Expect [EPTEST] VERDICT ... FAIL.
# If the RED arm also passes, the test proves nothing and must be discarded.
ifeq ($(XHCIEPTEST),1)
CFLAGS += -DXHCI_EPTEST
endif
ifeq ($(XHCIEPTEST),2)
CFLAGS += -DXHCI_EPTEST -DXHCI_EPTEST_NORECOVER
endif

# #156: `make XHCIMSI=1` arms #139's xHCI MSI. IT IS OFF IN A NORMAL/GOLDEN
# BUILD. #139 added the first xHCI MSI this kernel has ever had, and it could
# not run on the test rig (qemu-xhci exposes MSI-X 0x11, not MSI 0x05, so
# pci_enable_msi() declined), so xhci_msi_isr() shipped in build 1921 having
# executed zero times anywhere. The owner's iMac14,4 is the one machine that
# can execute it, and 1921 hangs at boot there where 1910 did not. Until the
# handler has been watched working on that machine, arming it is opt-in.
#
# OFF is the pre-#139 behaviour exactly: the event ring is drained by the two
# always-armed timer-driven workers. #139's Endpoint Context Interval fix (the
# `+= 3` that made every full-speed interrupt endpoint 8x slower) lives in
# xhci_configure_endpoint_ep() and is UNAFFECTED by this flag - it is in every
# build either way, which is the point of gating the MSI and nothing else.
#
# A/B ARM FROM ONE COMMIT, so a bisect control can be pinned to a hash:
#   make            -> MSI off  (control)
#   make XHCIMSI=1  -> MSI on   (test)
# and either build can still be flipped at boot without a rebuild by dropping
# /XHCIMSI.TXT (force on) or /XHCIMSI.OFF (force off, wins) on the FAT ESP.
ifeq ($(XHCIMSI),1)
CFLAGS += -DXHCI_MSI_DEFAULT_ON=1
endif

# #133: `make HIDPARKTEST=1` / `=2` arm the destructive proof that a PARKED HID
# device is actually recovered. Same argument and same top-level placement as
# NOBLOCKTEST / XHCIEPTEST / CANARYTEST above. No VM halts a HID endpoint on its
# own (QEMU does not even complete an in-flight interrupt-IN TD with a
# transaction error on device_del - blame.md #134), so without this arm
# usb_hid_retry_failed() would ship having never executed.
#   =1  GREEN arm: park the device by hand at reports 300 and 900; the re-scan
#                  worker must un-park it and [HIDLAT] gap statistics must
#                  resume. Expect [USB-HID] #133: ... un-parked ... re-arm=OK.
#   =2  RED arm:   -DHID_PARKTEST_NOREVIVE removes the un-park call, restoring
#                  the pre-#133 behaviour. Reports must stop for ever at 300.
# If the RED arm also recovers, the test proves nothing and must be discarded.
# NEVER for a golden build.
ifeq ($(HIDPARKTEST),1)
CFLAGS += -DHID_PARKTEST
endif
ifeq ($(HIDPARKTEST),2)
CFLAGS += -DHID_PARKTEST -DHID_PARKTEST_NOREVIVE
endif

# #624 step 2: `make CANARYTEST=1` arms a DELIBERATE stack smash so the canary
# can be SEEN to fire, exactly as `make NOBLOCKTEST=1` arms a deliberate
# wait_event violation. A canary that has never been observed catching anything
# is not a canary, it is a comment. This must NEVER be set for a golden build.
#
# THIS BLOCK MUST STAY AT TOP LEVEL. It was first written nested inside the
# NOBLOCKTEST conditional above, so -DCANARY_SELFTEST was only ever added when
# BOTH flags were set. `make CANARYTEST=1` therefore built a byte-identical
# kernel and the box booted straight to the desktop with no test at all. A test
# flag that silently does nothing is the same defect class #624 exists to fix;
# it was caught only because the proof-of-fire boot was actually RUN and the
# expected panic did not appear.
ifeq ($(CANARYTEST),1)
CFLAGS += -DCANARY_SELFTEST
endif

# #19/#645: `make SMAPTEST=1` arms ONE deliberate, UNSANCTIONED Ring-0 read of a
# user page in exec/elf.c, immediately after the correctly-bracketed PT_LOAD
# copy. It is the negative half of the SMAP proof and follows NOBLOCKTEST/
# CANARYTEST above for the same reason: a guard that has never been watched
# firing and a guard that is switched off produce identical evidence, namely a
# clean boot. With CR4.SMAP armed this must produce a #PF (err P|S, W clear,
# CR2 = the user VA the line above prints). MUST NEVER be set for a golden.
# This block must stay at TOP LEVEL (see the CANARYTEST note above: nesting it
# is how a test flag comes to silently do nothing).
ifeq ($(SMAPTEST),1)
CFLAGS += -DSMAPTEST
endif

# #745 (#116): `make SPAWNRACETEST=1` REOPENS the spawn fd-install window that
# ticket 116 closed - it drops the preemption bracket in spawn_impl() and yields
# to the scheduler on purpose the moment the child is queued - so the
# [SPAWNFD-RACE] guard can be SEEN to fire. Same argument as NOBLOCKTEST /
# CANARYTEST / SMAPTEST above, and the same rule: this block stays at TOP LEVEL
# (nesting one of these inside another is exactly how CANARYTEST came to
# silently do nothing), and it must NEVER be set for a golden.
ifeq ($(SPAWNRACETEST),1)
CFLAGS += -DSPAWN_FD_RACETEST
endif

# #745 (#62): the negative controls for the redundant tick source. A redundancy
# nobody has watched carry the system is indistinguishable from one that is
# switched off - both produce a clean boot - which is the same argument as
# NOBLOCKTEST/CANARYTEST/SMAPTEST above, and these blocks must likewise stay at
# TOP LEVEL (nesting one inside another is exactly how CANARYTEST came to
# silently do nothing).
#
#   make TICKFAULT=1      - MASK IRQ0 at the 8259 a fixed time after boot, which
#                           reproduces the iMac symptom (a periodic timer that is
#                           programmed and counting, whose interrupt never
#                           arrives) on hardware that would otherwise never fail.
#                           QEMU always delivers, so without this the failover
#                           could only ever be reasoned about, never observed.
#   make TICKNOREDUNDANT=1 - build WITHOUT the redundant source. This is the
#                           control arm: with TICKFAULT=1 it must FREEZE. If it
#                           does not freeze, the fault injector is not working
#                           and the test arm proves nothing.
# NEVER set either for a golden.
ifeq ($(TICKFAULT),1)
CFLAGS += -DTICKWATCH_FAULT_TEST
endif
ifeq ($(TICKNOREDUNDANT),1)
CFLAGS += -DTICKWATCH_NO_REDUNDANT
endif

# #746 fatlock. DEBUG ONLY - never for a golden/shipped kernel.
#   make FATLOCKTEST=1     - compile in fs/fat_lock_test.c: 4 kernel threads
#                            hammer the FAT ESP concurrently while fs/fat.c
#                            counts critical-section occupancy. Expect
#                            "[FATLOCKTEST] RESULT: PASS".
#   make FATLOCKTEST=red   - the NEGATIVE CONTROL. Also defines
#                            FAT_LOCK_SELFTEST_RED, which makes fat_lock_try()
#                            claim the lock unconditionally, i.e. a lock that
#                            excludes nobody. Expect "RESULT: FAIL". A safety
#                            test that has never been seen to fail is not a
#                            test, it is a comment (the #624 lesson).
# #745 local 107 YMM. DEBUG ONLY - never for a golden/shipped kernel, and the
# only configuration in which the kernel contains a VEX-encoded instruction at
# all (proc/fpu_ymm.asm is %ifdef'd on the nasm flag below).
#   make YMMTEST=1   - 4 kernel threads sign all 16 YMM registers, sleep one
#                      tick on a wait queue, and check the value survived the
#                      context switch. Expect "[YMMTEST] RESULT: PASS".
#   make YMMTEST=red - the NEGATIVE CONTROL, and it is the real bug rather than
#                      a mutation of the test: cpu/sse.c still enables AVX
#                      (CR4.OSXSAVE + XCR0.YMM) but forces the fxsave64 save
#                      path, which cannot preserve YMM_Hi128. Expect
#                      "RESULT: FAIL" with low128_mismatches=0 and
#                      high128_mismatches>0.
# Requires a CPU that actually has AVX: a kvm64 guest masks it and the test
# prints SKIPPED. Use cpu=host or cpu=kvm64,+avx.
ifeq ($(YMMTEST),1)
CFLAGS  += -DFPU_YMM_SELFTEST
endif
ifeq ($(YMMTEST),red)
CFLAGS  += -DFPU_YMM_SELFTEST -DFPU_YMM_SELFTEST_RED
endif

ifeq ($(FATLOCKTEST),1)
CFLAGS += -DFAT_LOCK_SELFTEST
endif
ifeq ($(FATLOCKTEST),red)
CFLAGS += -DFAT_LOCK_SELFTEST -DFAT_LOCK_SELFTEST_RED
endif
#   make PROCWAITBENCH=1 - compile in the #230 A/B benchmark (proc/process.c):
#                         the pre-#230 spin and the new wait_event() version each
#                         wait on an identical 2s child, and the parent's own
#                         per-process CPU ticks are printed for both.
ifeq ($(PROCWAITBENCH),1)
CFLAGS += -DPROCWAIT_BENCH
endif
#   make FLIPNETCLI=1   - #632 NEGATIVE CONTROL: restore the pre-#632 placement
#                         of the throttled net_poll() INSIDE sys_fb_flip()s
#                         interrupts-off region, with byte-identical
#                         instrumentation, so the before/after [FLIPPROF]
#                         numbers are directly comparable. Not a supported
#                         configuration; it is the bug, kept buildable so the
#                         measurement can be reproduced.
ifeq ($(FLIPNETCLI),1)
CFLAGS += -DFLIP_NET_INSIDE_CLI=1
endif

#   make FLIPNETBLOCK=1 - #745 (task #62) NEGATIVE CONTROL: restore the pre-fix
#                         mechanism where sys_fb_flip() BLOCKS on net_lock via
#                         net_poll(), and the dedicated netpump thread is not
#                         started. Every counter on the [NETSTARVE] line and in
#                         the /HEARTBEAT.TXT record stays byte-identical, so the
#                         before/after numbers are directly comparable. Not a
#                         supported configuration; it is the bug, kept buildable
#                         so the measurement can be reproduced. Same role, and
#                         the same reason, as FLIPNETCLI above.
ifeq ($(FLIPNETBLOCK),1)
CFLAGS += -DFLIP_NET_BLOCKING=1
endif

# #647 verification builds. DEBUG ONLY - never set for a golden/shipped kernel.
# Same append-to-CFLAGS form and same reason as SECTEST above (`make CFLAGS+=...`
# on the command line REPLACES CFLAGS wholesale, see blame.md).
#
#   make PTWALKTEST=1      - compile in vmm_ptwalk_selftest(): drain the physical
#                            allocator to exhaustion at boot and assert that not
#                            one page it hands out is a live UEFI page table.
#                            Also cross-checks the fast drain against the real
#                            pmm_alloc_page() on a 4096-page prefix.
#
#   make PTWALKNORESERVE=1 - the NEGATIVE CONTROL: identical build with the
#                            reservation in vmm_reserve_boot_page_tables()
#                            REMOVED. This is the #647 bug, kept buildable, so
#                            the self-test can be WATCHED TO FAIL. Combine with
#                            PTWALKTEST=1. A self-test that has only ever been
#                            seen to pass is decorative; #624's CANARYTEST=1
#                            silently built a byte-identical kernel for exactly
#                            this reason and the "proof" was vacuous.
ifeq ($(PTWALKTEST),1)
CFLAGS += -DPTWALK_SELFTEST
endif
ifeq ($(PTWALKNORESERVE),1)
CFLAGS += -DPTWALK_NO_RESERVE
endif

# #75 (enqrace75b): `make ENQTEST=1` compiles in proc/process.c's enqueue-probe
# SELF-TEST, which CONSTRUCTS the state each #75 enqueue probe claims to detect,
# prints whether it fired, and leaves the rest of the boot as the neighbouring-
# state control. Same argument as NOBLOCKTEST / CANARYTEST / SMAPTEST /
# PTWALKTEST above, and #75 is the ticket that most needs it: the [PINENQ] guard
# has never been observed to fire, and "never fired" says nothing about the world
# until the probe has been shown to be capable of firing at all. OFF by default,
# never in the golden - it deliberately manufactures a pinned enqueue.
ifeq ($(ENQTEST),1)
CFLAGS += -DENQ_PROBE_SELFTEST
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

# -DRUST_EXT2_DIRADD (#605, extends the #485 seam): route the live ext2
# directory-entry INSERT (ext2_dirblock_insert in fs/ext2.c, called per block by
# ext2_dir_add) to the Rust port ext2_dirblock_insert_rs (rustkern/ext2.rs)
# instead of the C reference (kept as ext2_dirblock_insert_c). This is the WRITE
# twin of RUST_EXT2_DIRFIND and it exists because #597 fixed a real MEMORY-SAFETY
# bug in that C loop: `slack = rec - used` is unsigned, so a record whose own
# name_len did not fit its own rec_len wrapped it to ~4e9, the room test always
# passed, and the code wrote a rec_len outside the block and memcpy'd the name
# past the end of the kmalloc(block_size) buffer. In Rust that subtraction is
# `checked_sub` and the whole walk is bounds-checked slice access, so the class
# is gone by construction rather than by a hand-added guard. ONLY the pure
# buffer logic crosses the FFI; locking, block I/O, inode/bitmap allocation and
# i_size stay in C. Strangler flag; REMOVE this one line and rebuild to roll
# straight back to C. The boot-time [RUST-DIFF] / [RUST-SEC] ext2_dirinsert
# self-tests in fs/ext2.c compare the two regardless.

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

# -DRUST_ISO9660 (#196 / #404 filesystem tier): routes the three ISO 9660 parse
# entry points used by the disk-image layer (iso_vd_parse / iso_dirrec_at /
# iso_name_decode in dos/diskimg.c) to the Rust ports in rustkern/iso9660.rs
# instead of the verbatim C references (kept as iso_*_c). Drop this one line to
# revert every ISO parse to C.
#
# WHY THIS ONE MATTERS: the input is a CD image the user downloaded. Record
# length, extent length, name length and the name bytes are all disc-controlled,
# and this tree has shipped three heap over-reads in C parsers of that exact
# shape (#476 ext2 dirent, #490 FAT LFN, #597). Every read in the Rust goes
# through a slice spanning exactly the caller buffer, so a lying length is a
# rejected record rather than a read off the end of a heap block.
#
# SPLIT: parse in Rust, I/O in C. dos/imgfile.c (the seekable, 256 KiB-cached
# reader that makes a 653 MB image usable on a 256 MB heap) stays C because it
# is entangled with ext2_read_file_range, the fat_file_t streaming handle and
# the sleeping locks both take.
#
# The boot [RUST-DIFF] iso9660 self-test proves rs == c on this build over
# hand-built malformed vectors plus a 12,000-case fuzz, regardless of the flag.
# HONEST LIMIT: a differential cannot catch a bug both arms share, so format
# correctness is proved separately by the /CDTEST.TXT harness reading a real
# disc and matching a host-computed checksum.

# Assembler flags
ASFLAGS = -f elf64
# #745 local 107: gate proc/fpu_ymm.asm's VEX instructions on the YMMTEST
# builds. This append MUST stay below the assignment above; putting it up with
# the CFLAGS block (where the YMMTEST documentation lives) would be silently
# discarded by it.
ifneq ($(YMMTEST),)
ASFLAGS += -dFPU_YMM_SELFTEST
endif

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

# #703: drivers/virtio/ likewise escaped $(wildcard drivers/*.c). It is a
# complete virtio 1.0+ stack (modern PCI cap transport, split+packed queues,
# blk/gpu/input/console). It is COMPILED AND LINKED but NOT INVOKED: nothing
# calls virtio_subsystem_init(). Turning it on is a separate, verified change.
VIRTIO_SOURCES = $(wildcard $(VIRTIO_DIR)/*.c)

# Network sources
NET_SOURCES = $(wildcard $(NET_DIR)/*.c)

# Network TLS sources
NET_TLS_SOURCES = $(wildcard $(NET_TLS_DIR)/*.c)

# SSH sources (new ssh2 client + server; the legacy net/ssh/*.c are dead scaffolding)
# #785: the CLIENT always builds; the SERVER only with MAYTERA_SSHD=1 (see the
# two-switches block near the top of this file). This is a hardcoded list rather
# than a $(wildcard) precisely so that adding a file to net/ssh/ cannot quietly
# put a network listener back into every kernel.
NET_SSH_SOURCES = net/ssh/ssh2.c
ifeq ($(MAYTERA_SSHD),1)
NET_SSH_SOURCES += net/ssh/ssh2_server.c
endif

# Filesystem sources
FS_SOURCES = $(wildcard $(FS_DIR)/*.c)

# #703: fs/graphfs/ has NEVER been in the build: $(wildcard fs/*.c) does not
# recurse, so these two files silently escaped every kernel build in recorded
# history. Only the content-addressed BLOB STORE is implemented; the ~96 gfs_*
# node/query/version prototypes in fs/graphfs/{node,query,version}.h have no
# implementation anywhere, so wiring this in ships blob storage, NOT a graph FS.
GRAPHFS_SOURCES = $(wildcard $(GRAPHFS_DIR)/*.c)

# Shell sources
SHELL_SOURCES = $(wildcard $(SHELL_DIR)/*.c)

# GUI sources
GUI_SOURCES = $(wildcard $(GUI_DIR)/*.c)


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

# Bluetooth stack sources (#372)
BT_SOURCES = $(wildcard $(BT_DIR)/*.c)
WIFI_SOURCES = $(wildcard $(WIFI_DIR)/*.c)

# All C sources
ALL_C_SOURCES = media/audio_decode.c media/mp3.c media/vorbis.c media/flac.c media/wav.c media/opus.c media/aac.c \
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
                $(VIRTIO_SOURCES) \
                $(NET_SOURCES) \
                $(NET_TLS_SOURCES) \
                $(NET_SSH_SOURCES) \
                $(FS_SOURCES) \
                $(GRAPHFS_SOURCES) \
                $(SHELL_SOURCES) \
                $(GUI_SOURCES) \
                $(EXEC_SOURCES) \
                $(DOS_SOURCES) \
                $(PROC_SOURCES) \
                $(IO_SOURCES) \
                $(SECURITY_SOURCES) \
                $(IPC_SOURCES) \
                $(SYNC_SOURCES) \
                $(CRYPTO_SOURCES) \
                $(VFS_SOURCES) \
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
OBJECTS = $(C_OBJECTS) $(ASM_OBJECTS)

# Header dependency files emitted by -MMD (#600). Every C object list gets one:
# C_OBJECTS covers the whole kernel plus the vendored media decoders, and
# (#703 removed DOOM_OBJECTS, the last separate relaxed-profile object list). ASM
# objects are built by nasm and have no depfiles, so they are deliberately not
# listed. `-include` (leading dash) is silent on the first build when no .d
# exists yet, and -MP keeps a deleted header from wedging make.
DEPFILES = $(C_OBJECTS:.o=.d)
-include $(DEPFILES)

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
	@mkdir -p $(OBJDIR)/$(VIRTIO_DIR)
	@mkdir -p $(OBJDIR)/$(NET_DIR)
	@mkdir -p $(OBJDIR)/$(NET_TLS_DIR)
	@mkdir -p $(OBJDIR)/$(FS_DIR)
	@mkdir -p $(OBJDIR)/$(GRAPHFS_DIR)
	@mkdir -p $(OBJDIR)/$(SHELL_DIR)
	@mkdir -p $(OBJDIR)/$(GUI_DIR)
	@mkdir -p $(OBJDIR)/$(EXEC_DIR)
	@mkdir -p $(OBJDIR)/$(DOS_DIR)
	@mkdir -p $(OBJDIR)/$(PROC_DIR)
	@mkdir -p $(OBJDIR)/$(IO_DIR)
	@mkdir -p $(OBJDIR)/$(SECURITY_DIR)
	@mkdir -p $(OBJDIR)/$(IPC_DIR)
	@mkdir -p $(OBJDIR)/$(SYNC_DIR)
	@mkdir -p $(OBJDIR)/$(CRYPTO_DIR)
	@mkdir -p $(OBJDIR)/$(VFS_DIR)
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
# --- #75 SMP context-corruption REPRODUCER -----------------------------------
# `make SCHEDRACE=1` widens the two race windows in the context-switch path with
# a bounded, clock-based delay and turns the first corrupt incoming context into
# a kpanic carrying both cores' switch history. It exists to make a fault that
# appears in 2 boots out of 5 appear in every run, quickly, with the machine
# state at the moment it happens.
#
# NEVER ship this. The delay is deliberate and large. The DETECTOR and its
# self-test are compiled in unconditionally (they are cheap and the fault is
# silent without them); only the widening and the panic are gated here, the same
# split sync/noblock.c uses.
#   make SCHEDRACE=1              - reproducer build, 40us per site
#   make SCHEDRACE=1 SCHEDRACE_US=200 - wider still
ifdef SCHEDRACE
CFLAGS += -DSCHEDRACE
ifdef SCHEDRACE_US
CFLAGS += -DSCHEDRACE_US=$(SCHEDRACE_US)
endif
endif

# #135: `make RTCBINTEST=1` compiles in drivers/rtc.c's LIVE hardware
# differential, which puts the RTC into the other data mode, writes the time
# with the OLD (shipped, broken) encoder and with the new one, prints both, and
# restores everything. OFF by default and never in the golden: it writes Status
# Register B, and doing that unasked on a real machine is how you leave someone
# with a halted clock. Same gating shape as NOBLOCKTEST.
ifeq ($(RTCBINTEST),1)
CFLAGS += -DRTC_LIVE_TEST
endif

# #740: `make DPMITEST=1` compiles in dos/dpmi_rmcs.c's DPMI 0300h harness,
# which drives the bridge with synthesised DPMI real-mode call structures
# against the REAL INT 21h service core and prints the before/after register
# files to serial. DEBUG ONLY, NEVER for a golden, for one concrete reason
# and not as boilerplate: the harness runs before any user has authenticated,
# so it arms the #708 guest filesystem slot explicitly as uid 0 (there is no
# session to inherit an identity from at that point, and guestfs_arm_session()
# correctly refuses). It also creates, writes and deletes a file at the
# filesystem root. Same top-level placement and same append-to-CFLAGS form as
# NOBLOCKTEST above, for the same two reasons (blame.md: `make CFLAGS+=...`
# REPLACES CFLAGS; #624: a flag nested inside another ifeq silently does
# nothing while appearing to be set).
ifeq ($(DPMITEST),1)
CFLAGS += -DDPMI_RMCS_SELFTEST
endif

# #192: `make FATTRACE=1` restores fat_read_file()'s four per-read narration
# lines, which are OFF by default because they printed six serial lines for
# every whole-file read in the system - config files, app launches and the
# 2-second timezone refresh alike. Failure lines are never gated. Same top-level
# placement and same append-to-CFLAGS form as NOBLOCKTEST/DPMITEST above, for
# the same two reasons (blame.md: `make CFLAGS+=...` REPLACES CFLAGS; #624: a
# flag nested inside another ifeq silently does nothing while appearing set).
ifeq ($(FATTRACE),1)
CFLAGS += -DFAT_TRACE_READS
endif

RUST_LIB         = librustkern.a
RUSTC            = rustc
RUST_PIN_VERSION = 1.97.0
RUSTFLAGS = --edition 2021 --crate-type staticlib --target x86_64-unknown-none \
            -C opt-level=2 -C panic=abort

# #113: `make RTCLKTESTFAIL=1` feeds rtclock_selftest_rs a deliberately WRONG
# expectation so the [RTCLK] boot line can be SEEN to say FAIL on an otherwise
# healthy machine. Same argument as `make NOBLOCKTEST=1`: a self-test nobody has
# watched go red is indistinguishable from one that is not wired up (#514/#665).
#
# THIS BLOCK MUST STAY BELOW THE RUSTFLAGS DEFINITION ABOVE. It was first
# written up beside NOBLOCKTEST, ~1100 lines EARLIER in this file, where the
# `+=` was silently discarded by the plain `=` here: the flag existed, the build
# accepted it, and the self-test stayed green. A build option that appears to
# work and does nothing is worse than no option at all.
ifeq ($(RTCLKTESTFAIL),1)
RUSTFLAGS += --cfg rtclk_test_fail
endif

# #86: `make KTZTESTFAIL=1` makes ktz_selftest_rs assert a WRONG offset, so the
# [KTZ] boot line can be SEEN to say FAIL. Must stay below RUSTFLAGS, as above.
ifeq ($(KTZTESTFAIL),1)
RUSTFLAGS += --cfg ktz_test_fail
endif

# #192: `make CFGTESTFAIL=1` makes cfgread_selftest_rs assert that an ABSENT
# config file deserves a WARNING (it does not; that is the whole ticket), so the
# [CFG] boot line can be SEEN to say FAIL on an otherwise healthy machine. Same
# argument as KTZTESTFAIL/RTCLKTESTFAIL: a self-test nobody has watched go red
# is indistinguishable from one that is not wired up (#514/#665). Must stay
# below the RUSTFLAGS definition, as above.
ifeq ($(CFGTESTFAIL),1)
RUSTFLAGS += --cfg cfg_test_fail
endif

# #ASUSDIAG: `make FLTTESTFAIL=1` makes fltrec_selftest_rs assert a WRONG value,
# so the [FLTREC] boot line can be SEEN to say FAIL on an otherwise healthy
# machine. Same argument as CFGTESTFAIL/KTZTESTFAIL/RTCLKTESTFAIL above: a
# self-test nobody has watched go red is indistinguishable from one that is not
# wired up (#514/#665), and this one guards the record layout of a diagnostic
# that is only ever read AFTER the machine it was recording has failed. Must stay
# below the RUSTFLAGS definition, for the reason that block spells out.
ifeq ($(FLTTESTFAIL),1)
RUSTFLAGS += --cfg fltrec_test_fail
endif

# #SB: `make BLKSTAGE_UNSAFE=1` REMOVES the single-owner claim over the block
# write staging buffer, restoring the exact pre-fix shape in which a second
# writer could overwrite a first writer's staged payload while that first
# writer was parked on msc_cmd_lock(). That is the fault that destroyed the
# ext2 primary superblock on the owner's stick (golden build 2215); see
# rustkern/blkstage.rs. It exists so `make BLKSTAGETEST=1 BLKSTAGE_UNSAFE=1`
# can be WATCHED reporting FAIL, because a guard nobody has seen go red is
# indistinguishable from one that is switched off (#514/#665).
#
# NEVER SHIP A KERNEL BUILT WITH THIS. It is a knowingly data-destroying build.
#
# Note it sets BOTH a CFLAG and a RUSTFLAG: the C side stops taking the claim
# and the Rust side stops hiding the raw buffer address. Must stay BELOW the
# RUSTFLAGS definition above, for the reason the four *TESTFAIL blocks give.
#
# It is a RUSTFLAG ONLY, on purpose: fs/blockdev.c compiles to the SAME object
# either way, so the only difference between the RED kernel and the GREEN one
# is whether blkstage_try_claim_rs() is exclusive. A knob that also changed the
# C would leave a reader wondering which of the two differences produced the
# red result.
ifeq ($(BLKSTAGE_UNSAFE),1)
RUSTFLAGS += --cfg blkstage_unsafe
endif

# #SB: `make BLKSTAGETEST=1` builds the staging-race self-test into the kernel
# and runs it just before the login stage. It performs NO device I/O (see the
# rationale in fs/blockdev.c); it exercises the staging layer, which is where
# the defect is. Off by default: it costs three worker threads and ~1 second.
ifeq ($(BLKSTAGETEST),1)
CFLAGS += -DBLKSTAGE_TEST
endif

# #SB: `make BLKSTAGETEST=1 BLKSTAGE_TEST_DEVICE=1` adds PHASE B, which drives
# REAL concurrent writes into the root filesystem so that ext2's own metadata
# updates (block bitmap, group descriptor, PRIMARY SUPERBLOCK free counts) run
# under contention. On a BLKSTAGE_UNSAFE kernel that is expected to DESTROY THE
# FILESYSTEM, which is the point: it is how the corruption was reproduced on a
# throwaway image rather than merely reasoned about.
#
# THROWAWAY IMAGES ONLY. Never point this at anything you want to keep.
ifeq ($(BLKSTAGE_TEST_DEVICE),1)
CFLAGS += -DBLKSTAGE_TEST_DEVICE
endif

# `make DIAG_SELF_DEMO=1` builds a THROWAWAY kernel that can be WATCHED doing
# the two things the boot instrumentation exists for: it holds 12s with the
# early framebuffer still owning the screen (so the stage list and the bottom
# banner can be looked at, which is impossible otherwise because the whole
# pre-console window is well under a second), and it forces the
# no-root-filesystem branch (so the paged hardware report can be seen rendering
# on a machine that mounted perfectly well). NEVER SHIP THIS: it deliberately
# refuses to reach a desktop. Same argument as the four *TESTFAIL knobs above.
ifeq ($(DIAG_SELF_DEMO),1)
CFLAGS += -DDIAG_SELF_DEMO
endif

# rustkern.rs is the crate ROOT only (panic handler + probes + `mod`
# declarations). The subsystems live in rustkern/*.rs, one module per subsystem
# (#404 / #526), so two agents working on different subsystems no longer edit
# the same file. rustc compiles the whole crate from the root in ONE
# invocation, so EVERY module is a prerequisite: touching rustkern/mono.rs must
# rebuild the archive.
RUST_MODULES = $(wildcard rustkern/*.rs)
# Data blobs pulled in with include_bytes!. They are NOT .rs files, so the
# wildcard above misses them: without this line a regenerated pwbreach.bin
# would leave the previously-linked table in the archive, and the kernel
# would enforce the OLD breached-password list while the file on disk said
# otherwise. That is the silent-staleness class this project keeps hitting.
RUST_BLOBS   = rustkern/pwbreach.bin

# #113: THE ARCHIVE MUST REBUILD WHEN RUSTFLAGS CHANGES, NOT ONLY WHEN A SOURCE
# FILE'S TIMESTAMP MOVES.
#
# THIS MECHANISM ALREADY EXISTED FOR C and was simply never extended to Rust
# when the Rust archive was added: $(FLAGS_STAMP) above ($(OBJDIR)/.cflags-stamp,
# #526) forces a full C rebuild when CFLAGS changes. librustkern.a had no
# equivalent, so it depended on FILE TIMESTAMPS alone.
#
# MEASURED CONSEQUENCE, not a theoretical one. `make RTCLKTESTFAIL=1` appends
# `--cfg rtclk_test_fail` to RUSTFLAGS, but no .rs file changed, so make
# considered librustkern.a up to date and rustc was NEVER RE-RUN. The build
# succeeded, the flag was accepted, and kernel.elf came out BYTE-IDENTICAL to
# the build without it (md5 a3401a44 both ways). That flag exists ONLY to prove
# a self-test can go red, so it would have been reported as "proven RED" on the
# strength of a compilation that never happened.
#
# Same shape as the C stamp: content is the expanded RUSTFLAGS, rewritten ONLY
# when it actually differs, so a no-op make stays a no-op. It lives under
# $(OBJDIR), which .gitignore already excludes, so it cannot dirty the working
# tree and trip build/repo-guard.sh's clean-tree requirement (the exact problem
# that got .cflags-stamp untracked at build 974).
RUST_FLAGS_STAMP = $(OBJDIR)/.rustflags-stamp

.PHONY: rustflags-stamp-force
rustflags-stamp-force:

$(RUST_FLAGS_STAMP): rustflags-stamp-force
	@mkdir -p $(OBJDIR)
	@printf '%s\n' '$(RUSTFLAGS)' > $@.tmp
	@if cmp -s $@.tmp $@; then \
	   rm -f $@.tmp; \
	 else \
	   if [ -f $@ ]; then echo ">>> RUSTFLAGS CHANGED - forcing a Rust rebuild (#113)"; fi; \
	   mv $@.tmp $@; \
	 fi

$(RUST_LIB): rustkern.rs $(RUST_MODULES) $(RUST_BLOBS) rust-toolchain.toml $(RUST_FLAGS_STAMP)
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
# The tool lives INSIDE kernel/ on purpose. `make lint` USED to reach for
# ../tools/concurrency-lint, which resolves to source/tools on the build
# container and DID NOT EXIST there, so that lint could not run where the
# kernel is actually built. A gate that silently skips is worse than no gate,
# so this one travels with the Makefile that invokes it, and #514 moved the
# concurrency lint into kernel/tools/ for exactly the same reason.
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

# --- Syscall dispatch gate (#565 regression guard) -------------------------
# FAILS THE BUILD if a syscall listed in syscall-dispatch.manifest loses its
# `case SYS_*:` in proc/syscall.c while its number is still #defined in
# proc/syscall.h. A dropped case BUILDS CLEAN (the number, the header define,
# and the kernel impl all remain; the call just falls through the switch), so
# the compiler is no backstop. This is how 68199dd (#565 theme loader) silently
# dropped SYS_APP_VERIFY_SIG and defeated the #563 app/kernel signing-key split.
# Same rationale and placement as rust-symbol-gate above: the tool travels
# INSIDE kernel/ so it can run where the kernel is actually built.
SYSCALL_GATE     = tools/syscall-dispatch-gate/syscall-dispatch-gate
SYSCALL_HEADER   = proc/syscall.h
SYSCALL_DISPATCH = proc/syscall.c
SYSCALL_MANIFEST = syscall-dispatch.manifest

.PHONY: syscall-dispatch-gate
syscall-dispatch-gate:
	@if [ ! -f $(SYSCALL_GATE) ]; then \
	   echo "ERROR: $(SYSCALL_GATE) is missing. The syscall dispatch gate cannot"; \
	   echo "       run, and a gate that skips silently is how a case disappears."; \
	   exit 1; \
	 fi
	@python3 $(SYSCALL_GATE) --header $(SYSCALL_HEADER) --dispatch $(SYSCALL_DISPATCH) --manifest $(SYSCALL_MANIFEST)

# Refresh the manifest from the current cases (run when you ADD a syscall case).
.PHONY: syscall-dispatch-update
syscall-dispatch-update:
	@python3 $(SYSCALL_GATE) --header $(SYSCALL_HEADER) --dispatch $(SYSCALL_DISPATCH) --manifest $(SYSCALL_MANIFEST) --update

# --- Syscall user-pointer lint (#565, enforcing #500) ----------------------
# FAILS THE BUILD if a syscall whose dispatcher case casts an arg to a USER
# POINTER has no validation: neither a descriptor in rustkern/argtab.rs nor an
# allowlist entry in syscall-ptr.manifest. A missing descriptor BUILDS CLEAN
# (syscall_validate_args() returns 0 for an undescribed syscall, so the pointer
# reaches the handler unchecked) - the exact Ring-3 -> kernel arbitrary R/W hole
# #500 closed with 114 descriptors. #500 was a one-time manual audit; this is
# the gate that keeps it closed as syscalls are added. Same rationale and
# placement as syscall-dispatch-gate above: the tool travels INSIDE kernel/ so
# it runs where the kernel is actually built.
SYSCALL_PTRLINT  = tools/syscall-ptr-lint/syscall-ptr-lint
SYSCALL_ARGTAB   = rustkern/argtab.rs
SYSCALL_PTRMAN   = syscall-ptr.manifest

.PHONY: syscall-ptr-lint
syscall-ptr-lint:
	@if [ ! -f $(SYSCALL_PTRLINT) ]; then \
	   echo "ERROR: $(SYSCALL_PTRLINT) is missing. The syscall pointer lint"; \
	   echo "       cannot run, and a gate that skips silently is how an"; \
	   echo "       unvalidated Ring-3 pointer syscall ships (#500/#565)."; \
	   exit 1; \
	 fi
	@python3 $(SYSCALL_PTRLINT) --header $(SYSCALL_HEADER) --dispatch $(SYSCALL_DISPATCH) --argtab $(SYSCALL_ARGTAB) --manifest $(SYSCALL_PTRMAN)

# Prove the gate is trustworthy: RED on a broken fixture, GREEN on the real tree.
.PHONY: syscall-ptr-lint-selftest
syscall-ptr-lint-selftest:
	@python3 $(SYSCALL_PTRLINT) --header $(SYSCALL_HEADER) --dispatch $(SYSCALL_DISPATCH) --argtab $(SYSCALL_ARGTAB) --manifest $(SYSCALL_PTRMAN) --self-test

# --- Syscall copy_*_user adoption lint (#566, enforcing #509) ---------------
# FAILS THE BUILD if a syscall handler in proc/syscall.c DIRECTLY DEREFERENCES a
# user pointer (`*p`, `p->f`, `p[i]`, or a bare memcpy/memset on it) instead of
# routing that access through the #509 TOCTOU-safe copy_*_user primitives -
# unless it is on the checked-in manifest (adopted needs no entry; already-safe
# goes [EXEMPT]; honestly-deferred goes [KNOWN_GAP], a loud debt ledger printed
# every build). syscall-ptr-lint (#565) proves each pointer syscall is VALIDATED
# AT ENTRY (the check half of the TOCTOU pair); THIS lint proves the USE half is
# atomic too - that the handler does not then raw-deref the pointer it just
# validated. A raw deref after an entry check BUILDS CLEAN: the compiler is happy
# and only a racing sibling thread that remaps the page in between reveals it.
# Same rationale and placement as syscall-ptr-lint above: the tool travels
# INSIDE kernel/ so it runs where the kernel is actually built.
COPYUSER_LINT    = tools/copy-user-lint/copy-user-lint
COPYUSER_MAN     = syscall-copyuser.manifest
# #746b: THE SYSCALL LAYER IS TWO FILES NOW. The legacy fd layer moved to
# proc/fdlayer.c, and this lint only ever analysed functions defined in the file
# given as --dispatch. Without naming fdlayer.c here, SYS_OPEN, SYS_READ,
# SYS_WRITE and SYS_READDIR leave the analysed set: the build stays green, the
# tracked SYS_READ TOCTOU debt DISAPPEARS from the output, and the lint prints a
# note inviting you to delete the manifest entry as stale. That was measured
# before the split, not guessed. Add every new file that defines syscall
# handlers here; a path that does not exist is a hard failure, by design.
COPYUSER_HANDLERS = proc/fdlayer.c

.PHONY: copy-user-lint
copy-user-lint:
	@if [ ! -f $(COPYUSER_LINT) ]; then \
	   echo "ERROR: $(COPYUSER_LINT) is missing. The copy_*_user adoption lint"; \
	   echo "       cannot run, and a gate that skips silently is how a"; \
	   echo "       TOCTOU-unsafe direct user-pointer deref ships (#509/#566)."; \
	   exit 1; \
	 fi
	@python3 $(COPYUSER_LINT) --header $(SYSCALL_HEADER) --dispatch $(SYSCALL_DISPATCH) $(addprefix --handlers ,$(COPYUSER_HANDLERS)) --manifest $(COPYUSER_MAN)

# Prove the gate is trustworthy: RED on a direct-deref fixture, GREEN on the real tree.
.PHONY: copy-user-lint-selftest
copy-user-lint-selftest:
	@python3 $(COPYUSER_LINT) --header $(SYSCALL_HEADER) --dispatch $(SYSCALL_DISPATCH) $(addprefix --handlers ,$(COPYUSER_HANDLERS)) --manifest $(COPYUSER_MAN) --self-test

# --- SMAP user-access lint (#645, enforcing the CR4.SMAP contract) ----------
# FAILS THE BUILD on (A) any regression in the SMAP mechanism itself and (B) any
# NEW Ring-0 access to user memory that is neither routed through copy_*_user
# nor bracketed by uaccess_begin()/uaccess_end() nor on the manifest.
#
# It closes the two documented blind spots of copy-user-lint above, BOTH of
# which are load-bearing once SMAP is on. That lint reasons about the syscall
# layer ONLY (proc/syscall.c plus the files named in COPYUSER_HANDLERS, #746b)
# and traces ONE call level; measured against this tree, 43 of the 122
# copy_*_user call sites are in other files, every heavy unwrapped site (ELF
# loader, argv builder, signal frames) is outside syscall.c, and sc_copy_str()
# reads a user string with NO validation two levels down from three dispatcher
# cases. This lint's watch set is DERIVED (any file that validates or copies a
# user pointer joins automatically) and it follows same-file calls to depth 3.
#
# SMAP_DEFAULT is computed from CFLAGS, not hand-set, so it cannot drift from
# what is actually being compiled: removing -DCONFIG_NO_SMAP flips it to `on`,
# and the lint then FAILS while the manifest still lists a KNOWN_GAP. That
# interlock is the whole point - it makes "do not arm SMAP yet" a build failure
# instead of a promise someone has to remember.
SMAP_LINT    = tools/smap-uaccess-lint/smap-uaccess-lint
SMAP_MAN     = smap-uaccess.manifest
SMAP_DEFAULT = $(if $(findstring -DCONFIG_NO_SMAP,$(CFLAGS)),off,on)

.PHONY: smap-uaccess-lint
smap-uaccess-lint:
	@if [ ! -f $(SMAP_LINT) ]; then \
	   echo "ERROR: $(SMAP_LINT) is missing. The SMAP user-access lint cannot"; \
	   echo "       run, and an unwrapped Ring-0 access to user memory is a #PF"; \
	   echo "       the moment CR4.SMAP is set (#645)."; \
	   exit 1; \
	 fi
	@python3 $(SMAP_LINT) --root . --manifest $(SMAP_MAN) --smap-default $(SMAP_DEFAULT)

# Prove the gate is trustworthy: RED on each synthetic violation, GREEN on
# correct code. A gate nobody has watched go red is not a gate.
.PHONY: smap-uaccess-lint-selftest
smap-uaccess-lint-selftest:
	@python3 $(SMAP_LINT) --self-test

# Re-anchor the manifest's LINE NUMBERS after an unrelated edit shifted them
# (#695). Any change that inserts or deletes lines in a listed file moves every
# entry below it, and rule B1 then fails the build with a wall of errors that
# have nothing to do with the change. That happened at #692 and again at #695;
# the fix was written ad hoc and thrown away both times, so it is checked in.
# It maps old line -> new line by DIFFING against the committed baseline, which
# is exact, and then VERIFIES the substring is really there. It does not search
# for the substring as its primary locator: at #695 the shift was ~130 lines and
# a 24-character key like "process_t *p = proc_current();" matched 28 places, so
# nearest-occurrence collapsed three distinct sites onto one line. Run
# `--check` first to see the blast radius; an entry it cannot map is REPORTED
# and never guessed at.
SMAP_REANCHOR = tools/smap-uaccess-lint/reanchor-manifest
.PHONY: smap-uaccess-reanchor smap-uaccess-reanchor-check
smap-uaccess-reanchor:
	@python3 $(SMAP_REANCHOR) --root . --manifest $(SMAP_MAN)
smap-uaccess-reanchor-check:
	@python3 $(SMAP_REANCHOR) --root . --manifest $(SMAP_MAN) --check

# --- Concurrency lint (#426, made real at #514) -----------------------------
# FAILS THE BUILD on any NEW hand-rolled busy-wait / spin / poll loop: the bug
# class behind the freeze/hang tickets #211 #212 #230 #231 #347 #381 #419 #420,
# and the single most-repeated rule in CLAUDE.md ("ALL sleeping/waiting goes
# through the kernel wait-queue or futex; NEVER hand-roll a busy-wait").
#
# #514: this lint EXISTED and was DOCUMENTED as a build guardrail for weeks
# while being incapable of running in a build. It lived at <repo>/tools/ and
# was invoked as `../tools/concurrency-lint/...`, but build-golden.sh ships
# ONLY `git archive <commit> kernel` to the kernel build container, so on the
# machine where the kernel is actually built that path did not exist and
# `make lint` died with "No such file or directory". It was also never a
# prerequisite of anything, so a normal `make` never called it. Two independent
# reasons it could not fail. The tool now travels INSIDE kernel/ and is an
# order-only prerequisite of the link, exactly like rust-symbol-gate,
# syscall-dispatch-gate, syscall-ptr-lint and copy-user-lint.
#
# Scoped to `--dirs kernel` on purpose: the build container only has kernel/,
# so a gate that also scanned userland would give a DIFFERENT verdict depending
# on where it ran. Use `make lint-all` from a full checkout for kernel+userland.
# --- Syscall number lint (#745) ---------------------------------------------
# FAILS THE BUILD when two SYS_* names share a number, when SYS_MAX is not
# strictly past the top, when kernel and libc disagree on a shared name, or when
# argtab.rs holds two descriptors for one syscall.
#
# It exists because on 2026-08-10 two agent branches each allocated 373 and git
# merged BOTH with no conflict, because the #defines sat at different line
# numbers. It compiled. One of the two features would simply never have run.
# Nothing in the build would have caught it; it was found by hand.
#
# `make syscall-number-lint-selftest` PROVES it goes red on each of the four
# rules and green on the real tree. A gate only ever seen green is not evidence.
SYSNUM_LINT = tools/syscall-number-lint/syscall-number-lint

.PHONY: syscall-number-lint
syscall-number-lint:
	@bash $(SYSNUM_LINT) ..

.PHONY: syscall-number-lint-selftest
syscall-number-lint-selftest:
	@bash tools/syscall-number-lint/selftest

# --- SYS_MPROTECT argument validator self-test (#404) ------------------------
# NOT wired into the link. It needs a HOST rustc target (it runs the validator
# as an ordinary program), whereas the kernel build targets x86_64-unknown-none,
# so making it a build prerequisite would break the kernel build container.
# Run it by hand when touching rustkern/mprotect.rs.
#
#   make mprotect-validate-selftest     # the validator as shipped
#   make mprotect-validate-selftest-red # PROVE it goes red on a removed guard
mprotect-validate-selftest:
	@bash tools/mprotect-validate-test/selftest

mprotect-validate-selftest-red:
	@bash tools/mprotect-validate-test/selftest --prove-red

# --- Duplicate-constant lint (#222) -----------------------------------------
# FAILS THE BUILD when a constant defined in a HEADER is also defined, with a
# DIFFERENT value, somewhere else.
#
# This is the most expensive recurring fault in the project and it is not a
# hard bug, it is a value written down twice: FIVE version.h files each with a
# different MAYTERA_VERSION_STRING behind one include guard (#106); five copies
# of the #145 fix where the fifth still held the baseline (#178); a SIXTH
# CPU-count constant read out of bounds when five were thought to be all of
# them (#166); two branches allocating syscall 373 and git merging both without
# a conflict (#745). Someone fixes one copy, review agrees, and the shipped
# binary reads the other.
#
# Scoped to `--dirs kernel` for the same reason as concurrency-lint: the kernel
# build container only receives `git archive <commit> kernel`, so a gate that
# also scanned userland would give a DIFFERENT verdict depending on where it
# ran. build/repo-guard.sh runs it --strict over the WHOLE tree at the release
# chokepoint, which is where the kernel-vs-userland and C-vs-Rust divergences
# (SYS_* headers, struct termios, the #745 fifth copy) can actually be seen.
#
# The tool travels INSIDE kernel/ and is an order-only prerequisite of the link,
# exactly like the gates above, because #514 is what happens otherwise: a lint
# living at <repo>/tools that build-golden.sh never shipped, and that nothing
# depended on, sat here for months looking exactly like a working guardrail.
#
# `make dup-const-lint-selftest` PROVES it: RED on a header contradicted
# elsewhere, on a Rust const disagreeing with a C #define, and on a declared
# value contract drifting; GREEN on 0x2C vs 44, on #ifdef branches in one file,
# and on two .c files whose private constants happen to collide.
DUPCONST_LINT = tools/dup-const-lint/dup-const-lint

.PHONY: dup-const-lint
dup-const-lint:
	@if [ ! -f $(DUPCONST_LINT) ]; then \
	   echo "ERROR: $(DUPCONST_LINT) is missing. The duplicate-constant lint"; \
	   echo "       cannot run, and a gate that skips silently is how the next"; \
	   echo "       second copy of a constant ships (#222/#514)."; \
	   exit 1; \
	 fi
	@python3 $(DUPCONST_LINT) .. --dirs kernel

.PHONY: dup-const-lint-selftest
dup-const-lint-selftest:
	@python3 $(DUPCONST_LINT) --self-test

CONC_LINT = tools/concurrency-lint/concurrency-lint

.PHONY: concurrency-lint
concurrency-lint:
	@if [ ! -f $(CONC_LINT) ]; then \
	   echo "ERROR: $(CONC_LINT) is missing. The concurrency lint cannot run,"; \
	   echo "       and a gate that skips silently is how the next hand-rolled"; \
	   echo "       spin loop ships (#426/#514)."; \
	   exit 1; \
	 fi
	@python3 $(CONC_LINT) --root .. --dirs kernel

# Prove the gate is trustworthy: RED on a synthetic spin of every rule, GREEN
# on correct wait_event code. A gate nobody has watched go red is not a gate.
.PHONY: concurrency-lint-selftest
concurrency-lint-selftest:
	@python3 $(CONC_LINT) --self-test

# --- Private-extern gate (#742, closing #693's one hole) -------------------
# FAILS THE BUILD if a .c file re-declares a guarded persistence symbol with
# its own `extern` instead of including the owning header. That pattern opts
# the whole file out of MUST_CHECK (warn_unused_result), permanently and
# silently, AND binds to a signature the author invented: void vs int, and a
# wrong pointer type or width, are all ABI-compatible on x86-64 SysV and link
# with no diagnostic. #693 measured the cost: its first attribute build found
# 41 discarded results when the truth was 57, the missing 16 hidden behind
# exactly this. #742 changed bootlog_write() from void to int with 46 such
# declarations live, two of them also non-variadic.
#
# The compiler cannot defend its own attribute here, so this gate does. Same
# rationale and placement as the gates above: the tool travels INSIDE kernel/
# so it runs where the kernel is actually built.
PERSIST_EXTGATE = tools/persist-extern-gate/persist-extern-gate
PERSIST_EXTMAN  = persist-extern.manifest

.PHONY: persist-extern-gate
persist-extern-gate:
	@if [ ! -f $(PERSIST_EXTGATE) ]; then \
	   echo "ERROR: $(PERSIST_EXTGATE) is missing. The private-extern gate"; \
	   echo "       cannot run, and a gate that skips silently is how a whole"; \
	   echo "       file opts out of MUST_CHECK without anybody noticing."; \
	   exit 1; \
	 fi
	@python3 $(PERSIST_EXTGATE) --root . --manifest $(PERSIST_EXTMAN)

# Prove it: RED on a planted private extern of EVERY guarded symbol, in both
# the file-scope and the inside-a-function form, and GREEN on the real tree.
.PHONY: persist-extern-gate-selftest
persist-extern-gate-selftest:
	@python3 $(PERSIST_EXTGATE) --root . --manifest $(PERSIST_EXTMAN) --self-test

# #DIAGLOG: FAIL THE BUILD when a boot-path diagnostic can only be read over a
# serial port. Serial is silent in GUI mode and the laptop and iMac targets have
# no serial port at all, so a kprintf-only diagnostic produces NOTHING on the
# machines it exists for. Measured on golden 2219: the one line that was the
# only evidence the block write-staging fix works on real hardware, plus its
# CORRUPTION IN PROGRESS alarm, plus 20 more boot-path report/selftest
# functions, were all in that shape. The durable sink (bootlog_write, and
# bootlog_fault_write for code on the storage path) already existed; the choice
# of sink was simply left to each author. Same placement rationale as the gates
# above: the tool travels INSIDE kernel/ so it runs where the kernel is built.
DIAGLOG_GATE  = tools/diaglog-gate/diaglog-gate
DIAGLOG_ALLOW = tools/diaglog-gate/allowlist.txt

.PHONY: diaglog-gate
diaglog-gate:
	@if [ ! -f $(DIAGLOG_GATE) ]; then \
	   echo "ERROR: $(DIAGLOG_GATE) is missing. A gate that skips silently is"; \
	   echo "       how a diagnostic ships that nobody can read on the machine"; \
	   echo "       it was written for."; \
	   exit 1; \
	 fi
	@python3 $(DIAGLOG_GATE) --root . --allowlist $(DIAGLOG_ALLOW)

# Prove it: RED on a synthetic serial-only boot diagnostic, GREEN when the same
# fixture uses bootlog_write(), plus the allowlist suppressing and a stale
# allowlist entry failing. The fixture is self-contained, so the proof does not
# depend on the state of any real kernel file.
.PHONY: diaglog-gate-selftest
diaglog-gate-selftest:
	@python3 $(DIAGLOG_GATE) --self-test

# ---------------------------------------------------------------------------
# #PERMSKIP: skipgate - FAIL THE BUILD when a self-test can decline to run and
# says so in a line that reads like ordinary boot noise.
#
# fs/perms.c perms_selftest() printed "[PERMS-SELFTEST] SKIP traversal vectors
# (/HOME/ADMIN not 1000:0750)" on every boot of every CORRECTLY PROVISIONED
# machine, because the first-boot wizard lets the owner name the account and
# the owner is not called "admin". MEASURED on golden 2234: the shipped
# PERMS.DB holds three entries and PASSWD is empty, so it could not run before
# provisioning either. The traversal half of the permission model had probably
# never been exercised anywhere. Same placement rationale as the gates above:
# the tool travels INSIDE kernel/ so it runs where the kernel is built.
SKIP_GATE  = tools/skipgate/skipgate
SKIP_ALLOW = tools/skipgate/allowlist.txt

.PHONY: skipgate
skipgate:
	@if [ ! -f $(SKIP_GATE) ]; then \
	   echo "ERROR: $(SKIP_GATE) is missing. A gate that skips silently is how"; \
	   echo "       a self-test ships that stopped testing years ago."; \
	   exit 1; \
	 fi
	@python3 $(SKIP_GATE) --root . --allowlist $(SKIP_ALLOW)

# Prove it: RED on a self-test printing its own SKIP, GREEN when the same
# fixture registers through selftest_notrun(), RED on UNDERCOUNT (one branch
# registered, a second still printing its own - the case that caught this
# gate's FIRST rule going green on a real violation), plus the allowlist
# suppressing and a stale allowlist entry failing.
.PHONY: skipgate-selftest
skipgate-selftest:
	@python3 $(SKIP_GATE) --self-test

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
$(TARGET): $(OBJECTS) $(RUST_LIB) | increment_build dirs rust-symbol-gate syscall-dispatch-gate syscall-ptr-lint copy-user-lint smap-uaccess-lint concurrency-lint persist-extern-gate syscall-number-lint dup-const-lint diaglog-gate skipgate
	@echo "Linking $(DEBUG_TARGET)..."
	$(LD) $(LDFLAGS) -o $(DEBUG_TARGET) $(OBJECTS) $(RUST_FORCE) $(RUST_LIB)
	@# #670: the SHIPPED kernel is the stripped copy. Link once, strip into
	@# $(TARGET), so build-golden.sh and invariant-gate.sh keep consuming
	@# "kernel.elf" unchanged and the #618 byte-for-byte rebuild gate still
	@# compares like with like (objcopy is deterministic for a given input and
	@# binutils version, and both sides of that gate build in the same CT).
	@# Debug with: addr2line -e $(DEBUG_TARGET) <address from serial>
	$(OBJCOPY) --strip-debug --strip-unneeded $(DEBUG_TARGET) $@
	@echo "Kernel built successfully!"
	@ls -lh $(TARGET) $(DEBUG_TARGET)
	@echo ""
	@echo "Kernel sections (shipped):"
	@size $(TARGET)
	@# The gate, not the prose (#514): fail the build rather than ship symbols.
	@if readelf -SW $(TARGET) | grep -qE '\.(symtab|debug_)'; then \
	   echo ">>> #670 STRIP GATE FAILED: $(TARGET) still carries .symtab/.debug_*"; \
	   rm -f $(TARGET); exit 1; \
	 fi
	@echo "#670 strip gate: shipped $(TARGET) has no .symtab / .debug_* section"


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
# NOTE: all the derived profiles (TTF_CFLAGS, LIBMAD_CFLAGS, TREMOR_CFLAGS, ...)
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
	rm -rf $(OBJDIR) $(TARGET) $(DEBUG_TARGET) $(RUST_LIB)
	@# depfiles (#600) live inside $(OBJDIR) so the rm -rf above already
	@# takes them; belt and braces in case OBJDIR is ever flattened.
	rm -f $(DEPFILES)
	@echo "Clean complete."

# Show info
.PHONY: info
info:
	@echo "C Sources: $(ALL_C_SOURCES)"
	@echo "ASM Sources: $(ALL_ASM_SOURCES)"
	@echo "Objects: $(OBJECTS)"

# Concurrency lint over kernel/ AND userland/ (#426). Needs a FULL checkout;
# the kernel build container only has kernel/, which is why the build gate
# above is kernel-scoped. `lint` is kept as the historical alias.
.PHONY: lint lint-all
lint lint-all:
	@python3 $(CONC_LINT) --root .. --dirs kernel,userland

# Re-baseline the concurrency-lint allowlist to the current tree (run only
# when knowingly accepting reviewed legacy sites; commit the allowlist).
.PHONY: lint-baseline
lint-baseline:
	@python3 $(CONC_LINT) --root .. --update-allowlist

# Full categorized backlog of every hit (allowlisted or not).
.PHONY: lint-report
lint-report:
	@python3 $(CONC_LINT) --root .. --report

# Install to UEFI boot directory
.PHONY: install
install: $(TARGET)
	@echo "Installing kernel to ../uefi/boot/"
	@mkdir -p ../uefi/boot
	cp $(TARGET) ../uefi/boot/

# Increment build number before building
increment_build:
	@./increment_build.sh

# #742: the four version-dependent objects are declared to DEPEND on version.h
# instead of being `rm`-ed by increment_build's recipe.
#
# The rm was unsound under -j by construction, and no amount of order-only
# prerequisite fixes it. Order-only says "update increment_build before you
# BUILD this object". If make decides an object does NOT need building, it never
# builds it, so the rm lands AFTER that decision and the link then dies with
# `ld: cannot find obj/main.o`. Intermittent, indistinguishable from a corrupt
# tree, and serial `make` always hides it. Two order-only fixes in a row failed
# to close it, which is the tell that the mechanism was wrong rather than
# mis-wired: a recipe that deletes another target's inputs is racing make's own
# scheduler.
#
# A declared prerequisite is what make is FOR. build-golden.sh seds version.h
# before invoking make, so these four rebuild exactly when the build number
# changes and never otherwise. main.c is listed even though it does not
# currently #include version.h, preserving the rm's intent rather than quietly
# narrowing it.
obj/main.o obj/video/graphics.o obj/proc/syscall.o obj/gui/desktop.o: version.h

# #742 (incidental, found while building): `all: increment_build dirs $(TARGET)`
# does NOT order its prerequisites under -j. GNU make runs prerequisites of the
# SAME target in parallel, so with `make -j8` the link could start while
# increment_build was still `rm`-ing the four version-dependent objects, and the
# build died with `ld: cannot find obj/main.o`. It is intermittent, it looks
# like a corrupt tree rather than a race, and `make` alone (serial) always
# hides it.
#
# Order-only on the OBJECTS is necessary and NOT sufficient, which cost a second
# failing build to find: when every object is already up to date, $(TARGET)
# never waits for anything, so make happily runs increment_build's `rm` AT THE
# SAME TIME as the link, and ld dies on a file that existed when it started. So
# $(TARGET) carries the order-only dependency too (see its rule below). Both are
# needed: the OBJECTS one covers the compile, the TARGET one covers the link.
# Being order-only, neither marks anything out of date.
$(OBJECTS): | increment_build dirs
