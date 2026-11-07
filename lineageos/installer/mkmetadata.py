#!/usr/bin/env python3
"""
Build metadata.img: an empty ext4 for HIDDEN (/metadata) as an Android sparse
image, for the install zip (update-binary.c writes it instead of zeros).

  mkmetadata.py <out.img> [size in MiB, default 150 = the HIDDEN partition]

Needs mke2fs (MKE2FS=/path, else PATH; the Android SDK platform-tools have
one). The file system is made with lazy_itable_init=0 and lazy_journal_init=0,
so that every block the image leaves out is really zero; the installer writes
the "don't care" chunks as zeros, nothing of the Samsung preload stays.
"""
import os
import shutil
import struct
import subprocess
import sys
import tempfile

BLK = 4096


def main():
    out = sys.argv[1]
    mib = int(sys.argv[2]) if len(sys.argv) > 2 else 150
    mke2fs = os.environ.get('MKE2FS') or shutil.which('mke2fs')
    if not mke2fs:
        sys.exit('mke2fs not found (set MKE2FS)')
    with tempfile.TemporaryDirectory() as d:
        raw = os.path.join(d, 'metadata.raw')
        subprocess.run([mke2fs, '-F', '-q', '-t', 'ext4', '-b', str(BLK), '-L', 'metadata',
                        '-E', 'lazy_itable_init=0,lazy_journal_init=0', raw, '%dk' % (mib * 1024)],
                       check=True)
        size = os.path.getsize(raw)
        assert size == mib << 20 and size % BLK == 0
        chunks = []  # (type, first block, nblocks)
        zero = bytes(BLK)
        with open(raw, 'rb') as f:
            blk = 0
            while True:
                data = f.read(BLK)
                if not data:
                    break
                kind = 0xcac3 if data == zero else 0xcac1
                if chunks and chunks[-1][0] == kind:
                    chunks[-1][2] += 1
                else:
                    chunks.append([kind, blk, 1])
                blk += 1
        total = size // BLK
        with open(raw, 'rb') as f, open(out, 'wb') as o:
            o.write(struct.pack('<IHHHHIIII', 0xed26ff3a, 1, 0, 28, 12, BLK, total, len(chunks), 0))
            for kind, first, n in chunks:
                body = n * BLK if kind == 0xcac1 else 0
                o.write(struct.pack('<HHII', kind, 0, n, 12 + body))
                if kind == 0xcac1:
                    f.seek(first * BLK)
                    o.write(f.read(body))
    print('%s: %d chunks, %d bytes' % (out, len(chunks), os.path.getsize(out)))


main()
