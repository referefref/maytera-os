#!/usr/bin/env python3
# gen_side1_fixture.py - #568 regression fixture for the missing side==1 plane
# normal negation in bsp.rs recursive_hull_check().
#
# hull_test.c / dust2_test.c's existing fixtures never exercise this bug: the
# sealed-room chain (gen_hull_fixture.py) always stores its solid leaf on
# children[1] with the player starting on the "positive" side of every node
# in the chain, so the impact side is ALWAYS 0. de_dust2's own single tested
# spawn point happens to land on a side==0 floor impact too. Neither is
# proof the algorithm is right in general.
#
# This fixture builds ONE clipnode representing a perfectly ordinary floor at
# z=0 (solid below, open air above) - but stores the split plane FACING DOWN
# (normal=(0,0,-1)) instead of up, with the children reversed to match, which
# is exactly the kind of plane-orientation choice a real qbsp/hlbsp compile
# can make for any given internal split (the compiler does not promise every
# clip plane's normal points "out of solid"; only the FINAL reported trace
# normal is contractually meaningful, which is precisely what this proves).
# Physically this is the SAME floor a "normal" plane(0,0,1) fixture would
# describe; only the algebraic encoding differs. A correct hull trace must
# report the SAME real-world outward normal (0,0,1) for a straight fall onto
# it either way. The pre-fix code returned (0,0,-1) here (backwards): fed into
# world.c's `best_n.z > FLOOR_NORMAL_Z` ground check, a real floor stored this
# way in a real map would NEVER register as ground, and the player would keep
# falling into/through it every frame gravity is re-applied - which is
# indistinguishable, from real hardware, from "flying through the floor at
# this particular elevation".
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
texinfo_bytes = struct.pack('<8f2i', 1, 0, 0, 0, 0, 1, 0, 0, -1, 0)
face_bytes = struct.pack('<HhihhBBBBi', 0, 0, 0, 4, 0, 0, 0, 0, 0, -1)

# ---- PLANES lump: ONE plane, a floor at z=0 stored FACING DOWN -------------
# dplane_t: normal[3] f32, dist f32, type i32. type=3 (>=3: general, forces
# the dot-product path rather than the ptype<3 fast-axis path - see
# plane_side_dist in bsp.rs, and matches gen_hull_fixture.py's own convention
# for a negative-facing axis plane, e.g. its P1/P3/P5).
planes = struct.pack('<4fi', 0.0, 0.0, -1.0, 0.0, 3)   # normal=(0,0,-1), dist=0

# ---- CLIPNODES lump: ONE node. children REVERSED vs. the "natural" floor:
# children[0] = CONTENTS_SOLID (the "below z=0" side, since the plane faces
#               down: distance = -z, so z<0 -> distance>0 -> children[0])
# children[1] = CONTENTS_EMPTY (the "above z=0" side: z>0 -> distance<0)
CONTENTS_EMPTY = -1
CONTENTS_SOLID = -2
clipnodes = struct.pack('<i2h', 0, CONTENTS_SOLID, CONTENTS_EMPTY)

# ---- models lump: model 0, headnode[1] = 0 (our single node) ----
model_bytes = struct.pack('<9f4i i2i',
    a, a, z0,  b, b, z0,  0, 0, 0,     # mins, maxs, origin (unused by the test)
    -1, 0, -1, -1,                     # headnode[hull0..3]: hull1 = our node
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
path = sys.argv[1] if len(sys.argv) > 1 else 'side1_floor.bsp'
with open(path, 'wb') as f:
    f.write(out)
print("WROTE", path, len(out), "bytes; single down-facing floor plane at z=0, "
      "children reversed so the impact side==1")
