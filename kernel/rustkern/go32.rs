// rustkern/go32.rs - the go32/DJGPP v2 i386 COFF loader (#211).
//
// WHAT THIS FORMAT IS, AND WHY THE STUB IS BYPASSED RATHER THAN RUN
// -----------------------------------------------------------------
// A DJGPP v2 program is a real-mode MZ ("the stub", `go32stub, v 2.0x`) with a
// 32-bit i386 COFF image concatenated after it. The stub's job on real DOS is
// to find a DPMI host: it probes INT 2Fh AX=1687h and, failing that, loads
// CWSDPMI.EXE, a real-mode program that puts the CPU into protected mode
// ITSELF. Our 16-bit interpreter cannot execute a mode switch, so there is no
// version of "run the stub" that ends anywhere useful.
//
// So we do what a DPMI host does: read the COFF ourselves, lay it out, build
// the `stubinfo` the stub would have built, and enter the program's own entry
// point in 32-bit protected mode. That is exactly the shape dos4gw_prepare()
// already uses for an LE module (dos/dosexec.c), and it is why THIS module is
// only the format: where the memory goes is the host's decision, not ours.
//
// THE TWO THINGS THAT ARE EASY TO GET WRONG AND ARE ASSERTED HERE
// ---------------------------------------------------------------
// 1. THE COFF IS NOT AT A FIXED OFFSET. It begins at the end of the MZ image
//    as the MZ header itself describes it: (e_cp - 1) * 512 + e_cblp, or
//    e_cp * 512 when e_cblp is zero. For the measured NetHack that is 0x800,
//    and hardcoding 0x800 would work for exactly the binaries built by the
//    same stubify and silently mislocate everything else.
// 2. ZMAGIC SECTION POINTERS ARE RELATIVE TO THE COFF HEADER, NOT TO THE FILE.
//    `.text` in the measured binary has scnptr 0xD0, which is file offset
//    0x8D0. Treating it as a file offset reads 0x730 bytes of the DOS stub as
//    the program's first instructions, which decodes as SOMETHING and runs.
//
// THE STUBINFO IS NOT OPTIONAL AND IS NOT PASSED IN A REGISTER. djgpp's crt0
// reads it through FS at offsets 0x10/0x14/0x18/0x1C in its first 0x180 bytes,
// and crt1 reads 0x20/0x22/0x24/0x26 afterwards. The ADDRESS is communicated
// only by FS's descriptor base. A loader that sets every register correctly
// and leaves FS alone produces a program that faults or, worse, reads a
// plausible-looking zero and sizes its heap to nothing.
#![allow(dead_code)]

extern "C" {
    fn kprintf(fmt: *const u8, ...) -> i32;
}

// Error codes. Negative, and each names ONE thing that was wrong, because
// "load failed" on a 1.8 MB binary is not a diagnosis.
pub const GO32_OK: i32 = 0;
pub const GO32_E_SHORT: i32 = -1; // file smaller than the header it claims
pub const GO32_E_NOTMZ: i32 = -2; // no MZ signature
pub const GO32_E_NOSTUB: i32 = -3; // MZ, but no go32stub marker in it
pub const GO32_E_NOCOFF: i32 = -4; // no i386 COFF where the MZ header says
pub const GO32_E_NOTZMAGIC: i32 = -5; // COFF, but not a ZMAGIC executable
pub const GO32_E_SECTIONS: i32 = -6; // section table absurd or out of file
pub const GO32_E_NOTEXT: i32 = -7; // no .text section
pub const GO32_E_TOOBIG: i32 = -8; // image does not fit the caller's window
pub const GO32_E_ARGS: i32 = -9; // null pointer or zero length from the caller

/// The 0x54-byte structure go32's stub hands to the program through FS.
/// Field names and offsets are djgpp's `Stub_Info`; `go32_stubinfo_size_rs()`
/// exports sizeof so the C side can `_Static_assert` against it.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct Go32StubInfo {
    pub magic: [u8; 16],      // 0x00 "go32stub, v 2.04"
    pub size: u32,            // 0x10 sizeof(this) == 0x54
    pub minstack: u32,        // 0x14 stack the program insists on
    pub memory_handle: u32,   // 0x18 DPMI handle of the program's block
    pub initial_size: u32,    // 0x1C size of the program's segment
    pub minkeep: u16,         // 0x20 bytes of DOS memory kept as __tb
    pub ds_selector: u16,     // 0x22 selector for that DOS block
    pub ds_segment: u16,      // 0x24 its real-mode segment: __tb = seg << 4
    pub psp_selector: u16,    // 0x26 selector covering the PSP
    pub cs_selector: u16,     // 0x28 the stub's own code selector (exit path)
    pub env_size: u16,        // 0x2A
    pub basename: [u8; 8],    // 0x2C
    pub argv0: [u8; 16],      // 0x34
    pub dpmi_server: [u8; 16], // 0x44
}

