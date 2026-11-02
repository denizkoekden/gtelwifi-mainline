#!/usr/bin/env python3
"""
Emit gen_init_cpio lines for dynamically linked armhf binaries: the
binaries themselves, every shared library they need (recursively, via
DT_NEEDED) and the dynamic loader. Libraries come from the Debian armhf
multiarch directories of the build container.

  libdeps.py /usr/sbin/dropbear:/sbin/dropbear ...   (src:dst pairs)
"""
import os
import re
import subprocess
import sys

LIBDIRS = ["/usr/lib/arm-linux-gnueabihf", "/lib/arm-linux-gnueabihf"]
DST_LIBDIR = "/usr/lib/arm-linux-gnueabihf"


def readelf(path, opt):
    return subprocess.run(["arm-linux-gnueabihf-readelf", opt, path],
                          check=True, capture_output=True, text=True).stdout


def needed(path):
    return re.findall(r"\(NEEDED\).*\[(.+?)\]", readelf(path, "-d"))


def interp(path):
    m = re.search(r"Requesting program interpreter: (.+?)\]", readelf(path, "-l"))
    return m.group(1) if m else None


def find(lib):
    for d in LIBDIRS:
        p = os.path.join(d, lib)
        if os.path.exists(p):
            return os.path.realpath(p)
    sys.exit(f"libdeps: {lib} not found")


def main():
    lines, libs, todo, loaders = [], {}, [], set()
    for pair in sys.argv[1:]:
        src, dst = pair.split(":")
        lines.append(f"file {dst} {os.path.realpath(src)} 0755 0 0")
        todo.append(src)
        if interp(src):
            loaders.add(interp(src))
    while todo:
        for lib in needed(todo.pop()):
            if lib not in libs:
                libs[lib] = find(lib)
                todo.append(libs[lib])
    for lib, real in sorted(libs.items()):
        lines.append(f"file {DST_LIBDIR}/{lib} {real} 0755 0 0")
    for ld in sorted(loaders):
        # e.g. /lib/ld-linux-armhf.so.3; /lib is a symlink to usr/lib
        name = os.path.basename(ld)
        if name not in libs:
            lines.append(f"file {DST_LIBDIR}/{name} {find(name)} 0755 0 0")
        lines.append(f"slink /usr/lib/{name} arm-linux-gnueabihf/{name} 0777 0 0")
    print("\n".join(lines))


if __name__ == "__main__":
    main()
