#!/usr/bin/env python3
#
# SPDX-License-Identifier: Apache-2.0
#
"""
Rebuild boot images that sboot is known to boot with mkbootimg_sprd.py and
compare them byte for byte.

  selftest.py --prebuilt-dt IMG   repack kernel/ramdisk/dt.img/cmdline of IMG
  selftest.py --from-dtb IMG      same, but rebuild the dt.img from its single
                                  DTB (dtbTool-sprd layout, one blob)
"""
import os
import struct
import subprocess
import sys
import tempfile

HDR = "<8s10I16s512s32s1024s"
TOOL = os.path.join(os.path.dirname(os.path.abspath(__file__)), "mkbootimg_sprd.py")


def parse(img):
    f = struct.unpack(HDR, img[:struct.calcsize(HDR)])
    h = dict(zip(("magic", "kernel_size", "kernel_addr", "ramdisk_size",
                  "ramdisk_addr", "second_size", "second_addr", "tags_addr",
                  "page_size", "dt_size", "unused", "name", "cmdline", "id",
                  "extra_cmdline"), f))
    assert h["magic"] == b"ANDROID!"
    page, off = h["page_size"], h["page_size"]
    for part in ("kernel", "ramdisk", "second", "dt"):
        size = h[f"{part}_size"]
        h[part] = img[off:off + size]
        off += (size + page - 1) // page * page
    return h


def main():
    mode, path = sys.argv[1], sys.argv[2]
    img = open(path, "rb").read()
    h = parse(img)
    base = h["kernel_addr"] - 0x8000
    with tempfile.TemporaryDirectory() as tmp:
        files = {}
        for part in ("kernel", "ramdisk", "dt"):
            files[part] = os.path.join(tmp, part)
            open(files[part], "wb").write(h[part])
        args = [sys.executable, "-I", TOOL, "--kernel", files["kernel"],
                "--ramdisk", files["ramdisk"],
                "--cmdline", h["cmdline"].rstrip(b"\0").decode(),
                "--board", h["name"].rstrip(b"\0").decode(),
                "--base", hex(base), "--pagesize", str(h["page_size"]),
                "--kernel_offset", hex(h["kernel_addr"] - base),
                "--ramdisk_offset", hex(h["ramdisk_addr"] - base),
                "--second_offset", hex(h["second_addr"] - base),
                "--tags_offset", hex(h["tags_addr"] - base),
                "--dt_out", os.path.join(tmp, "dt.out"),
                "-o", os.path.join(tmp, "out.img")]
        if mode == "--prebuilt-dt":
            args += ["--dt", files["dt"]]
        else:
            off = struct.unpack("<I", h["dt"][24:28])[0]
            total = struct.unpack(">I", h["dt"][off + 4:off + 8])[0]
            dtb = os.path.join(tmp, "one.dtb")
            open(dtb, "wb").write(h["dt"][off:off + total])
            args += ["--sprd_dtb", dtb]
        subprocess.run(args, check=True)
        out = open(os.path.join(tmp, "out.img"), "rb").read()
        dt = open(os.path.join(tmp, "dt.out"), "rb").read()
    print(f"{os.path.basename(path)} {mode}: dt.img {'identical' if dt == h['dt'] else 'DIFFERS'}, "
          f"boot.img {'identical' if out == img[:len(out)] and len(img) - len(out) < h['page_size'] else 'DIFFERS'} "
          f"({len(out)} vs {len(img)} bytes)")
    return 0 if dt == h["dt"] and out == img[:len(out)] else 1


if __name__ == "__main__":
    sys.exit(main())
