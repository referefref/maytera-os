// bsp.rs - MayteraOS Maytera Arena (Ring-3 userland) GoldSrc BSP v30 parser +
// WAD3 / embedded-miptex texture decoder, ALL IN RUST. Task #491 Stage 1.
//
// This module is compiled as part of the `arena_rs` staticlib (see arena_rs.rs
// `mod bsp;`) for the userland ABI (x86_64-unknown-none, code-model=large,
// relocation-model=static, panic=abort) and linked into the Arena ELF.
//
// ============================ SAFETY / THREAT MODEL ==========================
// A .bsp / .wad is UNTRUSTED FILE INPUT: it may be crafted or corrupt. The whole
// reason to write this in Rust is crash-safety: EVERY lump offset+length is
// bounds-checked against the file size, and EVERY edge / vertex / surfedge /
// texinfo / miptex index is bounds-checked against its lump's element count,
// using CHECKED arithmetic (checked_add / checked_mul) so nothing can wrap. A
// malformed file yields a clean error code (BspScene.error != 0), NEVER an
// out-of-bounds read/write. This mirrors the kernel port's #476 ext2 lesson.
//
// ============================ FLOAT-ABI CONSTRAINT ===========================
// The Stage 0 caveat (ARENA_BSP_PLAN.md): the precompiled `core` for
// x86_64-unknown-none is SOFT-float while the Arena C is -msse/-msse2 (hardware
// float). So this module does NO f32 arithmetic and passes NOTHING as f32 across
// the FFI. Vertex coordinates and texinfo projection vectors are read as raw
// little-endian 32-bit patterns (u32) and handed to C as u32; the C side
// reinterprets them as IEEE-754 float and does all UV math with hardware SSE.
// Palette-index -> ARGB is pure integer, so texture decode stays in Rust.

use alloc::boxed::Box;
use alloc::vec::Vec;

// ------------------------------------------------------------------ FFI types
// All #[repr(C)]; sizes are locked by const asserts below AND by _Static_assert
// in bsp_load.h so neither side can silently drift.

/// A world-space vertex. x/y/z are RAW IEEE-754 f32 bit patterns (little-endian
/// value), NOT floats computed in Rust. C reinterprets them as float.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct BspVec3 {
    pub x: u32,
    pub y: u32,
    pub z: u32,
}

/// One reconstructed face polygon. Its vertices are verts[first_vertex ..
/// first_vertex+num_vertices], already walked in winding order (surfedges ->
/// edges -> vertices). s_vec/t_vec are the texinfo vecs[0]/vecs[1] as raw f32
/// bit patterns (sx,sy,sz,soffset) / (tx,ty,tz,toffset); C computes per-vertex
/// UV = (dot(pos, s_vec.xyz) + s_vec.w) / tex_w  (and likewise for v). tex_id
/// indexes the textures[] array, or -1 if the face has no valid texture.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct BspFace {
    pub first_vertex: u32,
    pub num_vertices: u32,
    pub tex_id: i32,
    pub s_vec: [u32; 4],
    pub t_vec: [u32; 4],
    pub tex_w: u32,
    pub tex_h: u32,
}

/// A decoded texture. pixels live in the shared ARGB pool at pixels[pixel_offset
/// .. pixel_offset + width*height]. has_pixels == 0 means the texture referenced
/// an external WAD that was not supplied / not found (C falls back to a flat
/// colour); width/height are still valid for UV normalisation.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct BspTexture {
    pub width: u32,
    pub height: u32,
    pub pixel_offset: u32,
    pub has_pixels: u32,
}

/// #491 Stage 2: one BSP plane (dplane_t). normal/dist are raw f32 bit
/// patterns, exactly like BspVec3 elsewhere in this FFI; `ptype` is the
/// GoldSrc plane axis-type (0/1/2 = pure X/Y/Z axis-aligned fast path, >=3 =
/// general plane needing the full dot product). Consumed ONLY by the hull
/// trace (bsp_hull_trace); it never crosses back out to a C caller directly.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct BspPlane {
    pub normal: BspVec3,
    pub dist: u32,
    pub ptype: i32,
}

/// #491 Stage 2: one clipnode (dclipnode_t) from the CLIPNODES lump, widened
/// from the on-disk i16 children to i32 for a clean, unambiguous FFI type (no
/// odd 2-byte alignment to reason about on either side). A negative child is
/// a LEAF: its value is a CONTENTS_* constant (CONTENTS_SOLID = -2 blocks
/// movement; anything else is open space), not a further clipnode index.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct BspClipnode {
    pub planenum: i32,
    pub children: [i32; 2],
}

/// The parsed scene handed to C. On error, `error != 0` and all counts are 0.
#[repr(C)]
pub struct BspScene {
    pub verts: *mut BspVec3,
    pub faces: *mut BspFace,
    pub textures: *mut BspTexture,
    pub pixels: *mut u32,
    pub entities: *mut u8,
    pub num_verts: u32,
    pub num_faces: u32,
    pub num_textures: u32,
    pub num_pixels: u32,
    pub entities_len: u32,
    pub spawn: BspVec3,
    pub has_spawn: u32,
    pub error: i32,
    // #491 Stage 2 fields, appended at the END per the project's struct-layout
    // rule (never reorder/insert among existing fields). CLIPNODES + PLANES +
    // model 0's headnode[4] (one clipnode-tree root per hull: 0=point/unused
    // here, 1=standing player 32x32x72, 2=large 64x64x64, 3=crouch 32x32x36 -
    // these box sizes are FIXED GoldSrc/Quake engine constants, not stored in
    // the file; see bsp_hull_box()).
    pub planes: *mut BspPlane,
    pub clipnodes: *mut BspClipnode,
    pub num_planes: u32,
    pub num_clipnodes: u32,
    pub hull_headnode: [i32; 4],
    /// 1 if MODELS/CLIPNODES/PLANES were all present and non-empty (real
    /// collision data available); 0 means no usable clip hull was found (e.g.
    /// a degenerate/legacy file) and the C side must NOT enable real
    /// collision - it should stay in the Stage-1 noclip fallback rather than
    /// let gravity drop the player through an undefined world.
    pub hull_ok: u32,
}

