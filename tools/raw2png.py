#!/usr/bin/env python3
"""Convert a raw 800x1280 XRGB8888 framebuffer dump to PNG (stdlib only)."""
import struct
import sys
import zlib

W, H = 800, 1280


def chunk(tag, data):
    return (struct.pack(">I", len(data)) + tag + data +
            struct.pack(">I", zlib.crc32(tag + data) & 0xffffffff))


def main():
    src, dst = sys.argv[1], sys.argv[2]
    raw = open(src, "rb").read()[:W * H * 4]
    rows = []
    for y in range(H):
        line = raw[y * W * 4:(y + 1) * W * 4]
        # little-endian XRGB8888 in memory: B G R X
        rgb = bytearray(W * 3)
        rgb[0::3] = line[2::4]
        rgb[1::3] = line[1::4]
        rgb[2::3] = line[0::4]
        rows.append(b"\0" + bytes(rgb))
    png = (b"\x89PNG\r\n\x1a\n" +
           chunk(b"IHDR", struct.pack(">IIBBBBB", W, H, 8, 2, 0, 0, 0)) +
           chunk(b"IDAT", zlib.compress(b"".join(rows), 6)) +
           chunk(b"IEND", b""))
    open(dst, "wb").write(png)


if __name__ == "__main__":
    main()