/// Everything the host needs in order to decide where to put the image.
/// `*_off` are FILE offsets, already resolved from the ZMAGIC section
/// pointers; `*_va` are the addresses inside the program's own segment.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct Go32Image {
    pub coff_off: u32,
    pub entry: u32,
    pub text_va: u32,
    pub text_sz: u32,
    pub text_off: u32,
    pub data_va: u32,
    pub data_sz: u32,
    pub data_off: u32,
    pub bss_va: u32,
    pub bss_sz: u32,
    /// bss end rounded up to 64 KiB: the smallest segment that can hold the
    /// image. go32 rounds the same way, which is why a djgpp program's own
    /// `sbrk` arithmetic lines up with it.
    pub image_top: u32,
    /// File offset of the stubinfo TEMPLATE inside the stub, or 0 if absent.
    /// minstack and minkeep are read from it rather than invented.
    pub stub_off: u32,
    pub minstack: u32,
    pub minkeep: u32,
    pub nsections: u32,
}

pub const GO32_IMAGE_ZERO: Go32Image = Go32Image {
    coff_off: 0,
    entry: 0,
    text_va: 0,
    text_sz: 0,
    text_off: 0,
    data_va: 0,
    data_sz: 0,
    data_off: 0,
    bss_va: 0,
    bss_sz: 0,
    image_top: 0,
    stub_off: 0,
    minstack: 0,
    minkeep: 0,
    nsections: 0,
};

// ---------------------------------------------------------------------------
// Little-endian readers that REFUSE rather than wrap. Every one of these takes
// the length, because the whole attack surface of this file is a
// caller-supplied buffer whose contents are a downloaded DOS executable.
// ---------------------------------------------------------------------------
#[inline]
fn rd8(f: &[u8], o: u32) -> Option<u8> {
    f.get(o as usize).copied()
}

#[inline]
fn rd16(f: &[u8], o: u32) -> Option<u16> {
    let a = rd8(f, o)? as u16;
    let b = rd8(f, o + 1)? as u16;
    Some(a | (b << 8))
}

#[inline]
fn rd32(f: &[u8], o: u32) -> Option<u32> {
    let a = rd16(f, o)? as u32;
    let b = rd16(f, o + 2)? as u32;
    Some(a | (b << 16))
}

const MAGIC: &[u8; 8] = b"go32stub";

/// Find the stubinfo template inside the stub. The stub carries it as data, so
/// its offset is a property of the build, not a constant.
fn find_stub(f: &[u8], limit: u32) -> Option<u32> {
    let lim = if (limit as usize) < f.len() { limit as usize } else { f.len() };
    if lim < MAGIC.len() {
        return None;
    }
    let mut i = 0usize;
    while i + MAGIC.len() <= lim {
        if &f[i..i + MAGIC.len()] == MAGIC {
            return Some(i as u32);
        }
        i += 1;
    }
    None
}

