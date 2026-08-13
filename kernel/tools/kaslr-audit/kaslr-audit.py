#!/usr/bin/env python3
# kaslr-audit.py - measure what actually fixes the MayteraOS kernel's base.
#
# #655. THIS TOOL ENABLES NOTHING. It does not randomise anything, it does not
# change a byte of the kernel, and running it does not make KASLR closer to
# working. It exists because the #655 assessment produced a set of numbers
# (image span, absolute-relocation count, GOT slots with no relocation record,
# usable base range, entropy in bits) that are all properties of a BUILT
# kernel.elf and will drift as the kernel grows. A number quoted in a document
# rots; a number a tool re-derives from the artifact does not. See blame.md,
# "in-tree prose LIES - verify the artifact not the description".
#
# WHAT IT MEASURES, and why each number is load-bearing for #655:
#
#   image span          The distance from the lowest to the highest byte of the
#                       loaded image. linker.ld puts .text/.rodata at 0x400000
#                       and then jumps to 0x2000000 for .data/.bss, so the span
#                       is much larger than the sum of the segments. A relocating
#                       loader has to find a free hole of the SPAN, not the sum.
#
#   absolute relocs     R_X86_64_64 / _32 / _32S in ALLOCATED sections. These are
#                       the sites a slide has to fix up. They only appear if the
#                       kernel was linked with --emit-relocs (-q); a normal link
#                       discards them, which is why a stock kernel.elf reports
#                       "no relocation metadata" - that is the honest answer, not
#                       a tool failure.
#
#   pc-relative relocs  PC32 / PLT32 / GOTPCREL. Reported separately because they
#                       need NO fixup under a uniform slide, and lumping them in
#                       inflates the apparent cost of KASLR by more than half.
#
#   GOT slots           .got / .got.plt hold absolute addresses that the linker
#                       SYNTHESISED, so they have no input relocation and do NOT
#                       appear in --emit-relocs output. A relocator driven purely
#                       off the .rela sections silently skips them. This is the
#                       one measured landmine that would produce a kernel that
#                       links, boots partway, and dies somewhere unrelated.
#
#   entropy             Bits of randomness the image could ACTUALLY carry, given
#                       every constraint below. Reported per alignment because
#                       the alignment choice moves it by nine bits and any single
#                       headline figure would be picked to flatter.
#
# THE CONSTRAINTS ARE MEASURED FROM THE TREE, NOT ASSUMED. They are listed in
# WINDOWS/CEILING below with the file:line each comes from. If one of those
# constants moves, this tool's answer is wrong until the constant here is
# updated, so each carries its source.
#
# Usage:
#   kaslr-audit.py <kernel.elf>          report (always exit 0 unless unreadable)
#   kaslr-audit.py --self-test           prove the checks go RED and GREEN
#
# Deliberately NOT wired into kernel/Makefile. A post-link layout gate is a good
# idea (#655 stage 1 in the plan) but adding one is a change to the build that
# produces the golden, and this tool has never run inside that build. Wire it in
# as its own change, with its own verification.

import sys
import struct
import os
import tempfile

# --- measured constraints, each with the source that defines it ---------------

# The randomised image must not overlap these VIRTUAL windows. The kernel is
# identity-mapped (phys == virt), so a virtual collision is a real collision.
WINDOWS = [
    # (lo, hi, why)
    (0x10000000, 0x20000000,
     "kmalloc heap virtual window (kernel/mm/heap.c:18 HEAP_VIRT_BASE + "
     "HEAP_MAX_SIZE 256MB); kernel/fs/blockdev.c:91-92 restates it"),
    (0x80000000, 0xC0000000,
     "legacy user/MMIO identity PD rebuilt from a literal in every address "
     "space (kernel/mm/vmm.c:670); also kernel/exec/elf.c:89-90"),
    (0x00000000, 0x00400000,
     "low 4MB: real-mode/AP-trampoline/firmware scratch, reserved wholesale by "
     "kernel/mm/pmm.c:171-179"),
]

