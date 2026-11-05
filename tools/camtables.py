#!/usr/bin/env python3
"""
camtables: pull the camera sensor register tables out of the stock camera
HAL (camera.sc8830.so) of a Galaxy Tab E 9.6 (SM-T560). The tables are
Samsung/vendor data and stay on the machine they are extracted on: the
output goes to out/cam/ and is never distributed.

  tools/camtables.py [--firmware] [HAL.so | system.ext4.win] [outdir]

Default source is the TWRP system backup. Output per sensor and mode:
<sensor>-<index>-<w>x<h>.bin with little-endian {u16 reg, u16 val} pairs,
exactly as the HAL stores them (8-bit registers of the SR200PC20M sit in
16-bit fields). Index 0 is the init table.

--firmware writes the layout the kernel drivers request instead:
<outdir>/<sensor>/init.bin and <outdir>/<sensor>/<w>x<h>.bin, for
/lib/firmware on the device (default outdir out/cam-firmware).

HAL layout (vendor-ref/research/camera-m2-report.md section 3.1): the
sensor info struct holds a pointer to the resolution table, whose 24-byte
entries are {table pointer, count, w | h << 16, xclk, fmt, extra}.

--firmware also writes the S5K tables of the 5 MP still / autofocus draft
(camera-m4-report.md section 6) from the HAL's settings array, 12-byte
entries {table pointer, count, name pointer}: 2560x1920.bin (not in the
resolution table) and af-normal.bin / af-macro.bin, the three range tables
each followed by a 100 ms delay entry {0xffff, 100}.
"""

import os
import struct
import sys
import tarfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BACKUP = os.path.join(ROOT, 'backup/twrp/2013-12-31--21-52-36_KTU84PT560XXU0APL1/system.ext4.win')
MEMBER = '/lib/hw/camera.sc8830.so'

# sensor info structs in the stock HAL (KTU84P.T560XXU0APL1)
SENSORS = {'s5k4ecgx': 0xa5818, 'sr200pc20m': 0xa5ea8}
# S5K settings array (entries 0x00-0x8d) and the tables taken from it
SETTINGS, NSETTINGS = 0x9ae78, 0x8e
DELAY_100MS = struct.pack('<HH', 0xffff, 100)
EXTRA = {
    's5k4ecgx/2560x1920.bin': ['regs_s5k4ecgx_capture_2560_1920'],
    's5k4ecgx/af-normal.bin': ['regs_s5k4ecgx_AF_normal_mode_%d' % i for i in (1, 2, 3)],
    's5k4ecgx/af-macro.bin': ['regs_s5k4ecgx_AF_macro_mode_%d' % i for i in (1, 2, 3)],
}


def load_hal(path):
    if path.endswith('.so'):
        return open(path, 'rb').read()
    with tarfile.open(path) as tar:
        return tar.extractfile(tar.getmember(MEMBER)).read()


class Elf:
    def __init__(self, data):
        self.d = data
        phoff, = struct.unpack_from('<I', data, 0x1c)
        phentsize, phnum = struct.unpack_from('<HH', data, 0x2a)
        self.segs = []
        for i in range(phnum):
            ptype, poff, vaddr, _, filesz = struct.unpack_from(
                '<5I', data, phoff + i * phentsize)
            if ptype == 1:
                self.segs.append((vaddr, poff, filesz))

    def off(self, va):
        for vaddr, poff, filesz in self.segs:
            if vaddr <= va < vaddr + filesz:
                return poff + va - vaddr
        raise ValueError('address %#x not in a load segment' % va)

    def u32(self, va):
        return struct.unpack_from('<I', self.d, self.off(va))[0]

    def raw(self, va, n):
        o = self.off(va)
        return self.d[o:o + n]

    def cstr(self, va):
        o = self.off(va)
        return self.d[o:self.d.index(b'\0', o)].decode()


def main():
    args = [a for a in sys.argv[1:] if a != '--firmware']
    fwlayout = len(args) != len(sys.argv) - 1
    src = args[0] if args else BACKUP
    outdir = args[1] if len(args) > 1 else os.path.join(
        ROOT, 'out/cam-firmware' if fwlayout else 'out/cam')
    elf = Elf(load_hal(src))

    for name, info in SENSORS.items():
        restab = elf.u32(info + 15 * 4)
        for i in range(12):
            ptr, count, wh = (elf.u32(restab + 24 * i + 4 * j) for j in range(3))
            if not ptr or not count:
                continue
            w, h = wh & 0xffff, wh >> 16
            if fwlayout:
                fn = os.path.join(outdir, name, 'init.bin' if i == 0 else '%dx%d.bin' % (w, h))
            else:
                fn = os.path.join(outdir, '%s-%d-%dx%d.bin' % (name, i, w, h))
            os.makedirs(os.path.dirname(fn), exist_ok=True)
            with open(fn, 'wb') as f:
                f.write(elf.raw(ptr, 4 * count))
            first = ' '.join('%04x=%04x' % struct.unpack_from('<HH', elf.raw(ptr + 4 * k, 4))
                             for k in range(min(3, count)))
            print('%-40s %5d regs  %s ...' % (os.path.relpath(fn, ROOT), count, first))

    if not fwlayout:
        return
    settings = {}
    for i in range(NSETTINGS):
        ptr, count, name = (elf.u32(SETTINGS + 12 * i + 4 * j) for j in range(3))
        if ptr and count and name:
            settings[elf.cstr(name)] = (ptr, count)
    for fn, names in EXTRA.items():
        data = b''
        for n in names:
            ptr, count = settings[n]
            data += elf.raw(ptr, 4 * count)
            if len(names) > 1:
                data += DELAY_100MS
        fn = os.path.join(outdir, fn)
        with open(fn, 'wb') as f:
            f.write(data)
        print('%-40s %5d bytes from %s' % (os.path.relpath(fn, ROOT), len(data), ', '.join(names)))


if __name__ == '__main__':
    main()