/// Parse the MZ + COFF headers. No memory is touched and nothing is loaded;
/// this is also the DETECTOR, so it must be cheap and must never print.
#[no_mangle]
pub unsafe extern "C" fn go32_parse_rs(
    file: *const u8,
    len: u32,
    out: *mut Go32Image,
) -> i32 {
    if file.is_null() || out.is_null() || len < 0x40 {
        return GO32_E_ARGS;
    }
    let f = unsafe { core::slice::from_raw_parts(file, len as usize) };
    let out = unsafe { &mut *out };
    *out = GO32_IMAGE_ZERO;

    if rd8(f, 0) != Some(b'M') || rd8(f, 1) != Some(b'Z') {
        return GO32_E_NOTMZ;
    }
    let e_cblp = match rd16(f, 2) { Some(v) => v as u32, None => return GO32_E_SHORT };
    let e_cp = match rd16(f, 4) { Some(v) => v as u32, None => return GO32_E_SHORT };
    if e_cp == 0 {
        return GO32_E_NOCOFF;
    }
    // THE MZ IMAGE LENGTH, and therefore where the COFF starts. See the header.
    let coff = if e_cblp != 0 { (e_cp - 1) * 512 + e_cblp } else { e_cp * 512 };
    if coff < 0x40 || coff as usize >= f.len() {
        return GO32_E_NOCOFF;
    }

    // The go32 marker must be inside the stub. Refusing a plain MZ here is the
    // point: dos_run_file() tries this parser on EVERY MZ, and a false positive
    // would take an ordinary DOS program down a protected-mode path.
    let stub = match find_stub(f, coff) {
        Some(s) => s,
        None => return GO32_E_NOSTUB,
    };

    // COFF file header.
    let f_magic = match rd16(f, coff) { Some(v) => v, None => return GO32_E_SHORT };
    if f_magic != 0x014C {
        return GO32_E_NOCOFF; // not i386 COFF
    }
    let nscns = match rd16(f, coff + 2) { Some(v) => v as u32, None => return GO32_E_SHORT };
    let opthdr = match rd16(f, coff + 16) { Some(v) => v as u32, None => return GO32_E_SHORT };
    if nscns == 0 || nscns > 32 {
        return GO32_E_SECTIONS;
    }
    if opthdr < 28 {
        return GO32_E_NOTZMAGIC; // no a.out header: not an executable
    }
    let a = coff + 20;
    let amagic = match rd16(f, a) { Some(v) => v, None => return GO32_E_SHORT };
    if amagic != 0x010B {
        return GO32_E_NOTZMAGIC; // 0x010B == ZMAGIC; djgpp emits only this
    }
    out.entry = match rd32(f, a + 16) { Some(v) => v, None => return GO32_E_SHORT };
    out.coff_off = coff;
    out.nsections = nscns;
    out.stub_off = stub;
    out.minstack = rd32(f, stub + 0x14).unwrap_or(0);
    out.minkeep = rd16(f, stub + 0x20).unwrap_or(0) as u32;

    // Section table.
    let mut s = a + opthdr;
    let mut i = 0u32;
    let mut have_text = false;
    while i < nscns {
        if (s + 40) as usize > f.len() {
            return GO32_E_SECTIONS;
        }
        let mut name = [0u8; 8];
        let mut k = 0usize;
        while k < 8 {
            name[k] = rd8(f, s + k as u32).unwrap_or(0);
            k += 1;
        }
        let vaddr = rd32(f, s + 12).unwrap_or(0);
        let size = rd32(f, s + 16).unwrap_or(0);
        let scnptr = rd32(f, s + 20).unwrap_or(0);
        // ZMAGIC: relative to the COFF header. THE bug this comment prevents.
        let foff = coff.wrapping_add(scnptr);

        // A section with contents must lie wholly inside the file.
        let has_content = scnptr != 0 && size != 0;
        if has_content
            && (foff < coff
                || (foff as u64 + size as u64) > f.len() as u64)
        {
            return GO32_E_SECTIONS;
        }

        if &name[..5] == b".text" {
            out.text_va = vaddr;
            out.text_sz = size;
            out.text_off = foff;
            have_text = true;
        } else if &name[..5] == b".data" {
            out.data_va = vaddr;
            out.data_sz = size;
            out.data_off = foff;
        } else if &name[..4] == b".bss" && name[4] == 0 {
            out.bss_va = vaddr;
            out.bss_sz = size;
        }
        s += 40;
        i += 1;
    }
    if !have_text || out.text_sz == 0 {
        return GO32_E_NOTEXT;
    }

    // The smallest segment that holds everything, rounded to 64 KiB the way
    // go32 rounds it.
    let mut top = out.text_va as u64 + out.text_sz as u64;
    let d = out.data_va as u64 + out.data_sz as u64;
    if d > top {
        top = d;
    }
    let b = out.bss_va as u64 + out.bss_sz as u64;
    if b > top {
        top = b;
    }
    top = (top + 0xFFFF) & !0xFFFFu64;
    if top > 0x4000_0000 {
        return GO32_E_TOOBIG;
    }
    out.image_top = top as u32;

    // The entry has to be inside .text, or we would be jumping into data with
    // a perfectly successful-looking load behind us.
    if out.entry < out.text_va || out.entry >= out.text_va.wrapping_add(out.text_sz) {
        return GO32_E_NOTEXT;
    }
    GO32_OK
}