# Two independent reasons the whole image must stay below 2GB:
#   1. kernel/mm/pmm.c:156 PMM_IDENTITY_MAP_LIMIT - the PMM does not track a
#      page above 0x80000000, so it could not even reserve the image there.
#   2. R_X86_64_32S relocations (the -mcmodel=kernel code model, kernel/
#      Makefile:95) hold a SIGN-EXTENDED 32-bit address. 0x80000000 does not
#      fit; 0x7FFFFFFF is the last value that does.
CEILING = 0x80000000
CEILING_32S = 0x80000000  # first value an R_X86_64_32S cannot represent

ALIGNMENTS = [
    (0x1000, "4KB  (finest a page-granular loader can do)"),
    (0x200000, "2MB  (huge-page friendly; what Linux x86-64 KASLR uses)"),
    (0x40000000, "1GB  (PDPT-granular)"),
]

# Relocation types that a uniform slide must ADD the slide to.
ABSOLUTE_TYPES = {1: "R_X86_64_64", 10: "R_X86_64_32", 11: "R_X86_64_32S"}
# Relocation types that need NO fixup under a uniform slide (both ends move).
RELATIVE_TYPES = {2: "R_X86_64_PC32", 4: "R_X86_64_PLT32", 9: "R_X86_64_GOTPCREL"}

SHT_RELA = 4
SHT_NOBITS = 8
PT_LOAD = 1


# --- minimal ELF64 reader -----------------------------------------------------

class ElfError(Exception):
    pass


class Elf64:
    def __init__(self, data):
        if len(data) < 64 or data[:4] != b"\x7fELF":
            raise ElfError("not an ELF file")
        if data[4] != 2:
            raise ElfError("not ELFCLASS64")
        self.data = data
        (self.e_type, _mach, _ver, self.e_entry, self.e_phoff, self.e_shoff,
         _flags, _ehsize, self.e_phentsize, self.e_phnum, self.e_shentsize,
         self.e_shnum, self.e_shstrndx) = struct.unpack_from(
            "<HHIQQQIHHHHHH", data, 16)
        self.segments = self._read_segments()
        self.sections = self._read_sections()

    def _read_segments(self):
        out = []
        for i in range(self.e_phnum):
            off = self.e_phoff + i * self.e_phentsize
            if off + 56 > len(self.data):
                raise ElfError("program header table runs past end of file")
            p_type, p_flags, _poff, p_vaddr, _ppaddr, p_filesz, p_memsz, _al = \
                struct.unpack_from("<IIQQQQQQ", self.data, off)
            out.append({"type": p_type, "flags": p_flags, "vaddr": p_vaddr,
                        "filesz": p_filesz, "memsz": p_memsz})
        return out

    def _read_sections(self):
        out = []
        if self.e_shnum == 0:
            return out
        raw = []
        for i in range(self.e_shnum):
            off = self.e_shoff + i * self.e_shentsize
            if off + 64 > len(self.data):
                raise ElfError("section header table runs past end of file")
            raw.append(struct.unpack_from("<IIQQQQIIQQ", self.data, off))
        if self.e_shstrndx >= len(raw):
            raise ElfError("bad e_shstrndx")
        str_off, str_size = raw[self.e_shstrndx][4], raw[self.e_shstrndx][5]
        strtab = self.data[str_off:str_off + str_size]
        for (nameoff, stype, flags, addr, off, size, link, info, _al,
             entsize) in raw:
            end = strtab.find(b"\0", nameoff)
            name = strtab[nameoff:end if end >= 0 else None].decode(
                "utf-8", "replace")
            out.append({"name": name, "type": stype, "flags": flags,
                        "addr": addr, "offset": off, "size": size,
                        "link": link, "info": info, "entsize": entsize})
        return out

    def section(self, name):
        for s in self.sections:
            if s["name"] == name:
                return s
        return None


# --- analysis -----------------------------------------------------------------

def load_span(elf):
    """Lowest and highest byte of the image as the bootloader will place it."""
    loads = [s for s in elf.segments if s["type"] == PT_LOAD and s["memsz"]]
    if not loads:
        raise ElfError("no non-empty PT_LOAD segments")
    lo = min(s["vaddr"] for s in loads)
    hi = max(s["vaddr"] + s["memsz"] for s in loads)
    return lo, hi, loads