// Lock the on-wire / FFI sizes at compile time (mirrored by _Static_assert in
// bsp_load.h). Any accidental field change breaks the build on BOTH sides.
const _: () = assert!(core::mem::size_of::<BspVec3>() == 12);
const _: () = assert!(core::mem::size_of::<BspFace>() == 52);
const _: () = assert!(core::mem::size_of::<BspTexture>() == 16);
const _: () = assert!(core::mem::size_of::<BspPlane>() == 20);
const _: () = assert!(core::mem::size_of::<BspClipnode>() == 12);
const _: () = assert!(core::mem::size_of::<BspScene>() == 128);

// Error codes (BspScene.error).
pub const BSP_OK: i32 = 0;
pub const BSP_E_TOO_SMALL: i32 = 1;
pub const BSP_E_BAD_VERSION: i32 = 2;
pub const BSP_E_LUMP_OOB: i32 = 3;
pub const BSP_E_INDEX_OOB: i32 = 4;
pub const BSP_E_TEX_OOB: i32 = 5;
pub const BSP_E_DEGENERATE: i32 = 6;

// The owning allocation. `view` is the FIRST field and #[repr(C)], so a
// `*mut BspScene` returned to C aliases `&owner.view`, and bsp_free can recover
// the whole owner by casting back. The Vec buffers stay alive here; C only reads
// through the raw pointers in `view`.
#[repr(C)]
struct SceneOwner {
    view: BspScene,
    verts: Vec<BspVec3>,
    faces: Vec<BspFace>,
    textures: Vec<BspTexture>,
    pixels: Vec<u32>,
    entities: Vec<u8>,
    planes: Vec<BspPlane>,        // #491 Stage 2
    clipnodes: Vec<BspClipnode>,  // #491 Stage 2
}

// -------------------------------------------------- little-endian byte readers
// Every reader is bounds-checked (slice::get returns None past the end), so a
// truncated file can never read out of bounds. f32s are read as their raw u32
// bit pattern (NO float arithmetic in this module).

#[inline]
fn rd_u16(d: &[u8], o: usize) -> Option<u16> {
    let b = d.get(o..o.checked_add(2)?)?;
    Some(u16::from_le_bytes([b[0], b[1]]))
}
#[inline]
fn rd_i16(d: &[u8], o: usize) -> Option<i16> {
    rd_u16(d, o).map(|v| v as i16)
}
#[inline]
fn rd_u32(d: &[u8], o: usize) -> Option<u32> {
    let b = d.get(o..o.checked_add(4)?)?;
    Some(u32::from_le_bytes([b[0], b[1], b[2], b[3]]))
}
#[inline]
fn rd_i32(d: &[u8], o: usize) -> Option<i32> {
    rd_u32(d, o).map(|v| v as i32)
}

// ----------------------------------------------------------------- BSP layout
const BSP_VERSION: i32 = 30;
const HEADER_LUMPS: usize = 15;
const LUMP_ENTITIES: usize = 0;
const LUMP_PLANES: usize = 1;    // #491 Stage 2: hull-trace collision
const LUMP_TEXTURES: usize = 2;
const LUMP_VERTEXES: usize = 3;
const LUMP_TEXINFO: usize = 6;
const LUMP_FACES: usize = 7;
const LUMP_CLIPNODES: usize = 9; // #491 Stage 2: hull-trace collision
const LUMP_EDGES: usize = 12;
const LUMP_SURFEDGES: usize = 13;
const LUMP_MODELS: usize = 14;

const VERTEX_SZ: usize = 12; // 3 * f32
const EDGE_SZ: usize = 4; // 2 * u16
const SURFEDGE_SZ: usize = 4; // i32
const FACE_SZ: usize = 20; // dface_t (GoldSrc)
const TEXINFO_SZ: usize = 40; // vecs[2][4] f32 + miptex i32 + flags i32
const MODEL_SZ: usize = 64; // dmodel_t (GoldSrc)
const MIPTEX_HDR: usize = 40; // name[16] + w,h (u32) + offsets[4] (u32)
const PLANE_SZ: usize = 20;      // dplane_t: normal[3] f32 + dist f32 + type i32
const CLIPNODE_SZ: usize = 8;    // dclipnode_t: planenum i32 + children[2] i16

// A sane cap on a single texture so a crafted (huge width*height) header cannot
// request a gigantic allocation. 4096x4096 texels is far beyond any real
// GoldSrc texture (which max at 512x512 / 1024x1024 tops).
const MAX_TEXELS: usize = 4096 * 4096;

#[derive(Clone, Copy, Default)]
struct Lump {
    ofs: usize,
    len: usize,
}

// Bounds-checked lump: reads (fileofs, filelen) and verifies both are
// non-negative and ofs+len is within the file.
fn read_lump(d: &[u8], idx: usize) -> Result<Lump, i32> {
    let base = 4 + idx * 8;
    let ofs = rd_i32(d, base).ok_or(BSP_E_LUMP_OOB)?;
    let len = rd_i32(d, base + 4).ok_or(BSP_E_LUMP_OOB)?;
    if ofs < 0 || len < 0 {
        return Err(BSP_E_LUMP_OOB);
    }
    let ofs = ofs as usize;
    let len = len as usize;
    let end = ofs.checked_add(len).ok_or(BSP_E_LUMP_OOB)?;
    if end > d.len() {
        return Err(BSP_E_LUMP_OOB);
    }
    Ok(Lump { ofs, len })
}

