#!/usr/bin/env python3
# tools/twrp-repack.py <in twrp.img> <out twrp.img> [--dtb <dtb>]
#
# Rebuilds our TWRP image (boot image v0 + SPRD dt.img) with the recovery root
# overlay lineageos/twrp/device/samsung/gtelwifi/recovery/root/ copied into the
# ramdisk. Kernel, cmdline, addresses and every other ramdisk file stay as they
# are. For config-only TWRP fixes (twrp.flags, recovery.fstab, init rc) without
# a TWRP source tree (the one on the build host was deleted on 05.11.2026).
#
# The ramdisk is packed like the build system does it (xz --check=crc32
# --lzma2=dict=32MiB: LZMA2 only, no BCJ filter); with an unchanged overlay the
# output is byte-identical to the input. Only files that already exist in the
# ramdisk are replaced (their mode is kept).
#
# --dtb: device tree for the dt.img. Default: the image's own, cut to its FDT
# length. The gtel3g test image is built from out/gtel3g/allrev.dtb.
# --overlay: another root overlay instead of the device tree's, e.g. a test
# recovery.fstab (only the files in it are replaced).
import argparse, hashlib, lzma, os, struct, subprocess, sys, tempfile

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..')
OVERLAY = os.path.join(ROOT, 'lineageos/twrp/device/samsung/gtelwifi/recovery/root')
MKBOOTIMG = os.path.join(ROOT, 'lineageos/device/samsung/gtelwifi/tools/mkbootimg_sprd.py')
LIMIT = 16 * 1024 * 1024        # RECOVERY and KERNEL are 16 MiB
XZ_FILTERS = [{'id': lzma.FILTER_LZMA2, 'preset': 6, 'dict_size': 32 << 20}]


def cpio_parse(b):
    """newc archive -> list of [fields(13 ints), name, data]; trailer included"""
    ents, off = [], 0
    while True:
        h = b[off:off + 110]
        if h[:6] not in (b'070701', b'070702'):
            sys.exit(f'cpio: bad magic at {off:#x}')
        f = [int(h[6 + i * 8:14 + i * 8], 16) for i in range(13)]
        name = b[off + 110:off + 110 + f[11] - 1].decode()
        doff = (off + 110 + f[11] + 3) & ~3
        ents.append([f, name, b[doff:doff + f[6]]])
        off = (doff + f[6] + 3) & ~3
        if name == 'TRAILER!!!':
            return ents, b[off:]


def cpio_build(ents, tail):
    out = bytearray()
    for f, name, data in ents:
        f = list(f)
        f[6] = len(data)
        f[11] = len(name) + 1
        out += b'070701' + b''.join(b'%08x' % v for v in f) + name.encode() + b'\0'
        out += b'\0' * (-len(out) % 4)
        out += data
        out += b'\0' * (-len(out) % 4)
    return bytes(out) + tail


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('src')
    ap.add_argument('out')
    ap.add_argument('--dtb')
    ap.add_argument('--overlay', default=OVERLAY)
    a = ap.parse_args()

    d = open(a.src, 'rb').read()
    magic, ks, ka, rs, ra, ss, sa, ta, ps, dts = struct.unpack_from('<8s9I', d, 0)
    assert magic == b'ANDROID!' and ss == 0 and dts, 'boot image v0 with dt.img expected'
    board = d[48:64].split(b'\0')[0].decode()
    cmdline = d[64:64 + 512].split(b'\0')[0].decode()
    pg = lambda n: (n + ps - 1) // ps * ps
    kernel = d[ps:ps + ks]
    ramdisk = d[ps + pg(ks):ps + pg(ks) + rs]
    dt = d[ps + pg(ks) + pg(rs):ps + pg(ks) + pg(rs) + dts]

    if a.dtb:
        dtb = open(a.dtb, 'rb').read()
    else:
        assert dt[:4] == b'SPRD', 'SPRD dt.img expected'
        off, size = struct.unpack_from('<II', dt, 12 + 12)   # first entry: id, rev, subtype, offset, size
        dtb = dt[off:off + size]
        assert dtb[:4] == b'\xd0\x0d\xfe\xed', 'no FDT in dt.img'
        dtb = dtb[:struct.unpack_from('>I', dtb, 4)[0]]

    ents, tail = cpio_parse(lzma.decompress(ramdisk))
    by_name = {e[1]: e for e in ents}
    changed = []
    for top, _, files in os.walk(a.overlay):
        for fn in sorted(files):
            path = os.path.join(top, fn)
            rel = os.path.relpath(path, a.overlay)
            if rel not in by_name:
                sys.exit(f'{rel}: not in the ramdisk (new files are not supported)')
            data = open(path, 'rb').read()
            if by_name[rel][2] != data:
                by_name[rel][2] = data
                changed.append(rel)
    new_ramdisk = lzma.compress(cpio_build(ents, tail), format=lzma.FORMAT_XZ,
                                check=lzma.CHECK_CRC32, filters=XZ_FILTERS)
    lzma.decompress(new_ramdisk)    # round trip

    with tempfile.TemporaryDirectory() as t:
        for n, b in (('kernel', kernel), ('ramdisk', new_ramdisk), ('dtb', dtb)):
            open(os.path.join(t, n), 'wb').write(b)
        subprocess.run([sys.executable, MKBOOTIMG, '--kernel', f'{t}/kernel',
                        '--ramdisk', f'{t}/ramdisk', '--cmdline', cmdline, '--board', board,
                        '--base', '0', '--kernel_offset', hex(ka), '--ramdisk_offset', hex(ra),
                        '--second_offset', hex(sa), '--tags_offset', hex(ta),
                        '--pagesize', str(ps), '--sprd_dtb', f'{t}/dtb', '--output', a.out],
                       check=True, stdout=subprocess.DEVNULL)
    size = os.path.getsize(a.out)
    if size > LIMIT:
        os.remove(a.out)
        sys.exit(f'{a.out}: {size} bytes > {LIMIT}, removed')
    sha = hashlib.sha256(open(a.out, 'rb').read()).hexdigest()
    print(f'{a.out}: {size} bytes, ramdisk {rs} -> {len(new_ramdisk)}, sha256 {sha}')
    print('changed:', ', '.join(changed) if changed else 'nothing')


main()