#[no_mangle]
pub unsafe extern "C" fn go32_strerror_rs(e: i32) -> *const u8 {
    match e {
        GO32_OK => b"ok\0".as_ptr(),
        GO32_E_SHORT => b"file ends inside a header it declares\0".as_ptr(),
        GO32_E_NOTMZ => b"not an MZ executable\0".as_ptr(),
        GO32_E_NOSTUB => b"MZ, but no go32stub marker: not a DJGPP image\0".as_ptr(),
        GO32_E_NOCOFF => b"no i386 COFF where the MZ header says the image ends\0".as_ptr(),
        GO32_E_NOTZMAGIC => b"COFF present but not a ZMAGIC executable\0".as_ptr(),
        GO32_E_SECTIONS => b"section table is absurd or points outside the file\0".as_ptr(),
        GO32_E_NOTEXT => b"no .text, or the entry point is not inside it\0".as_ptr(),
        GO32_E_TOOBIG => b"image does not fit the segment the host offered\0".as_ptr(),
        GO32_E_ARGS => b"bad arguments\0".as_ptr(),
        _ => b"unknown error\0".as_ptr(),
    }
}

/// Lay the image out at `base` inside `arena`. `.bss` is zeroed explicitly
/// rather than relying on the caller having zeroed the arena, because "the
/// caller zeroed it" is the kind of precondition that survives exactly until
/// someone reuses this for a second guest.
#[no_mangle]
pub unsafe extern "C" fn go32_load_rs(
    img: *const Go32Image,
    file: *const u8,
    len: u32,
    arena: *mut u8,
    arena_len: u32,
    base: u32,
    seg_size: u32,
) -> i32 {
    if img.is_null() || file.is_null() || arena.is_null() {
        return GO32_E_ARGS;
    }
    let img = unsafe { &*img };
    let f = unsafe { core::slice::from_raw_parts(file, len as usize) };
    let a = unsafe { core::slice::from_raw_parts_mut(arena, arena_len as usize) };

    if (base as u64 + seg_size as u64) > arena_len as u64 {
        return GO32_E_TOOBIG;
    }
    if img.image_top > seg_size {
        return GO32_E_TOOBIG;
    }

    // .text and .data
    let mut copy = |va: u32, sz: u32, off: u32| -> bool {
        if sz == 0 {
            return true;
        }
        if (va as u64 + sz as u64) > seg_size as u64 {
            return false;
        }
        if (off as u64 + sz as u64) > f.len() as u64 {
            return false;
        }
        let d0 = (base + va) as usize;
        let s0 = off as usize;
        let mut i = 0usize;
        while i < sz as usize {
            a[d0 + i] = f[s0 + i];
            i += 1;
        }
        true
    };
    if !copy(img.text_va, img.text_sz, img.text_off) {
        return GO32_E_SECTIONS;
    }
    if !copy(img.data_va, img.data_sz, img.data_off) {
        return GO32_E_SECTIONS;
    }

    // .bss
    if img.bss_sz != 0 {
        if (img.bss_va as u64 + img.bss_sz as u64) > seg_size as u64 {
            return GO32_E_TOOBIG;
        }
        let d0 = (base + img.bss_va) as usize;
        let mut i = 0usize;
        while i < img.bss_sz as usize {
            a[d0 + i] = 0;
            i += 1;
        }
    }
    GO32_OK
}

#[no_mangle]
pub extern "C" fn go32_stubinfo_size_rs() -> u32 {
    core::mem::size_of::<Go32StubInfo>() as u32
}

/// Build the stubinfo the program will read through FS. Everything the caller
/// passes is something only the caller can know: it owns the memory and the
/// descriptors.
#[no_mangle]
pub unsafe extern "C" fn go32_stubinfo_build_rs(
    dst: *mut u8,
    dst_len: u32,
    minstack: u32,
    memory_handle: u32,
    initial_size: u32,
    minkeep: u16,
    ds_selector: u16,
    ds_segment: u16,
    psp_selector: u16,
    cs_selector: u16,
    env_size: u16,
    argv0: *const u8,
) -> i32 {
    let need = core::mem::size_of::<Go32StubInfo>() as u32;
    if dst.is_null() || dst_len < need {
        return GO32_E_ARGS;
    }
    let mut si = Go32StubInfo {
        magic: *b"go32stub, v 2.04",
        size: need,
        minstack,
        memory_handle,
        initial_size,
        minkeep,
        ds_selector,
        ds_segment,
        psp_selector,
        cs_selector,
        env_size,
        basename: [0; 8],
        argv0: [0; 16],
        dpmi_server: [0; 16],
    };
    // argv0 is char[16] in this structure and djgpp reads it as a NUL-terminated
    // string, so it is TRUNCATED rather than overflowed, and the terminator is
    // written by construction (the array starts zeroed and the last byte is
    // never touched).
    if !argv0.is_null() {
        let mut i = 0usize;
        while i < 15 {
            let c = unsafe { *argv0.add(i) };
            if c == 0 {
                break;
            }
            si.argv0[i] = c;
            i += 1;
        }
        // basename: the tail after the last separator, uppercased 8.3-style.
        let mut last = 0usize;
        let mut j = 0usize;
        while j < i {
            if si.argv0[j] == b'\\' || si.argv0[j] == b'/' || si.argv0[j] == b':' {
                last = j + 1;
            }
            j += 1;
        }
        let mut k = 0usize;
        while k < 8 && last + k < i {
            let c = si.argv0[last + k];
            if c == b'.' {
                break;
            }
            si.basename[k] = c;
            k += 1;
        }
    }

    let src = &si as *const Go32StubInfo as *const u8;
    let mut i = 0usize;
    while i < need as usize {
        unsafe { *dst.add(i) = *src.add(i) };
        i += 1;
    }
    GO32_OK
}

