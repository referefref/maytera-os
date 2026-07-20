#!/usr/bin/env python3
# gen_fixture.py - build a SYNTHETIC, copyright-clean GoldSrc BSP v30 fixture for
# the #491 Stage 1 offline parser test. A small textured cube room (6 quad faces,
# one embedded 16x16 miptex + palette, an info_player_start spawn). Emits room.bsp
# and prints the known-answer values the harness asserts.
import struct, sys

def i32(v): return struct.pack('<i', v)
def u32(v): return struct.pack('<I', v)

BSP_VERSION = 30
NUM_LUMPS = 15
L_ENTITIES, L_PLANES, L_TEXTURES, L_VERTEXES, L_VIS, L_NODES, L_TEXINFO, \
L_FACES, L_LIGHTING, L_CLIPNODES, L_LEAVES, L_MARKSURF, L_EDGES, L_SURFEDGES, \
L_MODELS = range(15)

# ---- geometry: a room from (-256,-256,0) to (256,256,256), z up ----
a, b, z0, z1 = -256.0, 256.0, 0.0, 256.0
V = [
    (a, a, z0), (b, a, z0), (b, b, z0), (a, b, z0),   # 0..3 floor
    (a, a, z1), (b, a, z1), (b, b, z1), (a, b, z1),   # 4..7 ceiling
]
# 6 faces as CCW-ish vertex loops (winding irrelevant: draw disables cull)
FACES_V = [
    [0, 1, 2, 3],   # floor  z0
    [4, 7, 6, 5],   # ceil   z1
    [0, 4, 5, 1],   # wall -Y
    [3, 2, 6, 7],   # wall +Y
    [0, 3, 7, 4],   # wall -X
    [1, 5, 6, 2],   # wall +X
]

# vertexes lump
vert_bytes = b''.join(struct.pack('<3f', *v) for v in V)

# edges + surfedges: per face, 4 edges (consecutive vertex pairs), forward surfedges
edges = []
surfedges = []
for loop in FACES_V:
    for k in range(4):
        va = loop[k]
        vb = loop[(k + 1) % 4]
        surfedges.append(len(edges))    # positive => forward (use v[0])
        edges.append((va, vb))
edge_bytes = b''.join(struct.pack('<2H', e[0], e[1]) for e in edges)
surfedge_bytes = b''.join(struct.pack('<i', s) for s in surfedges)

# texinfo: one entry, s=(1,0,0,0) t=(0,1,0,0), miptex 0, flags 0
texinfo_bytes = struct.pack('<8f2i', 1,0,0,0,  0,1,0,0,  0, 0)

# faces lump: dface_t = planenum(H) side(h) firstedge(i) numedges(h) texinfo(h) styles[4] lightofs(i)
face_bytes = b''
for fi, loop in enumerate(FACES_V):
    firstedge = fi * 4
    face_bytes += struct.pack('<HhihhBBBBi', 0, 0, firstedge, 4, 0, 0,0,0,0, -1)

# ---- textures lump: nummiptex + dataofs[] + one embedded miptex ----
TW, TH = 16, 16
# mip0 checker of palette idx 1 / idx 2
mip0 = bytearray(TW * TH)
for y in range(TH):
    for x in range(TW):
        mip0[y*TW + x] = 1 if (((x >> 2) + (y >> 2)) & 1) else 2
mip1 = bytes((TW//2)*(TH//2))
mip2 = bytes((TW//4)*(TH//4))
mip3 = bytes((TW//8)*(TH//8))
palette = bytearray(256*3)
palette[1*3:1*3+3] = bytes((200, 40, 40))    # idx1 -> ARGB 0xFFC82828
palette[2*3:2*3+3] = bytes((40, 60, 200))    # idx2 -> ARGB 0xFF283CC8

name = b'TESTTEX'.ljust(16, b'\x00')
off0 = 40
off1 = off0 + len(mip0)
off2 = off1 + len(mip1)
off3 = off2 + len(mip2)
# after mip3: 2-byte colour count, then palette
miptex = name + struct.pack('<2I', TW, TH) + struct.pack('<4I', off0, off1, off2, off3)
miptex += bytes(mip0) + mip1 + mip2 + mip3 + struct.pack('<H', 256) + bytes(palette)

nummiptex = 1
dataofs0 = 4 + 4 * nummiptex   # miptex starts right after the offset table
tex_bytes = struct.pack('<i', nummiptex) + struct.pack('<i', dataofs0) + miptex

# ---- models lump: model 0 covers all 6 faces ----
model_bytes = struct.pack('<9f7i',
    a, a, z0,  b, b, z1,  0,0,0,     # mins, maxs, origin
    0, -1, -1, -1,                   # headnode[4]
    0,                               # visleafs
    0, len(FACES_V))                 # firstface, numfaces

# ---- entities lump ----
ent = (b'{\n"classname" "worldspawn"\n"wad" "/ARENA/MAP.WAD"\n}\n'
       b'{\n"classname" "info_player_start"\n"origin" "0 0 64"\n}\n\x00')

# ---- assemble file: header (124 bytes) + lumps ----
lumps = [b''] * NUM_LUMPS
lumps[L_ENTITIES]  = ent
lumps[L_TEXTURES]  = tex_bytes
lumps[L_VERTEXES]  = vert_bytes
lumps[L_TEXINFO]   = texinfo_bytes
lumps[L_FACES]     = face_bytes
lumps[L_EDGES]     = edge_bytes
lumps[L_SURFEDGES] = surfedge_bytes
lumps[L_MODELS]    = model_bytes
# PLANES/VIS/NODES/LIGHTING/CLIPNODES/LEAVES/MARKSURF stay empty (parser skips)

header_size = 4 + NUM_LUMPS * 8
body = b''
dir_entries = []
ofs = header_size
for i in range(NUM_LUMPS):
    data = lumps[i]
    dir_entries.append((ofs if data else header_size, len(data)))
    body += data
    ofs += len(data)

header = i32(BSP_VERSION)
for (o, l) in dir_entries:
    header += i32(o) + i32(l)

out = header + body
path = sys.argv[1] if len(sys.argv) > 1 else 'room.bsp'
with open(path, 'wb') as f:
    f.write(out)

# ---- known answers (harness mirrors these) ----
print("WROTE", path, len(out), "bytes")
print("EXPECT num_verts=24 num_faces=6 num_textures=1")
print("EXPECT tex0 w=16 h=16")
print("EXPECT face0 verts (floor loop) = (-256,-256,0)(256,-256,0)(256,256,0)(-256,256,0)")
print("EXPECT face0 v0 uv = (-16, -16)   [x/16, y/16]")
print("EXPECT pixel[0]=0xFF283CC8  pixel[4]=0xFFC82828  (idx2, idx1)")
print("EXPECT spawn origin (0,0,64)")
