#!/usr/bin/env python3
"""
Dump the layout of a Spreadtrum/Samsung boot.img (Android header v0 with
the CAF "dt_size" field and a dtbToolSPRD dt.img behind the ramdisk).

Used to check the stock boot image of the SM-T560 against the assumptions
in mkimage.py: base/offsets, page size, board name, dt.img magic and the
sprd,sc-id entries. With --extract the pieces are written out as well.
"""
import argparse
import os
import shutil
import struct
import subprocess
import sys

HDR = struct.Struct("<8s10I16s512s32s1024s")
ZIMAGE_MAGIC = 0x016F2818
FDT_MAGIC = b"\xd0\x0d\xfe\xed"


def align(n, page):
    return (n + page - 1) // page * page


def compression(blob):
    sigs = {b"\x1f\x8b": "gzip", b"\x02\x21\x4c\x18": "lz4-legacy",
            b"\x04\x22\x4d\x18": "lz4", b"\xfd7zXZ": "xz", b"BZh": "bzip2",
            b"\x89LZO": "lzo", b"070701": "cpio (uncompressed)"}
    for sig, name in sigs.items():
        if blob.startswith(sig):
            return name
    return "unknown (%s)" % blob[:8].hex()


def dtb_props(blob):
    """model, compatible and sprd,sc-id of a DTB via dtc, if available."""
    if not shutil.which("dtc"):
        return {}
    src = subprocess.run(["dtc", "-q", "-I", "dtb", "-O", "dts", "-"],
                         input=blob, capture_output=True).stdout.decode()
    out, depth = {}, 0
    for line in src.splitlines():
        s = line.strip()
        if depth == 1:
            for key in ("model", "compatible", "sprd,sc-id"):
                if s.startswith(key + " ="):
                    out[key] = s.split("=", 1)[1].strip(" ;")
        depth += s.count("{") - s.count("}")
    return out


def parse_dtimg(dt):
    magic, version, count = struct.unpack_from("<4s2I", dt, 0)
    print(f"  dt.img magic {magic!r}, version {version}, {count} entries")
    if version != 1:
        print("  !! only version 1 (20-byte entries) is decoded")
        return []
    entries = []
    for i in range(count):
        chip, plat, rev, off, size = struct.unpack_from("<5I", dt, 12 + 20 * i)
        blob = dt[off:off + size]
        ok = blob[:4] == FDT_MAGIC
        tot = struct.unpack_from(">I", blob, 4)[0] if ok else 0
        print(f"  [{i}] chipset {chip} platform {plat:#x} rev {rev:#x} "
              f"off {off:#x} size {size:#x} fdt {'ok' if ok else 'BAD'} "
              f"totalsize {tot:#x}")
        for k, v in dtb_props(blob[:tot] if ok else blob).items():
            print(f"        {k} = {v}")
        entries.append((chip, plat, rev, blob[:tot] if ok else blob))
    return entries


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("image")
    ap.add_argument("--extract", metavar="DIR")
    a = ap.parse_args()

    img = open(a.image, "rb").read()
    (magic, ksz, kaddr, rsz, raddr, ssz, saddr, tags, page, dtsz, unused,
     name, cmdline, ident, extra) = HDR.unpack_from(img, 0)
    if magic != b"ANDROID!":
        sys.exit(f"not an Android boot image (magic {magic!r})")

    base = kaddr - 0x8000
    print(f"{a.image}: {len(img)} bytes ({len(img) / 2**20:.2f} MiB)")
    print(f"  page {page}, board {name.rstrip(b'\0')!r}, unused {unused:#x}")
    print(f"  kernel  {ksz:>9} @ {kaddr:#010x}   (base {base:#x} + 0x8000?)")
    print(f"  ramdisk {rsz:>9} @ {raddr:#010x}   offset {raddr - base:#x}")
    print(f"  second  {ssz:>9} @ {saddr:#010x}   offset {saddr - base:#x}")
    print(f"  tags              @ {tags:#010x}   offset {tags - base:#x}")
    print(f"  dt_size {dtsz:>9}")
    print(f"  cmdline {cmdline.rstrip(b'\0').decode(errors='replace')!r}")
    print(f"  extra   {extra.rstrip(b'\0').decode(errors='replace')!r}")
    print(f"  id      {ident.hex()}")

    off = align(HDR.size, page)
    kernel = img[off:off + ksz]
    off = align(off + ksz, page)
    ramdisk = img[off:off + rsz]
    off = align(off + rsz, page)
    second = img[off:off + ssz]
    off = align(off + ssz, page)
    dt = img[off:off + dtsz]
    off = align(off + dtsz, page)

    zmagic = struct.unpack_from("<I", kernel, 0x24)[0] if ksz > 0x28 else 0
    print(f"  kernel: zImage magic {'ok' if zmagic == ZIMAGE_MAGIC else hex(zmagic)}"
          f", zImage end {struct.unpack_from('<I', kernel, 0x2c)[0]:#x}"
          if zmagic == ZIMAGE_MAGIC else f"  kernel: no zImage magic ({zmagic:#x})")
    appended = kernel.find(FDT_MAGIC)
    if appended >= 0:
        print(f"  kernel: FDT magic inside kernel at {appended:#x} (appended DTB?)")
    print(f"  ramdisk: {compression(ramdisk)}")

    trailer = img[off:].rstrip(b"\0")
    print(f"  after payload: {len(img) - off} bytes, "
          f"trailer {trailer[:32]!r}")

    entries = []
    if dtsz:
        entries = parse_dtimg(dt)

    if a.extract:
        os.makedirs(a.extract, exist_ok=True)
        for fn, blob in (("kernel", kernel), ("ramdisk", ramdisk),
                         ("second", second), ("dt.img", dt)):
            if blob:
                open(os.path.join(a.extract, fn), "wb").write(blob)
        for i, (chip, plat, rev, blob) in enumerate(entries):
            open(os.path.join(a.extract, f"dtb{i}-plat{plat}.dtb"), "wb").write(blob)
        print(f"  extracted to {a.extract}")


if __name__ == "__main__":
    main()