// ---------------------------------------------------------- texture decoding
// Decode one miptex (embedded in the BSP textures lump OR inside a WAD3 lump)
// into an ARGB (0xAARRGGBB, opaque) pixel Vec. `d` is the whole file buffer,
// `mt` is the byte offset of the miptex header within `d`. Returns None on any
// out-of-bounds / degenerate condition (caller records has_pixels = 0).
//
// PURE INTEGER: palette index -> ARGB is byte math only, no float.
fn decode_miptex(d: &[u8], mt: usize, width: usize, height: usize) -> Option<Vec<u32>> {
    if width == 0 || height == 0 {
        return None;
    }
    let texels = width.checked_mul(height)?;
    if texels > MAX_TEXELS {
        return None;
    }
    // offsets[4] follow name[16] + width(u32) + height(u32) at mt+24.
    let off0 = rd_u32(d, mt.checked_add(24)?)? as usize;
    let off3 = rd_u32(d, mt.checked_add(36)?)? as usize;
    if off0 == 0 {
        return None; // pixels live in an external WAD, not here
    }
    // mip0 pixel data
    let pix0 = mt.checked_add(off0)?;
    let pix_end = pix0.checked_add(texels)?;
    if pix_end > d.len() {
        return None;
    }
    // palette: after mip3 there is a 2-byte colour count (usually 256) then
    // 256*3 RGB bytes. mip3 dimensions are (width/8)*(height/8).
    let mip3_texels = (width / 8).checked_mul(height / 8)?;
    let pal_count_off = mt.checked_add(off3)?.checked_add(mip3_texels)?;
    let pal_off = pal_count_off.checked_add(2)?; // skip the u16 count
    let pal_end = pal_off.checked_add(256 * 3)?;
    if pal_end > d.len() {
        return None;
    }
    let src = &d[pix0..pix_end];
    let pal = &d[pal_off..pal_end];
    let mut out: Vec<u32> = Vec::new();
    out.try_reserve(texels).ok()?;
    for &idx in src {
        let p = (idx as usize) * 3; // idx is u8 -> max 255*3 = 765, +2 < 768
        let r = pal[p] as u32;
        let g = pal[p + 1] as u32;
        let b = pal[p + 2] as u32;
        out.push(0xFF00_0000u32 | (r << 16) | (g << 8) | b);
    }
    Some(out)
}

// ----------------------------------------------------------------- WAD3 lookup
// Compare a 16-byte fixed name field (null-terminated, case-insensitive) to a
// query name slice. Both are treated as ASCII.
fn name16_eq(field: &[u8], query: &[u8]) -> bool {
    let namelen = field.iter().position(|&c| c == 0).unwrap_or(field.len());
    let qlen = query.iter().position(|&c| c == 0).unwrap_or(query.len());
    if namelen != qlen {
        return false;
    }
    for i in 0..namelen {
        if field[i].to_ascii_uppercase() != query[i].to_ascii_uppercase() {
            return false;
        }
    }
    true
}

// Find a miptex named `name` in a WAD3 blob and decode its mip0 to ARGB, also
// returning (width, height). Returns None if the WAD is absent/malformed or the
// name is not present. Fully bounds-checked.
fn wad3_lookup(wad: &[u8], name: &[u8]) -> Option<(usize, usize, Vec<u32>)> {
    if wad.len() < 12 || &wad[0..4] != b"WAD3" {
        return None;
    }
    let numlumps = rd_i32(wad, 4)?;
    let diroffset = rd_i32(wad, 8)?;
    if numlumps < 0 || diroffset < 0 {
        return None;
    }
    let numlumps = numlumps as usize;
    let diroffset = diroffset as usize;
    const DIRENT: usize = 32; // filepos,disksize,size (i32*3) + type,cmp,pad[2] + name[16]
    for i in 0..numlumps {
        let e = diroffset.checked_add(i.checked_mul(DIRENT)?)?;
        let filepos = rd_i32(wad, e)?;
        if filepos < 0 {
            continue;
        }
        let filepos = filepos as usize;
        let field = wad.get(e.checked_add(16)?..e.checked_add(32)?)?;
        if !name16_eq(field, name) {
            continue;
        }
        // The lump at filepos is a miptex_t. Read its own width/height header.
        let width = rd_u32(wad, filepos.checked_add(16)?)? as usize;
        let height = rd_u32(wad, filepos.checked_add(20)?)? as usize;
        let px = decode_miptex(wad, filepos, width, height)?;
        return Some((width, height, px));
    }
    None
}

// ------------------------------------------------------------- the core parse
struct Parsed {
    verts: Vec<BspVec3>,
    faces: Vec<BspFace>,
    textures: Vec<BspTexture>,
    pixels: Vec<u32>,
    entities: Vec<u8>,
    planes: Vec<BspPlane>,        // #491 Stage 2
    clipnodes: Vec<BspClipnode>,  // #491 Stage 2
    hull_headnode: [i32; 4],      // #491 Stage 2
    hull_ok: u32,                 // #491 Stage 2
}

// #491 Stage 2: parse the CLIPNODES + PLANES lumps and model 0's headnode[4].
// This is COLLISION data, layered on top of the Stage 1 render parse and kept
// deliberately independent of it: any problem here (missing/short lumps, a
// corrupt model 0 header) yields None and the caller falls back to hull_ok=0
// rather than failing the whole scene parse - a map that fails to give us
// clip data should still RENDER (Stage 1 behaviour), it just can't collide.
// Every index is bounds-checked via slice::get()/the rd_* Option readers,
// consistent with the rest of this module's untrusted-input discipline.
struct HullData {
    planes: Vec<BspPlane>,
    clipnodes: Vec<BspClipnode>,
    headnode: [i32; 4],
}

