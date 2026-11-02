import struct, sys
def dec(fn):
    d = open(fn, 'rb').read()
    hi = lo = 0; out = []
    for i in range(0, len(d), 4):
        r, v = struct.unpack_from('<HH', d, i)
        if r == 0x0028: hi = v
        elif r == 0x002a: lo = v
        elif r == 0x0f12:
            out.append('%04x%04x=%04x' % (hi, lo, v)); lo += 2
        elif r == 0xffff: out.append('sleep %d' % v)
        else: out.append('R%04x=%04x' % (r, v))
    return out
for fn in sys.argv[1:]:
    print(fn.split('/')[-1], ' '.join(dec(fn)))
