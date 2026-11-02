#!/usr/bin/env python3
"""
nv162png: YUV422 semi-planar (Y plane, then interleaved chroma plane at
w * h) to PNG, BT.601 full range (JFIF, what both gtelwifi camera sensors
deliver; --limited for 16..235), stdlib only. The DCAM path writes this
layout; whether the chroma pairs are U,V (NV16) or V,U (NV61) is decided by
looking at the picture: --vu swaps them.

  tools/nv162png.py in.nv16 out.png [width height] [--vu] [--nv12] [--limited]

--nv12 reads 4:2:0 (chroma plane of w * h / 2, one chroma line per two).
"""

import struct
import sys
import zlib


def clamp(v):
    return 0 if v < 0 else 255 if v > 255 else int(v)


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    vu = '--vu' in sys.argv
    nv12 = '--nv12' in sys.argv
    limited = '--limited' in sys.argv
    src, dst = args[0], args[1]
    w, h = (int(args[2]), int(args[3])) if len(args) > 3 else (800, 600)
    d = open(src, 'rb').read()
    y, c = d[:w * h], d[w * h:]
    rows = []
    for j in range(h):
        row = bytearray([0])
        for i in range(w):
            yy = y[j * w + i] - (16 if limited else 0)
            k = (j // 2 if nv12 else j) * w + (i & ~1)
            u, v = c[k] - 128, c[k + 1] - 128
            if vu:
                u, v = v, u
            if limited:
                row += bytes((clamp(1.164 * yy + 1.596 * v),
                              clamp(1.164 * yy - 0.392 * u - 0.813 * v),
                              clamp(1.164 * yy + 2.017 * u)))
            else:
                row += bytes((clamp(yy + 1.402 * v),
                              clamp(yy - 0.344 * u - 0.714 * v),
                              clamp(yy + 1.772 * u)))
        rows.append(bytes(row))

    def chunk(t, b):
        return struct.pack('>I', len(b)) + t + b + struct.pack('>I', zlib.crc32(t + b))

    png = b'\x89PNG\r\n\x1a\n'
    png += chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0))
    png += chunk(b'IDAT', zlib.compress(b''.join(rows), 6))
    png += chunk(b'IEND', b'')
    open(dst, 'wb').write(png)


if __name__ == '__main__':
    main()