fn parse_hull(d: &[u8]) -> Option<HullData> {
    let l_planes = read_lump(d, LUMP_PLANES).ok()?;
    let l_clip = read_lump(d, LUMP_CLIPNODES).ok()?;
    let l_model = read_lump(d, LUMP_MODELS).ok()?;
    let n_planes = l_planes.len / PLANE_SZ;
    let n_clip = l_clip.len / CLIPNODE_SZ;
    let n_model = l_model.len / MODEL_SZ;
    if n_planes == 0 || n_clip == 0 || n_model == 0 {
        return None; // no usable clip hull; caller falls back gracefully
    }

    let mut planes: Vec<BspPlane> = Vec::new();
    planes.try_reserve(n_planes).ok()?;
    for i in 0..n_planes {
        let o = l_planes.ofs + i * PLANE_SZ;
        let nx = rd_u32(d, o)?;
        let ny = rd_u32(d, o + 4)?;
        let nz = rd_u32(d, o + 8)?;
        let dist = rd_u32(d, o + 12)?;
        let ptype = rd_i32(d, o + 16)?;
        planes.push(BspPlane { normal: BspVec3 { x: nx, y: ny, z: nz }, dist, ptype });
    }

    let mut clipnodes: Vec<BspClipnode> = Vec::new();
    clipnodes.try_reserve(n_clip).ok()?;
    for i in 0..n_clip {
        let o = l_clip.ofs + i * CLIPNODE_SZ;
        let planenum = rd_i32(d, o)?;
        // on-disk children are i16; widen to i32 (sign preserved) so a leaf
        // CONTENTS_* value and a clipnode index share one unambiguous type.
        let c0 = rd_i16(d, o + 4)? as i32;
        let c1 = rd_i16(d, o + 6)? as i32;
        clipnodes.push(BspClipnode { planenum, children: [c0, c1] });
    }

    // dmodel_t: mins[3]+maxs[3]+origin[3] (f32, 36 bytes) then headnode[4]
    // (i32 each) at +36..+52. Model 0 (the static world) starts at the lump's
    // own base offset (index 0 needs no additional stride).
    let m0 = l_model.ofs;
    let mut headnode = [-1i32; 4];
    for (h, hn) in headnode.iter_mut().enumerate() {
        *hn = rd_i32(d, m0.checked_add(36 + h * 4)?)?;
    }

    Some(HullData { planes, clipnodes, headnode })
}

fn parse_inner(d: &[u8], wad: &[u8]) -> Result<Parsed, i32> {
    // Header: version(i32) + 15 * lump_t(i32 ofs, i32 len) = 124 bytes.
    if d.len() < 4 + HEADER_LUMPS * 8 {
        return Err(BSP_E_TOO_SMALL);
    }
    let version = rd_i32(d, 0).ok_or(BSP_E_TOO_SMALL)?;
    if version != BSP_VERSION {
        return Err(BSP_E_BAD_VERSION);
    }

    let l_ent = read_lump(d, LUMP_ENTITIES)?;
    let l_tex = read_lump(d, LUMP_TEXTURES)?;
    let l_vert = read_lump(d, LUMP_VERTEXES)?;
    let l_tinfo = read_lump(d, LUMP_TEXINFO)?;
    let l_face = read_lump(d, LUMP_FACES)?;
    let l_edge = read_lump(d, LUMP_EDGES)?;
    let l_surf = read_lump(d, LUMP_SURFEDGES)?;
    let l_model = read_lump(d, LUMP_MODELS)?;

    let n_vert = l_vert.len / VERTEX_SZ;
    let n_edge = l_edge.len / EDGE_SZ;
    let n_surf = l_surf.len / SURFEDGE_SZ;
    let n_tinfo = l_tinfo.len / TEXINFO_SZ;
    let n_face = l_face.len / FACE_SZ;
    let n_model = l_model.len / MODEL_SZ;

    // ---- decode all textures first (faces reference them by index) ---------
    let (textures, pixels) = parse_textures(d, wad, l_tex)?;
    let n_tex = textures.len();

    // ---- which faces to render: world model (model 0) if present ----------
    // model 0's firstface/numfaces select the static world geometry. Clamp to
    // the real face range so a crafted model can't push us out of bounds.
    let (first_face, num_face) = if n_model > 0 {
        let m0 = l_model.ofs; // model 0 header
        // dmodel_t: mins[3],maxs[3],origin[3] (f32) = 36 bytes; headnode[4] i32
        // = 16 (at +36..52); visleafs i32 (+52); firstface i32 (+56);
        // numfaces i32 (+60).
        let ff = rd_i32(d, m0.checked_add(56).ok_or(BSP_E_LUMP_OOB)?).ok_or(BSP_E_LUMP_OOB)?;
        let nf = rd_i32(d, m0.checked_add(60).ok_or(BSP_E_LUMP_OOB)?).ok_or(BSP_E_LUMP_OOB)?;
        if ff < 0 || nf < 0 {
            (0usize, n_face)
        } else {
            let ff = ff as usize;
            let nf = nf as usize;
            if ff >= n_face {
                (0usize, 0usize)
            } else {
                let avail = n_face - ff;
                (ff, if nf > avail { avail } else { nf })
            }
        }
    } else {
        (0usize, n_face)
    };

    let mut verts: Vec<BspVec3> = Vec::new();
    let mut faces: Vec<BspFace> = Vec::new();

    for fi in first_face..(first_face + num_face) {
        let f = l_face.ofs + fi * FACE_SZ; // in-range: fi < n_face
        // dface_t: planenum u16(+0), side i16(+2), firstedge i32(+4),
        // numedges i16(+8), texinfo i16(+10), styles[4](+12), lightofs i32(+16)
        let firstedge = rd_i32(d, f + 4).ok_or(BSP_E_LUMP_OOB)?;
        let numedges = rd_i16(d, f + 8).ok_or(BSP_E_LUMP_OOB)?;
        let texinfo = rd_i16(d, f + 10).ok_or(BSP_E_LUMP_OOB)?;
        if firstedge < 0 || numedges < 3 {
            continue; // degenerate face: skip (not fatal)
        }
        let firstedge = firstedge as usize;
        let numedges = numedges as usize;
        // surfedge range must be inside the surfedge lump
        let se_end = firstedge.checked_add(numedges).ok_or(BSP_E_INDEX_OOB)?;
        if se_end > n_surf {
            return Err(BSP_E_INDEX_OOB);
        }

        let base = verts.len() as u32;
        for k in 0..numedges {
            let se_off = l_surf.ofs + (firstedge + k) * SURFEDGE_SZ;
            let se = rd_i32(d, se_off).ok_or(BSP_E_INDEX_OOB)?;
            // surfedge >0 => edge forward (v0), <0 => edge reversed (v1)
            let (eidx, use_second) = if se >= 0 {
                (se as usize, false)
            } else {
                // -se; guard i32::MIN
                let m = se.checked_neg().ok_or(BSP_E_INDEX_OOB)? as usize;
                (m, true)
            };
            if eidx >= n_edge {
                return Err(BSP_E_INDEX_OOB);
            }
            let e_off = l_edge.ofs + eidx * EDGE_SZ;
            let v0 = rd_u16(d, e_off).ok_or(BSP_E_INDEX_OOB)? as usize;
            let v1 = rd_u16(d, e_off + 2).ok_or(BSP_E_INDEX_OOB)? as usize;
            let vi = if use_second { v1 } else { v0 };
            if vi >= n_vert {
                return Err(BSP_E_INDEX_OOB);
            }
            let v_off = l_vert.ofs + vi * VERTEX_SZ;
            // vertex coords copied as RAW f32 bit patterns (no float math)
            let vx = rd_u32(d, v_off).ok_or(BSP_E_INDEX_OOB)?;
            let vy = rd_u32(d, v_off + 4).ok_or(BSP_E_INDEX_OOB)?;
            let vz = rd_u32(d, v_off + 8).ok_or(BSP_E_INDEX_OOB)?;
            verts.push(BspVec3 { x: vx, y: vy, z: vz });
        }

        // resolve texinfo -> texture id + projection vectors (raw bit patterns)
        let mut s_vec = [0u32; 4];
        let mut t_vec = [0u32; 4];
        let mut tex_id: i32 = -1;
        let mut tex_w: u32 = 64;
        let mut tex_h: u32 = 64;
        if texinfo >= 0 && (texinfo as usize) < n_tinfo {
            let ti = l_tinfo.ofs + (texinfo as usize) * TEXINFO_SZ;
            for j in 0..4 {
                s_vec[j] = rd_u32(d, ti + j * 4).ok_or(BSP_E_INDEX_OOB)?;
                t_vec[j] = rd_u32(d, ti + 16 + j * 4).ok_or(BSP_E_INDEX_OOB)?;
            }
            let miptex = rd_i32(d, ti + 32).ok_or(BSP_E_INDEX_OOB)?;
            if miptex >= 0 && (miptex as usize) < n_tex {
                tex_id = miptex;
                tex_w = textures[miptex as usize].width;
                tex_h = textures[miptex as usize].height;
            }
        }
        if tex_w == 0 {
            tex_w = 64;
        }
        if tex_h == 0 {
            tex_h = 64;
        }

        faces.push(BspFace {
            first_vertex: base,
            num_vertices: numedges as u32,
            tex_id,
            s_vec,
            t_vec,
            tex_w,
            tex_h,
        });
    }

    if faces.is_empty() {
        return Err(BSP_E_DEGENERATE);
    }

    // entity text lump: exposed verbatim (Stage 3 will parse it; the C side
    // parses info_player_* spawn origin from it for this stage).
    let entities: Vec<u8> = d
        .get(l_ent.ofs..l_ent.ofs + l_ent.len)
        .ok_or(BSP_E_LUMP_OOB)?
        .to_vec();

    // #491 Stage 2: collision data is best-effort and independent of the
    // render-path success above (see parse_hull's doc comment).
    let (planes, clipnodes, hull_headnode, hull_ok) = match parse_hull(d) {
        Some(h) => (h.planes, h.clipnodes, h.headnode, 1u32),
        None => (Vec::new(), Vec::new(), [-1i32; 4], 0u32),
    };

    Ok(Parsed {
        verts,
        faces,
        textures,
        pixels,
        entities,
        planes,
        clipnodes,
        hull_headnode,
        hull_ok,
    })
}

