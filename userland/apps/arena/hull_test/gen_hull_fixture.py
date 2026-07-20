#!/usr/bin/env python3
# gen_hull_fixture.py - #491 Stage 2: a SYNTHETIC, copyright-clean GoldSrc BSP
# v30 fixture with a REAL PLANES + CLIPNODES + model-headnode[1] hull, for the
# offline known-answer hull-trace test (hull_test.c). Reuses bsp_test's proven
# minimal-face technique (one floor quad, no textures needed) so parse_inner()
# does not reject the scene as degenerate, then adds a hollow AABB room
# [-200,200] x [-200,200] x [0,200] as a 6-plane clipnode chain on HULL 1:
#   inside all 6 half-spaces -> CONTENTS_EMPTY (-1)
#   outside any one of them  -> CONTENTS_SOLID (-2)
# This is NOT a Minkowski-correct hull (real compilers shrink the room by the
# player half-extents) - it is a hand-built, self-consistent collision volume
# with KNOWN boundaries, good enough to prove the RECURSIVE TRACE ALGORITHM
# itself (split/backoff/start_solid/all_solid) against ground truth.
import struct, sys

def i32(v): return struct.pack('<i', v)

BSP_VERSION = 30
NUM_LUMPS = 15
L_ENTITIES, L_PLANES, L_TEXTURES, L_VERTEXES, L_VIS, L_NODES, L_TEXINFO, \
L_FACES, L_LIGHTING, L_CLIPNODES, L_LEAVES, L_MARKSURF, L_EDGES, L_SURFEDGES, \
L_MODELS = range(15)

# ---- one trivial floor quad so parse_inner() has >=1 face (not degenerate) ----
a, b, z0 = -256.0, 256.0, 0.0
V = [(a, a, z0), (b, a, z0), (b, b, z0), (a, b, z0)]
vert_bytes = b''.join(struct.pack('<3f', *v) for v in V)
edges = [(0, 1), (1, 2), (2, 3), (3, 0)]
edge_bytes = b''.join(struct.pack('<2H', e[0], e[1]) for e in edges)
surfedge_bytes = b''.join(struct.pack('<i', s) for s in range(4))
texinfo_bytes = struct.pack('<8f2i', 1, 0, 0, 0, 0, 1, 0, 0, -1, 0)  # miptex=-1: no texture
face_bytes = struct.pack('<HhihhBBBBi', 0, 0, 0, 4, 0, 0, 0, 0, 0, -1)

# ---- PLANES lump: 6 half-spaces of the room [-200,200]^2 x [0,200] ----
# dplane_t: normal[3] f32, dist f32, type i32.
ROOM = 200.0
def plane(nx, ny, nz, dist, ptype):
    return struct.pack('<4fi', nx, ny, nz, dist, ptype)

planes = b''
planes += plane(1, 0, 0, -ROOM, 0)   # P0: x >= -200            (fast axis type 0)
planes += plane(-1, 0, 0, -ROOM, 3)  # P1: -x >= -200 -> x<=200 (general: normal not +axis)
planes += plane(0, 1, 0, -ROOM, 1)   # P2: y >= -200            (fast axis type 1)
planes += plane(0, -1, 0, -ROOM, 3)  # P3: -y >= -200 -> y<=200 (general)
planes += plane(0, 0, 1, 0.0, 2)     # P4: z >= 0               (fast axis type 2)
planes += plane(0, 0, -1, -ROOM, 3)  # P5: -z >= -200 -> z<=200 (general)

# ---- CLIPNODES lump: a 6-node chain, "outside any plane -> SOLID(-2)",
# "inside all 6 -> EMPTY(-1)". dclipnode_t: planenum i32, children[2] i16.
CONTENTS_EMPTY = -1
CONTENTS_SOLID = -2
def clipnode(planenum, c0, c1):
    return struct.pack('<i2h', planenum, c0, c1)

clipnodes = b''
clipnodes += clipnode(0, 1, CONTENTS_SOLID)
clipnodes += clipnode(1, 2, CONTENTS_SOLID)
clipnodes += clipnode(2, 3, CONTENTS_SOLID)
clipnodes += clipnode(3, 4, CONTENTS_SOLID)
clipnodes += clipnode(4, 5, CONTENTS_SOLID)
clipnodes += clipnode(5, CONTENTS_EMPTY, CONTENTS_SOLID)

# ---- models lump: model 0, headnode[1] = 0 (our 6-node chain root) ----
model_bytes = struct.pack('<9f4i i2i',
    a, a, z0,  b, b, z0,  0, 0, 0,     # mins, maxs, origin (unused by the test)
    -1, 0, -1, -1,                     # headnode[hull0..3]: hull1 = our chain
    0,                                 # visleafs
    0, 1)                              # firstface, numfaces

ent = b'{\n"classname" "worldspawn"\n}\n\x00'

lumps = [b''] * NUM_LUMPS
lumps[L_ENTITIES]  = ent
lumps[L_PLANES]    = planes
lumps[L_VERTEXES]  = vert_bytes
lumps[L_TEXINFO]   = texinfo_bytes
lumps[L_FACES]     = face_bytes
lumps[L_CLIPNODES] = clipnodes
lumps[L_EDGES]     = edge_bytes
lumps[L_SURFEDGES] = surfedge_bytes
lumps[L_MODELS]    = model_bytes

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
path = sys.argv[1] if len(sys.argv) > 1 else 'hull_room.bsp'
with open(path, 'wb') as f:
    f.write(out)
print("WROTE", path, len(out), "bytes; room = [-200,200]^2 x [0,200], hull1 headnode=0")
