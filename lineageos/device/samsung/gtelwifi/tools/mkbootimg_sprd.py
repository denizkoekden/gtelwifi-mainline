#!/usr/bin/env python3
#
# SPDX-License-Identifier: Apache-2.0
#
"""
Boot image for Samsung sboot on SC7730 (gtelwifi): Android boot image header
v0 with the CAF "dt_size" field (offset 40, where later headers keep
header_version) and a Spreadtrum dt.img after the second stage.

AOSP mkbootimg only knows --dtb (header v2+), so BOARD_CUSTOM_BOOTIMG_MK
calls this instead. Arguments follow mkbootimg; on top of them:

  --dt FILE          prebuilt dt.img
  --sprd_dtb FILE    build the dt.img from one DTB, like dtbTool-sprd: one
                     table entry per "sprd,sc-id" triple of the root node,
                     all pointing at the same blob
  --dt_out FILE      also write the dt.img on its own

Layout and padding are those of buildkit/mkimage.py and dtbTool-sprd, which
sboot boots (byte-identical rebuilds of out/known-good.img and of
nura/out/gtelwifi-dtbtool-sprd-test.img, see tools/selftest.py).
"""
import argparse
import hashlib
import struct
import sys

FDT_MAGIC = 0xD00DFEED
FDT_BEGIN_NODE, FDT_END_NODE, FDT_PROP, FDT_NOP, FDT_END = 1, 2, 3, 4, 9


def align(n, page):
    return (page - n % page) % page


def pad_always(n, page):
    """dtbToolSPRD always pads, even a full extra page if already aligned."""
    return page - n % page


def root_property(dtb, wanted):
    magic, _, off_struct, off_strings = struct.unpack(">4I", dtb[:16])
    if magic != FDT_MAGIC:
        sys.exit("not a flattened device tree")
    pos, depth = off_struct, 0
    while True:
        token, = struct.unpack(">I", dtb[pos:pos + 4])
        pos += 4
        if token == FDT_BEGIN_NODE:
            end = dtb.index(b"\0", pos)
            pos = end + 1 + align(end + 1 - pos, 4)
            depth += 1
        elif token == FDT_END_NODE:
            depth -= 1
        elif token == FDT_PROP:
            length, nameoff = struct.unpack(">2I", dtb[pos:pos + 8])
            pos += 8
            name_end = dtb.index(b"\0", off_strings + nameoff)
            name = dtb[off_strings + nameoff:name_end].decode()
            if depth == 1 and name == wanted:
                return dtb[pos:pos + length]
            pos += length + align(length, 4)
        elif token == FDT_NOP:
            continue
        elif token == FDT_END:
            return None
        else:
            sys.exit(f"bad FDT token {token:#x} at {pos - 4:#x}")


def sprd_dtimg(dtb, page):
    ids = root_property(dtb, "sprd,sc-id")
    if not ids or len(ids) % 12:
        sys.exit("DTB has no sprd,sc-id = <chipset platform rev ...> triples")
    triples = [struct.unpack(">3I", ids[i:i + 12]) for i in range(0, len(ids), 12)]
    first = 12 + 20 * len(triples) + 4
    first += pad_always(first, page)
    size = len(dtb) + pad_always(len(dtb), page)
    table = b"".join(struct.pack("<5I", chip, plat, rev, first, size)
                     for chip, plat, rev in triples)
    hdr = b"SPRD" + struct.pack("<2I", 1, len(triples)) + table + struct.pack("<I", 0)
    return hdr + b"\0" * (first - len(hdr)) + dtb + b"\0" * (size - len(dtb))


def bootimg(a, kernel, ramdisk, dt):
    sha = hashlib.sha1()
    for blob in (kernel, ramdisk, b""):
        sha.update(blob)
        sha.update(struct.pack("<I", len(blob)))
    if dt:
        sha.update(dt)
        sha.update(struct.pack("<I", len(dt)))
    # like mkbootimg: what does not fit goes to extra_cmdline (sboot
    # ignores both and builds its own cmdline)
    cmdline = a.cmdline.encode()
    if len(cmdline) > 511 + 1023:
        sys.exit("cmdline longer than 1534 bytes")
    cmdline, extra = cmdline[:511], cmdline[511:]
    hdr = struct.pack(
        "<8s10I16s512s32s1024s",
        b"ANDROID!",
        len(kernel), a.base + a.kernel_offset,
        len(ramdisk), a.base + a.ramdisk_offset,
        0, a.base + a.second_offset,
        a.base + a.tags_offset,
        a.pagesize,
        len(dt), 0,
        a.board.encode(), cmdline, sha.digest(), extra)
    img = hdr + b"\0" * align(len(hdr), a.pagesize)
    for blob in (kernel, ramdisk, dt):
        img += blob + b"\0" * align(len(blob), a.pagesize)
    return img


def main():
    num = lambda s: int(s, 0)
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--kernel", required=True)
    ap.add_argument("--ramdisk")
    ap.add_argument("--second")
    ap.add_argument("--cmdline", default="")
    ap.add_argument("--board", default="")
    ap.add_argument("--base", type=num, default=0)
    ap.add_argument("--kernel_offset", type=num, default=0x00008000)
    ap.add_argument("--ramdisk_offset", type=num, default=0x01000000)
    ap.add_argument("--second_offset", type=num, default=0x00f00000)
    ap.add_argument("--tags_offset", type=num, default=0x00000100)
    ap.add_argument("--pagesize", type=num, default=2048)
    ap.add_argument("--header_version", type=num, default=0)
    # passed by the build system, no room for them in this header layout
    ap.add_argument("--os_version")
    ap.add_argument("--os_patch_level")
    dt = ap.add_mutually_exclusive_group(required=True)
    dt.add_argument("--dt")
    dt.add_argument("--sprd_dtb")
    ap.add_argument("--dt_out")
    ap.add_argument("-o", "--output", required=True)
    a = ap.parse_args()

    if a.header_version != 0:
        sys.exit("sboot only takes header version 0")
    if a.second:
        sys.exit("no second stage: its slot carries the dt.img")
    read = lambda p: open(p, "rb").read() if p else b""
    dtimg = read(a.dt) if a.dt else sprd_dtimg(read(a.sprd_dtb), a.pagesize)
    if a.dt_out:
        with open(a.dt_out, "wb") as f:
            f.write(dtimg)
    with open(a.output, "wb") as f:
        f.write(bootimg(a, read(a.kernel), read(a.ramdisk), dtimg))


if __name__ == "__main__":
    main()