/// One line naming what was found, for the boot log. Separate from the parser
/// so the parser can stay silent when it is being used as a detector.
#[no_mangle]
pub unsafe extern "C" fn go32_report_rs(path: *const u8, img: *const Go32Image) {
    if img.is_null() {
        return;
    }
    let i = unsafe { &*img };
    unsafe {
        kprintf(
            b"[go32] %s: DJGPP v2 COFF at file offset 0x%08x, entry 0x%08x, .text 0x%08x+0x%x, .data 0x%08x+0x%x, .bss 0x%08x+0x%x, segment needs %u KiB, minstack %u KiB, minkeep %u bytes\n\0"
                .as_ptr(),
            if path.is_null() { b"?\0".as_ptr() } else { path },
            i.coff_off,
            i.entry,
            i.text_va,
            i.text_sz,
            i.data_va,
            i.data_sz,
            i.bss_va,
            i.bss_sz,
            i.image_top >> 10,
            i.minstack >> 10,
            i.minkeep,
        );
    }
}

// ---------------------------------------------------------------------------
// The self-test.
//
// It builds a SYNTHETIC go32 image in caller-supplied scratch and parses it,
// then checks the rejections. Synthetic rather than a copy of NetHack's header
// because the point is to pin the FORMAT rules (where the COFF is, what scnptr
// is relative to, what the stubinfo offsets are), and a single real header
// pins only the values that particular linker happened to emit.
//
// SCRATCH IS CALLER-SUPPLIED AND THE TEST REPORTS SKIPPED WITHOUT IT. #212
// shipped a 16 KiB self-test buffer on the DOS task's 64 KiB kernel stack and
// got 40 stack-overflow reports and an intermittent Invalid Opcode panic.
// ---------------------------------------------------------------------------
// Two COFF offsets, because the MZ image-length arithmetic has TWO branches
// and a fixture that exercises one of them proves half a rule. 0x400 is an
// exact multiple of 512 (e_cblp == 0); 0x520 is not (e_cblp == 288).
const ST_COFF: u32 = 0x520;
const ST_COFF_ALT: u32 = 0x400;
const ST_TEXT_VA: u32 = 0x10D0;
const ST_TEXT_SZ: u32 = 0x40;
const ST_DATA_VA: u32 = 0x2000;
const ST_DATA_SZ: u32 = 0x20;
const ST_BSS_VA: u32 = 0x3000;
const ST_BSS_SZ: u32 = 0x1000;
const ST_MINSTACK: u32 = 0x0020_0000;
const ST_MINKEEP: u16 = 0x4000;

fn w16(b: &mut [u8], o: usize, v: u16) {
    b[o] = (v & 0xFF) as u8;
    b[o + 1] = (v >> 8) as u8;
}
fn w32(b: &mut [u8], o: usize, v: u32) {
    b[o] = (v & 0xFF) as u8;
    b[o + 1] = ((v >> 8) & 0xFF) as u8;
    b[o + 2] = ((v >> 16) & 0xFF) as u8;
    b[o + 3] = ((v >> 24) & 0xFF) as u8;
}