// Parse the TEXTURES (miptex) lump: nummiptex + dataofs[], then each miptex.
// Embedded miptex are decoded in place; miptex whose mip0 offset is 0 are looked
// up by name in the supplied WAD3 blob. Everything is bounds-checked.
fn parse_textures(d: &[u8], wad: &[u8], l_tex: Lump) -> Result<(Vec<BspTexture>, Vec<u32>), i32> {
    let mut textures: Vec<BspTexture> = Vec::new();
    let mut pixels: Vec<u32> = Vec::new();
    if l_tex.len < 4 {
        return Ok((textures, pixels)); // no textures lump: allowed
    }
    let nummiptex = rd_i32(d, l_tex.ofs).ok_or(BSP_E_TEX_OOB)?;
    if nummiptex < 0 {
        return Err(BSP_E_TEX_OOB);
    }
    let nummiptex = nummiptex as usize;
    // dataofs table must fit in the lump.
    let table_end = 4usize
        .checked_add(nummiptex.checked_mul(4).ok_or(BSP_E_TEX_OOB)?)
        .ok_or(BSP_E_TEX_OOB)?;
    if table_end > l_tex.len {
        return Err(BSP_E_TEX_OOB);
    }

    for i in 0..nummiptex {
        let dofs = rd_i32(d, l_tex.ofs + 4 + i * 4).ok_or(BSP_E_TEX_OOB)?;
        // dataofs < 0 => texture entry present but has no data (rare); flat.
        if dofs < 0 {
            textures.push(BspTexture {
                width: 0,
                height: 0,
                pixel_offset: 0,
                has_pixels: 0,
            });
            continue;
        }
        let mt = l_tex.ofs.checked_add(dofs as usize).ok_or(BSP_E_TEX_OOB)?;
        // miptex header must fit
        if mt.checked_add(MIPTEX_HDR).ok_or(BSP_E_TEX_OOB)? > d.len() {
            return Err(BSP_E_TEX_OOB);
        }
        let name = &d[mt..mt + 16];
        let width = rd_u32(d, mt + 16).ok_or(BSP_E_TEX_OOB)? as usize;
        let height = rd_u32(d, mt + 20).ok_or(BSP_E_TEX_OOB)? as usize;
        let off0 = rd_u32(d, mt + 24).ok_or(BSP_E_TEX_OOB)? as usize;

        let (w, h, decoded): (usize, usize, Option<Vec<u32>>) = if off0 != 0 {
            // embedded pixels
            (width, height, decode_miptex(d, mt, width, height))
        } else {
            // external: look up by name in the WAD3
            match wad3_lookup(wad, name) {
                Some((ww, hh, px)) => (ww, hh, Some(px)),
                None => (width, height, None),
            }
        };

        match decoded {
            Some(px) => {
                let off = pixels.len() as u32;
                pixels.extend_from_slice(&px);
                textures.push(BspTexture {
                    width: w as u32,
                    height: h as u32,
                    pixel_offset: off,
                    has_pixels: 1,
                });
            }
            None => {
                textures.push(BspTexture {
                    width: w as u32,
                    height: h as u32,
                    pixel_offset: 0,
                    has_pixels: 0,
                });
            }
        }
    }
    Ok((textures, pixels))
}

