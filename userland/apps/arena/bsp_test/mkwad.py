import struct, sys

def read_wad(path):
    d = open(path,'rb').read()
    assert d[0:4] == b'WAD3', (path, d[0:4])
    numlumps, diroffset = struct.unpack('<ii', d[4:12])
    out = {}
    for i in range(numlumps):
        e = diroffset + i*32
        filepos, disksize, size = struct.unpack('<iii', d[e:e+12])
        typ, comp = d[e+12], d[e+13]
        name = d[e+16:e+32].split(b'\0')[0]
        out[name.upper()] = (d[filepos:filepos+disksize], typ, comp, size, name)
    return out

bsp = open(sys.argv[1],'rb').read()
lumps = [struct.unpack('<ii', bsp[4+i*8:12+i*8]) for i in range(15)]
tofs, tlen = lumps[2]
nummiptex = struct.unpack('<i', bsp[tofs:tofs+4])[0]
need = []
for i in range(nummiptex):
    ofs = struct.unpack('<i', bsp[tofs+4+i*4:tofs+8+i*4])[0]
    mt = tofs + ofs
    name = bsp[mt:mt+16].split(b'\0')[0]
    mip0 = struct.unpack('<I', bsp[mt+24:mt+28])[0]
    if mip0 == 0:
        need.append(name)

srcs = [read_wad(p) for p in sys.argv[3:]]
picked, missing = [], []
for n in need:
    for s in srcs:
        if n.upper() in s:
            picked.append((n, s[n.upper()]))
            break
    else:
        missing.append(n.decode('latin1'))

# Build WAD3: header(12) + lump data + directory
blob = bytearray(b'WAD3' + struct.pack('<ii', 0, 0))
dirents = []
for name, (data, typ, comp, size, origname) in picked:
    filepos = len(blob)
    blob += data
    while len(blob) % 4: blob += b'\0'   # 4-byte align
    dirents.append((filepos, len(data), size, typ, comp, origname))
diroffset = len(blob)
for filepos, disksize, size, typ, comp, origname in dirents:
    blob += struct.pack('<iii', filepos, disksize, size)
    blob += bytes([typ, comp, 0, 0])
    blob += origname[:16].ljust(16, b'\0')
blob[4:12] = struct.pack('<ii', len(dirents), diroffset)
open(sys.argv[2],'wb').write(blob)
print("needed from WAD : %d" % len(need))
print("packed          : %d" % len(picked))
print("MISSING         : %s" % (missing if missing else "none"))
print("output          : %s (%d bytes)" % (sys.argv[2], len(blob)))