fn build_fixture(b: &mut [u8], coff_off: u32) -> u32 {
    let n = b.len();
    let mut i = 0usize;
    while i < n {
        b[i] = 0;
        i += 1;
    }
    // MZ header saying the image ends at coff_off, expressed the way a real
    // MZ expresses it: e_cblp is the length of the LAST page, and is 0 when
    // the image is an exact multiple of 512.
    b[0] = b'M';
    b[1] = b'Z';
    let cblp = coff_off % 512;
    let cp = if cblp == 0 { coff_off / 512 } else { coff_off / 512 + 1 };
    w16(b, 2, cblp as u16); // e_cblp
    w16(b, 4, cp as u16); // e_cp
    w16(b, 8, 4); // e_cparhdr
    // The stubinfo template, wherever it lands inside the stub.
    let stub = 0x200usize;
    let m = b"go32stub, v 2.04";
    let mut k = 0usize;
    while k < 16 {
        b[stub + k] = m[k];
        k += 1;
    }
    w32(b, stub + 0x10, 0x54);
    w32(b, stub + 0x14, ST_MINSTACK);
    w16(b, stub + 0x20, ST_MINKEEP);

    // COFF file header: i386, 3 sections, 28-byte a.out header.
    let c = coff_off as usize;
    w16(b, c, 0x014C);
    w16(b, c + 2, 3);
    w16(b, c + 16, 28);
    // a.out header
    w16(b, c + 20, 0x010B); // ZMAGIC
    w32(b, c + 20 + 16, ST_TEXT_VA); // entry
    // Section table. scnptr is RELATIVE TO c.
    let s = c + 20 + 28;
    let text_scnptr = (s - c) as u32 + 3 * 40; // right after the table
    b[s] = b'.';
    b[s + 1] = b't';
    b[s + 2] = b'e';
    b[s + 3] = b'x';
    b[s + 4] = b't';
    w32(b, s + 12, ST_TEXT_VA);
    w32(b, s + 16, ST_TEXT_SZ);
    w32(b, s + 20, text_scnptr);

    let s2 = s + 40;
    b[s2] = b'.';
    b[s2 + 1] = b'd';
    b[s2 + 2] = b'a';
    b[s2 + 3] = b't';
    b[s2 + 4] = b'a';
    w32(b, s2 + 12, ST_DATA_VA);
    w32(b, s2 + 16, ST_DATA_SZ);
    w32(b, s2 + 20, text_scnptr + ST_TEXT_SZ);

    let s3 = s2 + 40;
    b[s3] = b'.';
    b[s3 + 1] = b'b';
    b[s3 + 2] = b's';
    b[s3 + 3] = b's';
    w32(b, s3 + 12, ST_BSS_VA);
    w32(b, s3 + 16, ST_BSS_SZ);
    w32(b, s3 + 20, 0); // no contents

    // Section contents: a distinctive byte pattern so the load can be checked
    // for having copied the RIGHT bytes to the RIGHT place, not merely
    // something to somewhere.
    let t0 = c + text_scnptr as usize;
    let mut j = 0usize;
    while j < ST_TEXT_SZ as usize {
        b[t0 + j] = 0xA0u8.wrapping_add(j as u8);
        j += 1;
    }
    let d0 = t0 + ST_TEXT_SZ as usize;
    let mut j = 0usize;
    while j < ST_DATA_SZ as usize {
        b[d0 + j] = 0xD0u8.wrapping_add(j as u8);
        j += 1;
    }
    (d0 + ST_DATA_SZ as usize) as u32
}