def reloc_census(elf):
    """Type census over ALLOCATED sections only.

    A --emit-relocs kernel carries ~10MB of .rela.debug_* that the loader never
    sees, because non-alloc sections are not in any PT_LOAD. Counting them would
    overstate the relocator's work by an order of magnitude, so the target
    section's own SHF_ALLOC bit decides.
    """
    SHF_ALLOC = 0x2
    absolute = {}
    relative = {}
    other = {}
    sections_seen = []
    for s in elf.sections:
        if s["type"] != SHT_RELA or s["entsize"] != 24:
            continue
        tgt_idx = s["info"]
        if tgt_idx >= len(elf.sections):
            continue
        tgt = elf.sections[tgt_idx]
        if not (tgt["flags"] & SHF_ALLOC):
            continue
        sections_seen.append((s["name"], s["size"] // 24))
        blob = elf.data[s["offset"]:s["offset"] + s["size"]]
        for off in range(0, len(blob) - 23, 24):
            r_info = struct.unpack_from("<Q", blob, off + 8)[0]
            rtype = r_info & 0xFFFFFFFF
            if rtype in ABSOLUTE_TYPES:
                absolute[ABSOLUTE_TYPES[rtype]] = \
                    absolute.get(ABSOLUTE_TYPES[rtype], 0) + 1
            elif rtype in RELATIVE_TYPES:
                relative[RELATIVE_TYPES[rtype]] = \
                    relative.get(RELATIVE_TYPES[rtype], 0) + 1
            else:
                other[rtype] = other.get(rtype, 0) + 1
    return sections_seen, absolute, relative, other


def got_slots(elf):
    """Absolute addresses the linker synthesised, which carry NO relocation."""
    total = 0
    detail = []
    for name in (".got", ".got.plt"):
        s = elf.section(name)
        if s and s["size"]:
            n = s["size"] // 8
            detail.append((name, n, s["addr"]))
            total += n
    return total, detail


def free_base_ranges(span):
    """Base addresses at which an image of `span` bytes fits, honouring every
    measured constraint. Returns a list of (lo, hi_exclusive) for the BASE."""
    # The whole image, base..base+span, must sit below the ceiling.
    hard_hi = min(CEILING, CEILING_32S)
    # Start from one interval and subtract every forbidden window, remembering
    # that the IMAGE, not just the base, must avoid it: a base is forbidden if
    # [base, base+span) intersects [lo, hi).
    intervals = [(0, hard_hi - span)]
    for (wlo, whi, _why) in WINDOWS:
        nxt = []
        for (lo, hi) in intervals:
            # forbidden base range for this window is (wlo - span, whi)
            flo, fhi = wlo - span, whi
            if fhi <= lo or flo >= hi:
                nxt.append((lo, hi))
                continue
            if lo < flo:
                nxt.append((lo, min(hi, flo)))
            if hi > fhi:
                nxt.append((max(lo, fhi), hi))
        intervals = [(a, b) for (a, b) in nxt if b > a]
    return sorted(intervals)


def count_slots(intervals, align):
    n = 0
    for (lo, hi) in intervals:
        first = (lo + align - 1) // align * align
        if first >= hi:
            continue
        n += (hi - first + align - 1) // align
    return n


def bits(n):
    if n <= 1:
        return 0.0
    b = 0.0
    v = float(n)
    while v > 1.0:
        v /= 2.0
        b += 1.0
    # exact log2 via integer bit_length plus fraction
    import math
    return math.log2(n)


def report(path):
    with open(path, "rb") as f:
        elf = Elf64(f.read())

    print("=" * 78)
    print("KASLR audit (#655): %s" % path)
    print("=" * 78)
    print("ELF type      : %s (entry 0x%X)" %
          ("EXEC (fixed base)" if elf.e_type == 2 else
           "DYN (relocatable)" if elf.e_type == 3 else
           "type %d" % elf.e_type, elf.e_entry))

    lo, hi, loads = load_span(elf)
    span = hi - lo
    print("")
    print("LOADED SEGMENTS")
    prev_end = None
    for s in sorted(loads, key=lambda x: x["vaddr"]):
        gap = ""
        if prev_end is not None and s["vaddr"] > prev_end:
            gap = "   <-- %.1f MB HOLE before this segment" % (
                (s["vaddr"] - prev_end) / 1048576.0)
        print("  vaddr 0x%08X  memsz %10d (%7.2f MB)%s" %
              (s["vaddr"], s["memsz"], s["memsz"] / 1048576.0, gap))
        prev_end = s["vaddr"] + s["memsz"]
    print("  image span 0x%08X..0x%08X = %.2f MB" %
          (lo, hi, span / 1048576.0))
    print("  NOTE: a relocating loader must find a free hole of the SPAN,")
    print("        not the sum of the segments. The hole above is dead weight.")

    print("")
    print("RELOCATION METADATA (allocated sections only)")
    seen, absolute, relative, other = reloc_census(elf)
    if not seen:
        print("  NONE. This kernel was linked without --emit-relocs (-q), so")
        print("  every absolute address is baked in with no record of where.")
        print("  Nothing can slide this image. This is the expected result for")
        print("  a stock build; it is the measurement, not a failure.")
        abs_total = None
    else:
        for (name, n) in seen:
            print("  %-16s %7d entries" % (name, n))
        abs_total = sum(absolute.values())
        rel_total = sum(relative.values())
        print("  --")
        for k in sorted(absolute):
            print("  NEEDS SLIDE   %-18s %7d" % (k, absolute[k]))
        for k in sorted(relative):
            print("  no fixup      %-18s %7d" % (k, relative[k]))
        for k in sorted(other):
            print("  UNHANDLED     type %-13d %7d  <-- relocator must reject" %
                  (k, other[k]))
        print("  --")
        print("  fixups a relocator would apply : %d" % abs_total)
        print("  entries it can skip            : %d" % rel_total)

    print("")
    print("ABSOLUTE ADDRESSES WITH NO RELOCATION RECORD")
    ngot, gotdetail = got_slots(elf)
    if ngot == 0:
        print("  none: no .got / .got.plt in this image.")
    else:
        for (name, n, addr) in gotdetail:
            print("  %-9s at 0x%08X : %d slots" % (name, addr, n))
        print("  These hold absolute addresses the LINKER synthesised, so they")
        print("  have no input relocation and do NOT appear above. A relocator")
        print("  driven only off .rela.* silently skips all %d." % ngot)
        print("  It must slide every 8-byte slot in these sections as well.")

    print("")
    print("USABLE BASE RANGE (every constraint measured from the tree)")
    for (wlo, whi, why) in WINDOWS:
        print("  forbidden 0x%08X-0x%08X  %s" % (wlo, whi, why))
    print("  ceiling   0x%08X            image must end below this: PMM "
          "identity cap" % CEILING)
    print("                                    (kernel/mm/pmm.c:156) AND "
          "R_X86_64_32S range")
    ranges = free_base_ranges(span)
    if not ranges:
        print("  RESULT: NO legal base. The image no longer fits anywhere.")
        total = 0
    else:
        total = 0
        for (a, b) in ranges:
            print("  base may be 0x%08X..0x%08X (%.1f MB wide)" %
                  (a, b, (b - a) / 1048576.0))
            total += b - a
        print("  total usable base range: %.1f MB" % (total / 1048576.0))

    print("")
    print("ENTROPY THIS IMAGE COULD CARRY")
    if total == 0:
        print("  0 bits.")
    else:
        for (align, label) in ALIGNMENTS:
            n = count_slots(ranges, align)
            print("  %-40s %8d slots = %.2f bits" % (label, n, bits(n)))
        print("")
        print("  HONEST READING: these are UPPER BOUNDS. They assume every")
        print("  byte of the usable range is free conventional RAM at boot.")
        print("  It is not: the firmware memory map fragments low memory, and")
        print("  dropping a %.0f MB image into the middle of a 2 GB pool costs" %
              (span / 1048576.0))
        print("  the physical allocator its largest contiguous run, which the")
        print("  DMA-capable drivers depend on. A real policy that keeps a big")
        print("  contiguous pool intact will land well below these numbers.")
    print("")
    return 0


# --- self-test ----------------------------------------------------------------
#
# Builds synthetic ELF64 files byte by byte, so the test needs no toolchain and
# no kernel. It must go RED on the broken shapes and GREEN on the good one; a
# checker that has never been seen to fail is not a checker.

def _build_elf(segments, sections=(), e_type=2, entry=0x400000):
    """segments: [(vaddr, memsz)]. sections: [(name, type, flags, addr, size,
    entsize, info, payload)]. Returns bytes."""
    shnames = [""] + [s[0] for s in sections] + [".shstrtab"]
    strtab = b"\0".join(n.encode() for n in shnames) + b"\0"
    nameoff = {}
    off = 0
    for n in shnames:
        nameoff[n] = off
        off += len(n) + 1

    ehsize, phentsize, shentsize = 64, 56, 64
    phoff = ehsize
    body_off = phoff + phentsize * len(segments)

    payloads = []
    cur = body_off
    for (_n, _t, _f, _a, size, _e, _i, payload) in sections:
        payloads.append((cur, payload))
        cur += len(payload)
    strtab_off = cur
    cur += len(strtab)
    shoff = (cur + 7) & ~7

    out = bytearray()
    out += b"\x7fELF\x02\x01\x01\x00" + b"\0" * 8
    out += struct.pack("<HHIQQQIHHHHHH", e_type, 0x3E, 1, entry, phoff, shoff,
                       0, ehsize, phentsize, len(segments), shentsize,
                       len(sections) + 2, len(sections) + 1)
    for (vaddr, memsz) in segments:
        out += struct.pack("<IIQQQQQQ", PT_LOAD, 5, 0, vaddr, vaddr, 0, memsz,
                           0x1000)
    for (poff, payload) in payloads:
        assert len(out) == poff, (len(out), poff)
        out += payload
    assert len(out) == strtab_off
    out += strtab
    out += b"\0" * (shoff - len(out))

    # index 0: NULL
    out += b"\0" * 64
    for i, (name, stype, flags, addr, size, entsize, info, payload) in \
            enumerate(sections):
        out += struct.pack("<IIQQQQIIQQ", nameoff[name], stype, flags, addr,
                           payloads[i][0], size, 0, info, 1, entsize)
    out += struct.pack("<IIQQQQIIQQ", nameoff[".shstrtab"], 3, 0, 0,
                       strtab_off, len(strtab), 0, 0, 1, 0)
    return bytes(out)


def _rela(entries):
    """entries: [(r_offset, sym, rtype)] -> packed Elf64_Rela payload."""
    b = bytearray()
    for (roff, sym, rtype) in entries:
        b += struct.pack("<QQq", roff, (sym << 32) | rtype, 0)
    return bytes(b)


def self_test():
    SHF_ALLOC, SHF_WRITE = 0x2, 0x1
    ok = True
    tmp = tempfile.mkdtemp(prefix="kaslr-audit-selftest-")

    def check(label, cond):
        nonlocal ok
        print("  %-62s %s" % (label, "PASS" if cond else "FAIL"))
        if not cond:
            ok = False

    print("kaslr-audit self-test")
    print("")
    print("A. ELF parsing and span")
    # Two segments with a hole, exactly the MayteraOS shape.
    e = Elf64(_build_elf([(0x400000, 0x3DB538), (0x2000000, 0x4A18468)]))
    lo, hi, loads = load_span(e)
    check("two-segment image: lo == 0x400000", lo == 0x400000)
    check("two-segment image: span includes the 28MB hole",
          hi - lo == 0x2000000 + 0x4A18468 - 0x400000)
    check("hole is not silently summed away",
          (hi - lo) > (0x3DB538 + 0x4A18468))

    print("")
    print("B. relocation census: RED on absolutes, GREEN on pure PC-relative")
    # target section .text is ALLOC; .rela.text points at it via sh_info.
    text = ("\x2etext", 1, SHF_ALLOC | 0x4, 0x400000, 16, 0, 0, b"\0" * 16)
    def mk(entries, tgt_alloc=True):
        tflags = (SHF_ALLOC | 0x4) if tgt_alloc else 0
        secs = [(".text", 1, tflags, 0x400000, 16, 0, 0, b"\0" * 16),
                (".rela.text", SHT_RELA, 0, 0, len(_rela(entries)), 24, 1,
                 _rela(entries))]
        return Elf64(_build_elf([(0x400000, 0x1000)], secs))
    seen, absolute, relative, other = reloc_census(
        mk([(0, 1, 11), (8, 1, 11), (16, 1, 1), (24, 1, 2), (32, 1, 4)]))
    check("counts R_X86_64_32S as needing a slide",
          absolute.get("R_X86_64_32S") == 2)
    check("counts R_X86_64_64 as needing a slide",
          absolute.get("R_X86_64_64") == 1)
    check("counts PC32/PLT32 as needing NO slide",
          sum(relative.values()) == 2)
    seen2, abs2, rel2, oth2 = reloc_census(mk([(0, 1, 2), (8, 1, 4)]))
    check("a purely PC-relative image reports zero fixups",
          sum(abs2.values()) == 0 and sum(rel2.values()) == 2)
    check("an unknown relocation type is surfaced, not swallowed",
          reloc_census(mk([(0, 1, 42)]))[3].get(42) == 1)

    print("")
    print("C. non-allocated .rela.debug_* must NOT inflate the count")
    secs = [(".debug_info", 1, 0, 0, 16, 0, 0, b"\0" * 16),
            (".rela.debug_info", SHT_RELA, 0, 0,
             len(_rela([(0, 1, 10)] * 50)), 24, 1, _rela([(0, 1, 10)] * 50))]
    s3, a3, r3, o3 = reloc_census(Elf64(_build_elf([(0x400000, 0x1000)], secs)))
    check("50 relocations against a non-ALLOC section are ignored",
          s3 == [] and sum(a3.values()) == 0)

    print("")
    print("D. GOT detection")
    got = [(".got", 1, SHF_ALLOC | SHF_WRITE, 0x202A2A0, 0x360, 0, 0,
            b"\0" * 0x360)]
    n, _d = got_slots(Elf64(_build_elf([(0x400000, 0x1000)], got)))
    check("a 0x360-byte .got reports 108 unrelocated absolute slots", n == 108)
    n0, _ = got_slots(Elf64(_build_elf([(0x400000, 0x1000)])))
    check("no .got reports zero (does not invent slots)", n0 == 0)

    print("")
    print("E. forbidden-window arithmetic")
    # An image of 1 byte may sit anywhere outside the windows.
    r = free_base_ranges(1)
    def covered(x, rr):
        return any(a <= x < b for (a, b) in rr)
    check("base inside the kmalloc heap window is REJECTED",
          not covered(0x18000000, r))
    check("base inside the legacy 2-3GB window is REJECTED",
          not covered(0x90000000, r))
    check("base in the low 4MB is REJECTED", not covered(0x200000, r))
    check("base at 0x30000000 is ACCEPTED", covered(0x30000000, r))
    # A large image must be rejected for STRADDLING a window even though its
    # base is legal. This is the case a naive base-only check gets wrong.
    big = free_base_ranges(0x8000000)  # 128MB image
    check("a 128MB image based just below the heap window is REJECTED "
          "(straddle)", not covered(0x0F000000, big))
    check("the real 102MB span still has somewhere to go",
          sum(b - a for (a, b) in free_base_ranges(0x6619000)) > 0)
    check("an image larger than the whole usable space has NO legal base",
          free_base_ranges(0x7F000000) == [])

    print("")
    print("F. entropy arithmetic")
    check("512 slots is exactly 9 bits", abs(bits(512) - 9.0) < 1e-9)
    check("1 slot is 0 bits (no randomisation)", bits(1) == 0.0)
    check("coarser alignment yields strictly fewer slots",
          count_slots(r, 0x200000) < count_slots(r, 0x1000))

    print("")
    print("G. end-to-end: report() runs on a synthetic kernel-shaped ELF")
    p = os.path.join(tmp, "synthetic.elf")
    with open(p, "wb") as f:
        f.write(_build_elf([(0x400000, 0x3DB538), (0x2000000, 0x4A18468)]))
    try:
        rc = report(p)
        check("report() completes on a well-formed image", rc == 0)
    except Exception as exc:                                  # noqa: BLE001
        check("report() completes on a well-formed image (%s)" % exc, False)
    try:
        Elf64(b"not an elf at all")
        check("a non-ELF input is rejected", False)
    except ElfError:
        check("a non-ELF input is rejected", True)

    print("")
    print("SELF-TEST %s" % ("PASS" if ok else "FAIL"))
    return 0 if ok else 1


def main(argv):
    if len(argv) == 2 and argv[1] == "--self-test":
        return self_test()
    if len(argv) != 2:
        print(__doc__ or "", file=sys.stderr)
        print("usage: kaslr-audit.py <kernel.elf> | --self-test",
              file=sys.stderr)
        return 2
    try:
        return report(argv[1])
    except (ElfError, OSError) as exc:
        print("kaslr-audit: %s: %s" % (argv[1], exc), file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