// ------------------------------------------------------------------- FFI edge
fn empty_view(error: i32) -> BspScene {
    BspScene {
        verts: core::ptr::null_mut(),
        faces: core::ptr::null_mut(),
        textures: core::ptr::null_mut(),
        pixels: core::ptr::null_mut(),
        entities: core::ptr::null_mut(),
        num_verts: 0,
        num_faces: 0,
        num_textures: 0,
        num_pixels: 0,
        entities_len: 0,
        spawn: BspVec3 { x: 0, y: 0, z: 0 },
        has_spawn: 0,
        error,
        planes: core::ptr::null_mut(),
        clipnodes: core::ptr::null_mut(),
        num_planes: 0,
        num_clipnodes: 0,
        hull_headnode: [-1, -1, -1, -1],
        hull_ok: 0,
    }
}

/// Parse a GoldSrc BSP v30 map (and optionally an external WAD3 for textures)
/// entirely in Rust. Returns a heap-owned BspScene* the caller must release with
/// bsp_free(). On a malformed / truncated / hostile input this returns a scene
/// with `error != 0` and zero counts (NEVER an out-of-bounds access); it returns
/// null only if the initial owner allocation fails.
///
/// # Safety
/// `data`/`wad` must each point to `len`/`wad_len` valid, readable bytes for the
/// duration of the call (or be null with a 0 length). The returned pointer is
/// owned by Rust and must be freed exactly once with bsp_free().
#[no_mangle]
pub unsafe extern "C" fn bsp_parse(
    data: *const u8,
    len: usize,
    wad_data: *const u8,
    wad_len: usize,
) -> *mut BspScene {
    let d: &[u8] = if data.is_null() || len == 0 {
        &[]
    } else {
        core::slice::from_raw_parts(data, len)
    };
    let wad: &[u8] = if wad_data.is_null() || wad_len == 0 {
        &[]
    } else {
        core::slice::from_raw_parts(wad_data, wad_len)
    };

    let owner = match parse_inner(d, wad) {
        Ok(p) => {
            let hull_headnode = p.hull_headnode;
            let hull_ok = p.hull_ok;
            let mut o = Box::new(SceneOwner {
                view: empty_view(BSP_OK),
                verts: p.verts,
                faces: p.faces,
                textures: p.textures,
                pixels: p.pixels,
                entities: p.entities,
                planes: p.planes,
                clipnodes: p.clipnodes,
            });
            o.view.hull_headnode = hull_headnode;
            o.view.hull_ok = hull_ok;
            o
        }
        Err(code) => Box::new(SceneOwner {
            view: empty_view(code),
            verts: Vec::new(),
            faces: Vec::new(),
            textures: Vec::new(),
            pixels: Vec::new(),
            entities: Vec::new(),
            planes: Vec::new(),
            clipnodes: Vec::new(),
        }),
    };

    let mut owner = owner;
    // Fill the view's raw pointers/counts from the (now heap-stable) Vec buffers.
    owner.view.verts = owner.verts.as_mut_ptr();
    owner.view.num_verts = owner.verts.len() as u32;
    owner.view.faces = owner.faces.as_mut_ptr();
    owner.view.num_faces = owner.faces.len() as u32;
    owner.view.textures = owner.textures.as_mut_ptr();
    owner.view.num_textures = owner.textures.len() as u32;
    owner.view.pixels = owner.pixels.as_mut_ptr();
    owner.view.num_pixels = owner.pixels.len() as u32;
    owner.view.entities = owner.entities.as_mut_ptr();
    owner.view.entities_len = owner.entities.len() as u32;
    owner.view.planes = owner.planes.as_mut_ptr();
    owner.view.num_planes = owner.planes.len() as u32;
    owner.view.clipnodes = owner.clipnodes.as_mut_ptr();
    owner.view.num_clipnodes = owner.clipnodes.len() as u32;

    // view is the first field of the #[repr(C)] owner, so this pointer both
    // aliases &view AND lets bsp_free recover the owner by casting back.
    Box::into_raw(owner) as *mut BspScene
}

/// Release a scene returned by bsp_parse(). Null-safe. Double-free is a caller
/// bug (do not call twice).
///
/// # Safety
/// `scene` must be a pointer returned by bsp_parse() and not previously freed,
/// or null.
#[no_mangle]
pub unsafe extern "C" fn bsp_free(scene: *mut BspScene) {
    if scene.is_null() {
        return;
    }
    // view is the first field, so the BspScene* IS the SceneOwner*.
    let owner = Box::from_raw(scene as *mut SceneOwner);
    drop(owner);
}

// ============================================================================
// #491 Stage 2: GoldSrc/Quake recursive clipnode hull trace.
//
// This is the classic id Software algorithm (SV_RecursiveHullCheck /
// SV_HullPointContents from world.c in the Quake source release), unchanged
// in shape: walk the clipnode binary tree, split the segment at any plane it
// crosses, recurse the near side first, and when the far side of the split
// turns out solid, back the impact point off along the segment in small
// steps until a non-solid point is found. The DIST_EPSILON back-off (1/32
// unit, matching Arena's own world.c TRACE_EPS) is what stops the traced
// point landing EXACTLY on a surface, which is what causes sinking/jitter:
// without it, floating-point rounding can put the "safe" endpoint a hair on
// the solid side of the plane, so the very next frame's trace starts
// embedded again.
//
// All f32 arithmetic in this section is INTERNAL to this Rust module and
// never crosses the FFI as a float value (see the file header's float-ABI
// note) - only raw u32 bit patterns cross bsp_hull_trace()'s boundary, same
// discipline as the rest of this file.
//
// Crash-safety: every clipnode/plane index is bounds-checked via slice::get()
// (a corrupt CLIPNODES lump can encode arbitrary indices), and BOTH a
// recursion-depth cap and a shared total-node-visit budget guard against a
// crafted cyclic clipnode graph blowing the stack or spinning forever. Any
// budget/depth/index failure fails CLOSED (treated as solid) - better to
// falsely block a move for one frame than let a corrupt map either let the
// player fall through the world or crash the process.

