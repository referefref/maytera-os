#!/bin/bash
# mkgen.sh - assemble the dosring3 BUILD TREE (generated; not committed).
#
# THE WHOLE POINT: the Ring-3 DOS host compiles the KERNEL'S OWN DOS SOURCES,
# byte-identical, rather than a reimplementation. kernel/dos/*.c use quoted
# relative includes ("../mm/heap.h"), which always resolve against the
# INCLUDING FILE'S directory and therefore cannot be redirected with -I. The
# only way to substitute a Ring-3 implementation of a kernel facility is to
# place the sources in a tree whose SIBLING DIRECTORIES are ours. That is what
# this builds.
#
# Copying rather than editing kernel/dos in place is deliberate and load-
# bearing on three counts:
#   1. kernel/dos stays untouched, so the in-kernel path remains the default
#      and this port can never be blamed for a regression in it;
#   2. an upstream change to the interpreter flows into the Ring-3 host on the
#      next build instead of needing a merge (no long-lived divergence);
#   3. there is exactly ONE implementation of DOS semantics in the tree, so the
#      two paths cannot drift into guest-visible disagreement.
set -euo pipefail
K=../../../kernel
G=gen
rm -rf $G
mkdir -p $G/dos $G/exec $G/mm $G/fs $G/cpu $G/drivers $G/sync $G/gui $G/video $G/proc $G/rustkern $G/obj

# --- kernel DOS/exec sources, VERBATIM (the port's whole value) -------------
cp $K/dos/*.c $K/dos/*.h             $G/dos/

cp $K/exec/x86_16.c $K/exec/x86_16.h $G/exec/
cp $K/exec/softfpu.c $K/exec/softfpu.h $G/exec/
for f in go32 le x86_32; do
  for e in c h; do [ -f $K/exec/$f.$e ] && cp $K/exec/$f.$e $G/exec/; done
done
# types.h and string.h are taken verbatim (the DOS sources are compiled against
# the kernel's type world; see shim/kbridge.h for the wall that keeps it apart
# from libc's).
#
# string.c is DELIBERATELY NOT COMPILED, though the header is still the kernel's.
# It was originally included so the guest would get byte-identical string and
# printf semantics, but it collides with libc on eight symbols that libc's own
# stdio.o/stdlib.o also define (vsnprintf, snprintf, atoi, atol, strtol,
# strtoul, strtoll, strtoull), and those objects are pulled in unavoidably by
# printf and malloc. MEASURED: with string.c dropped the link is CLEAN, i.e.
# libc already provides every string function the DOS sources actually call, and
# none of the kernel-only helpers (kvformat, vsnprintf_dropped, itoa/ltoa/ultoa)
# has a caller in them.
#
# The honest cost, stated rather than glossed: kernel and libc snprintf could
# format some edge case differently. That affects LOG LINES ONLY - guest console
# output goes through dos_tty_putc(), not through printf - so it cannot change
# what a guest computes or displays. Resolving the collision by renaming symbols
# with -D would have kept both, at the price of a build-flag substitution that
# is invisible at the call site; that trade was not worth it for log formatting.
cp $K/types.h $K/string.h $G/
# Real CP437 glyph data and the real set-1 scancode->char table, rather than a
# shim's approximation of either: a DOS guest's text output IS these glyphs, and
# a re-typed key table is how the Ring-0 and Ring-3 paths would come to disagree
# about what the user pressed.
cp $K/video/font.c      $G/video/font.c
cp $K/drivers/keymap.c  $G/drivers/keymap.c
# The SHARED streaming resampler, verbatim, from the file the kernel itself
# links. It was split out of drivers/audio.c precisely so this line can exist:
# audio.c is a driver (HDA, AC97, SB16, the mixer) and cannot be compiled for
# Ring 3, while these two functions are pure fixed-point arithmetic over
# caller-owned buffers. Copying the FILE rather than shimming the functions is
# what stops the two DOS paths from resampling a guest differently.
cp $K/drivers/audio_resample.c $G/drivers/audio_resample.c

# --- kernel headers, VERBATIM, by TRANSITIVE INCLUDE CLOSURE ---------------
# MEASURED: of the kernel headers the DOS sources include, only sync/spinlock.h
# contains inline asm, and all 18 occurrences are in the atomic_*/barrier static
# inlines (lock cmpxchg, xadd, mfence) - every one a legal Ring-3 instruction.
# So headers are copied VERBATIM rather than hand-written as shims. That
# preserves EXACT struct layouts (the DOS sources index into fat_file_t,
# hotplug_raw_t, wait_queue_head_t), which a hand-written shim header is free to
# get subtly and silently wrong. Only the .c IMPLEMENTATIONS are Ring-0.
#
# The closure is COMPUTED, not a hand-maintained list: a hand list goes stale
# the moment someone adds an #include upstream, and the failure mode is a
# confusing "No such file" three headers deep. This walks the graph instead, so
# the build tracks the kernel automatically. serial.h is excluded because it is
# the one facility that genuinely cannot exist in Ring 3 (a UART we do not own);
# it gets a real shim, copied after this step.
python3 - "$K" "$G" <<'PYEOF'
import os, re, sys, shutil
K, G = sys.argv[1], sys.argv[2]
inc_re = re.compile(rb'^\s*#\s*include\s+"([^"]+)"', re.M)
SHIMMED = {'serial.h'}          # provided by shim/inc, never copied from kernel
# Seeds: the sources we compile, addressed by their path INSIDE the kernel tree.
seeds = [os.path.join('dos', f) for f in os.listdir(os.path.join(K,'dos'))]
seeds += ['exec/x86_16.c','exec/x86_16.h','exec/softfpu.c','exec/softfpu.h',
          'string.c','string.h','types.h',
          'video/font.c','video/font.h','drivers/keymap.c','drivers/keymap.h']
