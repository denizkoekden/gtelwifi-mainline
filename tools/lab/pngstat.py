import sys, zlib, struct
d = open(sys.argv[1], 'rb').read()
assert d[:8] == b'\x89PNG\r\n\x1a\n'
pos, idat, w = 8, b'', 0
while pos < len(d):
    n, t = struct.unpack('>I4s', d[pos:pos + 8]); body = d[pos + 8:pos + 8 + n]; pos += 12 + n
    if t == b'IHDR':
        w, h, bd, ct = struct.unpack('>IIBB', body[:10])
    elif t == b'IDAT':
        idat += body
raw = zlib.decompress(idat)
bpp = {6: 4, 2: 3}[ct]
stride = w * bpp
prev = bytearray(stride)
nonblack = alpha0 = total = 0
i = 0
for y in range(h):
    f = raw[i]; line = bytearray(raw[i + 1:i + 1 + stride]); i += 1 + stride
    for x in range(stride):
        a = line[x - bpp] if x >= bpp else 0
        b = prev[x]; c = prev[x - bpp] if x >= bpp else 0
        if f == 1: line[x] = (line[x] + a) & 255
        elif f == 2: line[x] = (line[x] + b) & 255
        elif f == 3: line[x] = (line[x] + (a + b) // 2) & 255
        elif f == 4:
            p = a + b - c; pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
            line[x] = (line[x] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
    prev = line
    if y % 8 == 0:
        for x in range(0, w, 8):
            px = line[x * bpp:(x + 1) * bpp]; total += 1
            if any(px[:3]): nonblack += 1
            if bpp == 4 and px[3] == 0: alpha0 += 1
print(f"{w}x{h} colortype {ct}: sampled {total}, non-black {nonblack}, alpha 0: {alpha0}")