const CONTENTS_SOLID: i32 = -2;
const DIST_EPSILON: f32 = 0.03125; // 1/32 unit; matches world.c's TRACE_EPS
const MAX_TRACE_DEPTH: u32 = 1024;
const MAX_TRACE_BUDGET: u32 = 200_000;

struct HullCtx<'a> {
    planes: &'a [BspPlane],
    clipnodes: &'a [BspClipnode],
    root: i32,
    budget: u32,
}

struct TraceState {
    all_solid: bool,
    start_solid: bool,
    frac: f32,
    end: [f32; 3],
    normal: [f32; 3],
}

#[inline]
fn plane_normal(p: &BspPlane) -> [f32; 3] {
    [f32::from_bits(p.normal.x), f32::from_bits(p.normal.y), f32::from_bits(p.normal.z)]
}
#[inline]
fn dot3(a: [f32; 3], b: [f32; 3]) -> f32 {
    a[0] * b[0] + a[1] * b[1] + a[2] * b[2]
}
#[inline]
fn plane_side_dist(p: &BspPlane, v: [f32; 3]) -> f32 {
    let dist = f32::from_bits(p.dist);
    if p.ptype >= 0 && p.ptype < 3 {
        v[p.ptype as usize] - dist
    } else {
        dot3(plane_normal(p), v) - dist
    }
}

/// Fetch clipnode `num`'s plane, bounds-checked against both the clipnode and
/// plane arrays. None means "treat as solid" to the caller (fail closed).
#[inline]
fn node_plane<'a>(ctx: &HullCtx<'a>, num: i32) -> Option<(&'a BspClipnode, &'a BspPlane)> {
    let node = ctx.clipnodes.get(num as usize)?;
    if node.planenum < 0 {
        return None;
    }
    let plane = ctx.planes.get(node.planenum as usize)?;
    Some((node, plane))
}

/// Walk down the clipnode tree from `num` to the leaf containing point `p`,
/// no splitting (pure point classification). Returns a CONTENTS_* value; any
/// bounds failure or budget exhaustion returns CONTENTS_SOLID (fail closed).
fn hull_point_contents(ctx: &mut HullCtx, mut num: i32, p: [f32; 3]) -> i32 {
    while num >= 0 {
        if ctx.budget == 0 {
            return CONTENTS_SOLID;
        }
        ctx.budget -= 1;
        let (node, plane) = match node_plane(ctx, num) {
            Some(np) => np,
            None => return CONTENTS_SOLID,
        };
        let d = plane_side_dist(plane, p);
        num = if d < 0.0 { node.children[1] } else { node.children[0] };
    }
    num
}

/// The classic SV_RecursiveHullCheck. Returns true while the segment is still
/// entirely in empty space (no impact resolved yet); false once a definitive
/// result is in `st` (either a solid impact was found, or the depth/budget
/// guard tripped, which is reported as all_solid so the caller fails closed).
#[allow(clippy::too_many_arguments)]
fn recursive_hull_check(
    ctx: &mut HullCtx,
    num: i32,
    p1f: f32,
    p2f: f32,
    p1: [f32; 3],
    p2: [f32; 3],
    depth: u32,
    st: &mut TraceState,
) -> bool {
    if depth > MAX_TRACE_DEPTH || ctx.budget == 0 {
        st.all_solid = true;
        return false;
    }
    ctx.budget -= 1;

    if num < 0 {
        if num != CONTENTS_SOLID {
            st.all_solid = false;
        } else {
            st.start_solid = true;
        }
        return true; // empty leaf (or a solid leaf the caller already handles)
    }

    let (node, plane) = match node_plane(ctx, num) {
        Some(np) => np,
        None => {
            st.all_solid = true;
            return false;
        }
    };

    let t1 = plane_side_dist(plane, p1);
    let t2 = plane_side_dist(plane, p2);

    if t1 >= 0.0 && t2 >= 0.0 {
        return recursive_hull_check(ctx, node.children[0], p1f, p2f, p1, p2, depth + 1, st);
    }
    if t1 < 0.0 && t2 < 0.0 {
        return recursive_hull_check(ctx, node.children[1], p1f, p2f, p1, p2, depth + 1, st);
    }

    // segment straddles this plane: split at the crossing, nudged by
    // DIST_EPSILON toward the near side. This is the "leave a small gap"
    // guarantee: the split point never lands EXACTLY on the plane.
    let denom = t1 - t2;
    let mut frac = if t1 < 0.0 { (t1 + DIST_EPSILON) / denom } else { (t1 - DIST_EPSILON) / denom };
    if frac < 0.0 {
        frac = 0.0;
    }
    if frac > 1.0 {
        frac = 1.0;
    }

    let mut midf = p1f + (p2f - p1f) * frac;
    let mut mid = [
        p1[0] + frac * (p2[0] - p1[0]),
        p1[1] + frac * (p2[1] - p1[1]),
        p1[2] + frac * (p2[2] - p1[2]),
    ];

    let side = if t1 < 0.0 { 1usize } else { 0usize };

    if !recursive_hull_check(ctx, node.children[side], p1f, midf, p1, mid, depth + 1, st) {
        return false;
    }

    if hull_point_contents(ctx, node.children[side ^ 1], mid) != CONTENTS_SOLID {
        return recursive_hull_check(ctx, node.children[side ^ 1], midf, p2f, mid, p2, depth + 1, st);
    }

    if st.all_solid {
        return false;
    }

    // The far side of the crossing is solid: this plane is the impact plane.
    st.normal = plane_normal(plane);

    // Back off along the segment in 0.1-fraction steps (matching the
    // reference algorithm exactly) until a non-solid point is found. This is
    // what prevents the reported stop point sitting inside solid geometry.
    let mut f = frac;
    loop {
        if hull_point_contents(ctx, ctx.root, mid) != CONTENTS_SOLID {
            break;
        }
        f -= 0.1;
        if f < 0.0 {
            st.frac = midf;
            st.end = mid;
            return false;
        }
        midf = p1f + (p2f - p1f) * f;
        mid = [
            p1[0] + f * (p2[0] - p1[0]),
            p1[1] + f * (p2[1] - p1[1]),
            p1[2] + f * (p2[2] - p1[2]),
        ];
    }
    st.frac = midf;
    st.end = mid;
    false
}