for f in ('go32','le','x86_32'):
    for e in ('c','h'):
        if os.path.exists(os.path.join(K,'exec',f+'.'+e)): seeds.append('exec/%s.%s'%(f,e))
seen, queue, copied = set(), list(seeds), 0
while queue:
    rel = os.path.normpath(queue.pop())
    if rel in seen: continue
    seen.add(rel)
    src = os.path.join(K, rel)
    if not os.path.isfile(src): continue
    if os.path.basename(rel) in SHIMMED: continue
    dst = os.path.join(G, rel)
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    if not os.path.exists(dst):
        shutil.copy2(src, dst); copied += 1
    d = os.path.dirname(rel)
    for m in inc_re.findall(open(src,'rb').read()):
        inc = m.decode()
        # Quoted includes resolve against the including file's directory first,
        # then (GCC) the -I path, which for us is the gen root. Try both, in
        # that order, exactly as the compiler will.
        for cand in (os.path.join(d, inc), inc):
            if os.path.isfile(os.path.join(K, os.path.normpath(cand))):
                queue.append(cand); break
print("  include closure: %d files copied" % copied)
PYEOF

# --- Rust DOS modules, VERBATIM; a userland crate root selects the subset ---
for f in $K/rustkern/dos*.rs $K/rustkern/go32.rs $K/rustkern/dpmi.rs \
         $K/rustkern/dpmi_rmcs.rs $K/rustkern/x86_32.rs $K/rustkern/le.rs \
         $K/rustkern/vbe.rs $K/rustkern/cga.rs $K/rustkern/common.rs \
         $K/rustkern/drvmap.rs $K/rustkern/opl2.rs \
         $K/rustkern/cfgread.rs $K/rustkern/iso9660.rs $K/rustkern/imgra.rs \
         $K/rustkern/isomemo.rs $K/rustkern/blkhist.rs $K/rustkern/mono.rs \
         $K/rustkern/modex.rs $K/rustkern/ktime.rs $K/rustkern/guestfs.rs \
         $K/rustkern/x87.rs $K/rustkern/spawnid.rs $K/rustkern/usbvol.rs; do
  [ -f "$f" ] && cp "$f" $G/rustkern/
