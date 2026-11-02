#!/usr/bin/env python3
"""
Build a boot.img for the Samsung Galaxy Tab E SM-T560 (gtelwifi, SC7730).

Format (from the LineageOS device tree, device/samsung/gtelwifi):
  - Android boot image header v0, page size 2048, base 0x00000000,
    ramdisk offset 0x01000000, second offset 0x00f00000, tags 0x100,
    board name "sc8830"
  - CAF-style "dt_size" field in the header, followed by a dt.img
  - dt.img = QCDT v1 layout with magic "SPRD", entries matched on
    "sprd,sc-id = <8830 REV 0x20000>" (dtbToolSPRD)

We do not know the hardware revision of the user's board, so the same
mainline DTB is emitted once per known revision with its own sc-id.
"""
import argparse
import gzip
import hashlib
import io
import os
import struct
import subprocess
import tempfile

PAGE = 2048
BASE = 0x00000000
KERNEL_OFFSET = 0x00008000
RAMDISK_OFFSET = 0x01000000
SECOND_OFFSET = 0x00F00000
TAGS_OFFSET = 0x00000100
# Stock KERNEL and the TWRP image in RECOVERY both leave the board name empty
BOARD = b""
CHIPSET = 8830
SOC_REV = 0x20000
BOARD_REVS = [2, 3, 4, 5, 6, 9, 10, 11]   # all gtelwifi revisions in vendor tree


def pad_qcdt(n):
    """dtbToolSPRD always pads, even a full extra page if already aligned."""
    return PAGE - (n % PAGE)


def pad_align(n):
    return (PAGE - (n % PAGE)) % PAGE


def dtb_for_rev(dts_dtb, rev, tmp):
    """Decompile, patch sprd,sc-id, recompile."""
    src = subprocess.run(["dtc", "-q", "-I", "dtb", "-O", "dts", dts_dtb],
                         check=True, capture_output=True, text=True).stdout
    old = "sprd,sc-id = <0x227e 0x0b 0x20000>;"
    assert old in src, "sc-id property not found in DTB"
    src = src.replace(old, f"sprd,sc-id = <0x227e {rev:#04x} 0x20000>;")
    out = os.path.join(tmp, f"rev{rev}.dtb")
    subprocess.run(["dtc", "-q", "-I", "dts", "-O", "dtb", "-o", out, "-"],
                   input=src, check=True, text=True)
    return open(out, "rb").read()


def build_dtimg(dtbs):
    count = len(dtbs)
    first = 12 + 20 * count + 4
    first += pad_qcdt(first)
    table, blobs, off = b"", b"", first
    for rev, blob in dtbs:
        size = len(blob) + pad_qcdt(len(blob))
        table += struct.pack("<5I", CHIPSET, rev, SOC_REV, off, size)
        blobs += blob + b"\0" * (size - len(blob))
        off += size
    hdr = b"SPRD" + struct.pack("<2I", 1, count) + table + struct.pack("<I", 0)
    hdr += b"\0" * (first - len(hdr))
    return hdr + blobs


def tiny_ramdisk():
    """Minimal newc cpio (just the trailer) - some bootloaders dislike size 0."""
    def entry(name, mode=0, nlink=1):
        name = name.encode() + b"\0"
        h = b"070701" + b"".join(b"%08X" % v for v in (
            0, mode, 0, 0, nlink, 0, 0, 0, 0, 0, 0, len(name), 0))
        rec = h + name
        rec += b"\0" * ((4 - len(rec) % 4) % 4)
        return rec
    data = entry("TRAILER!!!")
    data += b"\0" * ((512 - len(data) % 512) % 512)
    buf = io.BytesIO()
    with gzip.GzipFile(fileobj=buf, mode="wb", mtime=0) as g:
        g.write(data)
    return buf.getvalue()


def build_bootimg(kernel, ramdisk, dt, cmdline=b""):
    sha = hashlib.sha1()
    for blob in (kernel, ramdisk, b""):
        sha.update(blob)
        sha.update(struct.pack("<I", len(blob)))
    sha.update(dt)
    sha.update(struct.pack("<I", len(dt)))
    ident = sha.digest() + b"\0" * 12

    hdr = struct.pack(
        "<8s10I16s512s32s1024s",
        b"ANDROID!",
        len(kernel), BASE + KERNEL_OFFSET,
        len(ramdisk), BASE + RAMDISK_OFFSET,
        0, BASE + SECOND_OFFSET,
        BASE + TAGS_OFFSET,
        PAGE,
        len(dt), 0,
        BOARD, cmdline[:511], ident, b"")
    img = hdr + b"\0" * pad_align(len(hdr))
    for blob in (kernel, ramdisk, dt):
        img += blob + b"\0" * pad_align(len(blob))
    return img


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--zimage", required=True)
    ap.add_argument("--dtb", required=True)
    ap.add_argument("-o", "--out", required=True)
    a = ap.parse_args()

    with tempfile.TemporaryDirectory() as tmp:
        dtbs = [(r, dtb_for_rev(a.dtb, r, tmp)) for r in BOARD_REVS]
    dt = build_dtimg(dtbs)
    # No SEANDROIDENFORCE trailer: stock carries an RSA signature block
    # instead, and the unsigned TWRP image that sboot boots carries nothing.
    img = build_bootimg(open(a.zimage, "rb").read(), tiny_ramdisk(), dt)
    with open(a.out, "wb") as f:
        f.write(img)
    with open(os.path.splitext(a.out)[0] + "-dt.img", "wb") as f:
        f.write(dt)
    print(f"{a.out}: {len(img)} bytes, dt.img {len(dt)} bytes, "
          f"{len(dtbs)} board revisions")


if __name__ == "__main__":
    main()