/// #491 Stage 2 FFI result of one hull trace. frac/end/normal are RAW f32 bit
/// patterns (same integer-only-FFI discipline as the rest of this file).
#[repr(C)]
pub struct HullTrace {
    pub frac: u32,
    pub end: BspVec3,
    pub normal: BspVec3,
    pub start_solid: u32,
    pub all_solid: u32,
}
const _: () = assert!(core::mem::size_of::<HullTrace>() == 36);

/// Sweep a POINT through the active BSP map's clip hull from p1 to p2 (both
/// raw f32 bit patterns, already in HULL-ORIGIN space - see bsp_hull_box();
/// the caller (C) does the feet<->origin conversion in hardware float, per
/// the Stage-0/1 integer-only-FFI rule, this function does no FFI-crossing
/// float math). `hull` selects which of model 0's headnode[4] roots to trace
/// (0..3; Arena uses hull 1, the standing-player box - see bsp_load.c).
/// Returns 0 on success (result written to *out); a negative code means no
/// usable clip data (missing/degenerate CLIPNODES+PLANES+MODELS, or a bad
/// hull index) and the caller must NOT trust *out.
///
/// # Safety
/// `scene` must be a live pointer returned by bsp_parse() and not yet freed.
/// `out` must point to a valid, writable HullTrace.
#[no_mangle]
pub unsafe extern "C" fn bsp_hull_trace(
    scene: *const BspScene,
    hull: i32,
    p1x: u32,
    p1y: u32,
    p1z: u32,
    p2x: u32,
    p2y: u32,
    p2z: u32,
    out: *mut HullTrace,
) -> i32 {
    if scene.is_null() || out.is_null() {
        return -1;
    }
    let sc = &*scene;
    if sc.hull_ok == 0 || sc.num_clipnodes == 0 || sc.num_planes == 0 {
        return -2;
    }
    if !(0..4).contains(&hull) {
        return -3;
    }
    let root = sc.hull_headnode[hull as usize];

    let planes: &[BspPlane] = if sc.planes.is_null() || sc.num_planes == 0 {
        &[]
    } else {
        core::slice::from_raw_parts(sc.planes, sc.num_planes as usize)
    };
    let clipnodes: &[BspClipnode] = if sc.clipnodes.is_null() || sc.num_clipnodes == 0 {
        &[]
    } else {
        core::slice::from_raw_parts(sc.clipnodes, sc.num_clipnodes as usize)
    };

    let p1 = [f32::from_bits(p1x), f32::from_bits(p1y), f32::from_bits(p1z)];
    let p2 = [f32::from_bits(p2x), f32::from_bits(p2y), f32::from_bits(p2z)];

    let mut ctx = HullCtx { planes, clipnodes, root, budget: MAX_TRACE_BUDGET };
    let mut st = TraceState { all_solid: true, start_solid: false, frac: 1.0, end: p2, normal: [0.0, 0.0, 0.0] };

    let _ = recursive_hull_check(&mut ctx, root, 0.0, 1.0, p1, p2, 0, &mut st);

    if st.all_solid {
        // Never leave the caller with an undefined result: an entity that is
        // (or becomes, via the depth/budget guard) fully embedded is reported
        // as an immediate, zero-fraction block at the start point.
        st.frac = 0.0;
        st.end = p1;
    }

    (*out).frac = st.frac.to_bits();
    (*out).end = BspVec3 { x: st.end[0].to_bits(), y: st.end[1].to_bits(), z: st.end[2].to_bits() };
    (*out).normal = BspVec3 { x: st.normal[0].to_bits(), y: st.normal[1].to_bits(), z: st.normal[2].to_bits() };
    (*out).start_solid = st.start_solid as u32;
    (*out).all_solid = st.all_solid as u32;
    0
}

/// The FIXED, engine-standard hull box half-extents for hulls 0..3. These are
/// NOT stored in the BSP file - they are GoldSrc/Quake compile-time constants
/// the map compiler used to generate each hull's CLIPNODES, so the trace and
/// the box size must agree by convention exactly as the original engine's
/// hull_t table does:
///   hull 0 = point (uses the main BSP nodes, not clipnodes; Arena never
///            traces hull 0, so this returns a degenerate zero box)
///   hull 1 = standing player, 32x32x72  (mins -16,-16,-36 / maxs 16,16,36)
///   hull 2 = large/duck-through-able,   64x64x64
///   hull 3 = crouching player, 32x32x36 (mins -16,-16,-18 / maxs 16,16,18)
/// Returns 0 on success, -1 for an out-of-range hull index.
#[no_mangle]
pub extern "C" fn bsp_hull_box(hull: i32, out_mins: *mut BspVec3, out_maxs: *mut BspVec3) -> i32 {
    let (mn, mx): ([f32; 3], [f32; 3]) = match hull {
        0 => ([0.0, 0.0, 0.0], [0.0, 0.0, 0.0]),
        1 => ([-16.0, -16.0, -36.0], [16.0, 16.0, 36.0]),
        2 => ([-32.0, -32.0, -32.0], [32.0, 32.0, 32.0]),
        3 => ([-16.0, -16.0, -18.0], [16.0, 16.0, 18.0]),
        _ => return -1,
    };
    let mnb = BspVec3 { x: mn[0].to_bits(), y: mn[1].to_bits(), z: mn[2].to_bits() };
    let mxb = BspVec3 { x: mx[0].to_bits(), y: mx[1].to_bits(), z: mx[2].to_bits() };
    unsafe {
        if !out_mins.is_null() {
            *out_mins = mnb;
        }
        if !out_maxs.is_null() {
            *out_maxs = mxb;
        }
    }
    0
}