done

# --- the ONE shim header: Ring-3 logging in place of the UART --------------
cp shim/inc/serial.h $G/serial.h

# --- #VOLAPI: the ONE deliberate omission, applied LAST --------------------
# LAST, and that is not cosmetic: the python include-closure step above takes
# every file in kernel/dos as a SEED and re-copies anything missing, so an rm
# placed next to the cp is silently undone. MEASURED: the first attempt put it
# there and the link failed with 'multiple definition of diskimg_mscdex'. The
# link is the check that caught it, which is the argument for having one.
# These sources exist to turn RAW BLOCK READS into a mounted volume:
# usbvol.c walks the boot device's unpartitioned tail, imgfile.c's
# IMGF_KIND_BLKDEV reads device ranges, and diskimg.c parses and owns the
# resulting images. All three bottom out in blk_read(), which is ABSENT in
# Ring 3 and MUST stay absent - exposing it would hand the DOS host the whole
# disk and destroy the boundary this port exists to create.
#
# Compiled here they were not merely useless, they were ACTIVELY WRONG: they
# gave the Ring-3 process a second, permanently EMPTY mount table, and the DOS
# layer believed it. That is what Red Alert's "PLEASE INSERT A RED ALERT CD"
# actually was, after 16,759,601,960 instructions.
#
# shim/volshim.c provides the five metadata symbols the rest of the DOS sources
# import from diskimg.c, answered by the KERNEL's live mount table through
# SYS_DISKIMG VOLINFO. File CONTENT is unaffected and needs nothing: it already
# flows through fat_open() -> kb_open() -> the kernel's own open(), where
# fs/fat.c's #196 image redirect serves the disc under this process's real
# credentials.
#
# THE EXCLUSION IS VERIFIED, NOT ASSUMED. The five symbols were MEASURED by
# grepping every OTHER dos/*.c and exec/*.c for diskimg_/imgf_/usbvol_/isomemo_
# references; the link is the second check, and it fails loudly rather than
# silently if this list is ever wrong.
rm -f $G/dos/diskimg.c $G/dos/imgfile.c $G/dos/usbvol.c \
      $G/dos/cdbench.c $G/dos/cdprobe.c $G/dos/diskimg_test.c

# --- #fmbridge: THE SECOND deliberate omission, and the same trap applies -----
# dos/dosfmq.c owns the ONE FM event queue: the struct, its spinlock, the two
# pid latches, dos_fm_drain() (the SYS_DOS_FM_EVENTS backend) and the
# SYS_DOS_FM_HOST demultiplexer. Compiled here it would give this process a
# SECOND queue in its own address space - which is EXACTLY the defect being
# fixed. Before the split, dosexec.c carried `static dos_fm_queue_t g_dos_fmq`,
# so the Ring-3 host queued the guest's OPL2 register writes faithfully into a
# buffer with no consumer, while /APPS/FMSYNTH drained the kernel's empty one
# through SYS_DOS_FM_EVENTS. Ring-3 DOS guests therefore had no music at all.
#
# shim/kshim.c supplies the dos/dosfmq.h seam instead, forwarding every
# operation to the KERNEL's queue through SYS_DOS_FM_HOST. The exclusion is
# CHECKABLE, not asserted: `nm build/*.o | grep g_dos_fmq` finds nothing and the
# link fails loudly if some other DOS source ever reaches for the struct.
#
# rustkern/fmq.rs goes with it (it is no longer copied above, and shim/dosrust.rs
# no longer declares the module): the queue implementation belongs to whoever
# owns the queue, and leaving a compiled-in copy here would be a second
# implementation waiting for a caller. MEASURED: fmq.rs is referenced by no
# other module in rustkern/.
rm -f $G/dos/dosfmq.c

echo "gen tree: $(find $G -type f | wc -l) files"