/// Returns 0 if every check passed, else the NUMBER OF THE FIRST FAILING
/// CHECK; *out_checks receives how many RAN. A NULL or too-small scratch
/// reports 0 checks and 0 failures, which the caller must print as SKIPPED and
/// never as PASS.
#[no_mangle]
pub unsafe extern "C" fn go32_selftest_rs(
    scratch: *mut u8,
    scratch_len: u32,
    out_checks: *mut u32,
) -> u32 {
    let need_file: u32 = 0x1000;
    let need_arena: u32 = 0x20000;
    if scratch.is_null() || scratch_len < need_file + need_arena {
        if !out_checks.is_null() {
            unsafe { *out_checks = 0 };
        }
        return 0;
    }
    let all = unsafe { core::slice::from_raw_parts_mut(scratch, scratch_len as usize) };
    let (fbuf, abuf) = all.split_at_mut(need_file as usize);
    // ZERO THE ARENA. The scratch is the caller's kmalloc, which is not zeroed,
    // and two of the checks below are about what the loader did NOT write
    // (nothing below the load base, .bss really cleared). Asserting those over
    // uninitialised heap tests the allocator, not the loader, and it failed for
    // exactly that reason the first time it ran.
    {
        let mut i = 0usize;
        while i < abuf.len() {
            abuf[i] = 0;
            i += 1;
        }
    }

    // `bad` is the number of the FIRST failing check, not a count. An index
    // names the assertion; a count only says that something went wrong.
    let mut bad = 0u32;
    let mut n = 0u32;
    macro_rules! ck {
        ($cond:expr) => {{
            n += 1;
            if !($cond) && bad == 0 {
                bad = n;
            }
        }};
    }

    // Pass one: the e_cblp != 0 branch of the MZ image-length arithmetic.
    let flen = build_fixture(fbuf, ST_COFF);
    let mut img = GO32_IMAGE_ZERO;
    let rc = unsafe { go32_parse_rs(fbuf.as_ptr(), flen, &mut img) };
    ck!(rc == GO32_OK);
    ck!(img.coff_off == ST_COFF);
    ck!(img.entry == ST_TEXT_VA);
    ck!(img.text_va == ST_TEXT_VA && img.text_sz == ST_TEXT_SZ);
    ck!(img.data_va == ST_DATA_VA && img.data_sz == ST_DATA_SZ);
    ck!(img.bss_va == ST_BSS_VA && img.bss_sz == ST_BSS_SZ);
    ck!(img.minstack == ST_MINSTACK);
    ck!(img.minkeep == ST_MINKEEP as u32);
    ck!(img.nsections == 3);
    // image_top is bss end rounded UP to 64 KiB: 0x4000 -> 0x10000.
    ck!(img.image_top == 0x10000);
    // The section pointer was relative to the COFF, so text_off must be
    // coff + scnptr and NOT scnptr. This is the check that would have caught
    // the classic mislocation.
    ck!(img.text_off > ST_COFF);

    // Load it and verify the bytes landed at base + vaddr.
    let base: u32 = 0x1000;
    let seg: u32 = 0x10000;
    let lrc = unsafe {
        go32_load_rs(
            &img,
            fbuf.as_ptr(),
            flen,
            abuf.as_mut_ptr(),
            need_arena,
            base,
            seg,
        )
    };
    ck!(lrc == GO32_OK);
    ck!(abuf[(base + ST_TEXT_VA) as usize] == 0xA0);
    ck!(abuf[(base + ST_TEXT_VA + 1) as usize] == 0xA1);
    ck!(abuf[(base + ST_TEXT_VA + ST_TEXT_SZ - 1) as usize] == 0xA0u8.wrapping_add((ST_TEXT_SZ - 1) as u8));
    ck!(abuf[(base + ST_DATA_VA) as usize] == 0xD0);
    ck!(abuf[(base + ST_DATA_VA + ST_DATA_SZ - 1) as usize] == 0xD0u8.wrapping_add((ST_DATA_SZ - 1) as u8));
    // Nothing was written BELOW the base, which is where the host keeps the
    // guest's first megabyte.
    ck!(abuf[0] == 0);
    ck!(abuf[(base - 1) as usize] == 0);
    // .bss is zero and did not eat .data.
    ck!(abuf[(base + ST_BSS_VA) as usize] == 0);

    // --- rejections, each for ONE reason -------------------------------
    let mut img2 = GO32_IMAGE_ZERO;
    fbuf[0] = b'X';
    ck!(unsafe { go32_parse_rs(fbuf.as_ptr(), flen, &mut img2) } == GO32_E_NOTMZ);
    fbuf[0] = b'M';

    // A plain MZ with no go32 marker must be REFUSED, or dos_run_file() would
    // route every DOS program down the protected-mode path.
    fbuf[0x200] = b'X';
    ck!(unsafe { go32_parse_rs(fbuf.as_ptr(), flen, &mut img2) } == GO32_E_NOSTUB);
    fbuf[0x200] = b'g';

    // Wrong COFF machine.
    w16(fbuf, ST_COFF as usize, 0x8664);
    ck!(unsafe { go32_parse_rs(fbuf.as_ptr(), flen, &mut img2) } == GO32_E_NOCOFF);
    w16(fbuf, ST_COFF as usize, 0x014C);

    // Not ZMAGIC.
    w16(fbuf, (ST_COFF + 20) as usize, 0x0107);
    ck!(unsafe { go32_parse_rs(fbuf.as_ptr(), flen, &mut img2) } == GO32_E_NOTZMAGIC);
    w16(fbuf, (ST_COFF + 20) as usize, 0x010B);

    // A section whose contents run off the end of the file.
    let s = (ST_COFF + 20 + 28) as usize;
    let keep = ST_TEXT_SZ;
    w32(fbuf, s + 16, 0x10_0000);
    ck!(unsafe { go32_parse_rs(fbuf.as_ptr(), flen, &mut img2) } == GO32_E_SECTIONS);
    w32(fbuf, s + 16, keep);

    // An entry point outside .text.
    w32(fbuf, (ST_COFF + 20 + 16) as usize, 0x9000_0000);
    ck!(unsafe { go32_parse_rs(fbuf.as_ptr(), flen, &mut img2) } == GO32_E_NOTEXT);
    w32(fbuf, (ST_COFF + 20 + 16) as usize, ST_TEXT_VA);

    // Back to good, so a later failure cannot be blamed on a poisoned fixture.
    ck!(unsafe { go32_parse_rs(fbuf.as_ptr(), flen, &mut img2) } == GO32_OK);

    // A segment too small for the image must be refused by the LOADER, not
    // truncated into it.
    ck!(unsafe {
        go32_load_rs(&img, fbuf.as_ptr(), flen, abuf.as_mut_ptr(), need_arena, base, 0x2000)
    } == GO32_E_TOOBIG);

    // --- the stubinfo -----------------------------------------------------
    ck!(go32_stubinfo_size_rs() == 0x54);
    let mut si = [0u8; 0x54];
    let name = b"C:\\NETHACK\\NETHACK.EXE\0";
    let rc = unsafe {
        go32_stubinfo_build_rs(
            si.as_mut_ptr(),
            0x54,
            ST_MINSTACK,
            0x1234_5678,
            0x00C0_0000,
            ST_MINKEEP,
            0x0057,
            0x2000,
            0x005F,
            0x0067,
            0,
            name.as_ptr(),
        )
    };
    ck!(rc == GO32_OK);
    ck!(&si[0..16] == b"go32stub, v 2.04");
    // Every offset djgpp's crt0/crt1 reads, checked as a NUMBER at the exact
    // byte the guest loads from. A struct that is right in Rust and wrong at
    // offset 0x1C is a guest with a zero-sized heap and no diagnosis.
    let g32 = |o: usize| -> u32 {
        (si[o] as u32) | ((si[o + 1] as u32) << 8) | ((si[o + 2] as u32) << 16) | ((si[o + 3] as u32) << 24)
    };
    let g16 = |o: usize| -> u16 { (si[o] as u16) | ((si[o + 1] as u16) << 8) };
    ck!(g32(0x10) == 0x54); // size
    ck!(g32(0x14) == ST_MINSTACK); // minstack   <- crt0 sizes its stack from this
    ck!(g32(0x18) == 0x1234_5678); // memory_handle
    ck!(g32(0x1C) == 0x00C0_0000); // initial_size <- crt0's sbrk ceiling
    ck!(g16(0x20) == ST_MINKEEP); // minkeep    <- becomes __tb_size
    ck!(g16(0x22) == 0x0057); // ds_selector
    ck!(g16(0x24) == 0x2000); // ds_segment <- __tb = this << 4
    ck!(g16(0x26) == 0x005F); // psp_selector
    ck!(g16(0x28) == 0x0067); // cs_selector
    ck!(g16(0x2A) == 0); // env_size
    // argv0 is char[16]: 15 characters and a terminator, so a longer DOS path
    // is TRUNCATED and still NUL-terminated. basename is derived from the
    // truncated string, which is what go32 itself does.
    ck!(&si[0x34..0x44] == b"C:\\NETHACK\\NETH\0");
    ck!(si[0x43] == 0);
    ck!(&si[0x2C..0x34] == b"NETH\0\0\0\0");

    // --- pass two: the e_cblp == 0 branch, at a different COFF offset ------
    // Same fixture, relocated. If the image-length arithmetic were hardcoded
    // to 0x800, or to one of the two branches, this pass would locate nothing.
    let flen2 = build_fixture(fbuf, ST_COFF_ALT);
    let mut img3 = GO32_IMAGE_ZERO;
    ck!(unsafe { go32_parse_rs(fbuf.as_ptr(), flen2, &mut img3) } == GO32_OK);
    ck!(img3.coff_off == ST_COFF_ALT);
    ck!(img3.entry == ST_TEXT_VA);
    ck!(img3.text_off == ST_COFF_ALT + (20 + 28 + 3 * 40));
    ck!(img3.text_off != img.text_off); // it really did move with the header

    if !out_checks.is_null() {
        unsafe { *out_checks = n };
    }
    bad
}
