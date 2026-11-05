#!/usr/bin/env python3
"""
Build the TWRP packages of a LineageOS build for the SM-T560:

  mkzip.py <update-binary> <boot.img> <super_SYSTEM.img (sparse)> <outdir> <name>

writes <name>-install.zip (also clears /metadata and /data, a clean
install) and <name>-update.zip (keeps /data). Entries are stored, not
compressed: the static update-binary (update-binary.c) reads them directly,
there is no zlib on the device side. TWRP runs META-INF/com/google/android/
update-binary; the updater-script is only a placeholder for tools that look
for it.
"""
import hashlib
import os
import sys
import zipfile

ub, boot, system, outdir, name = sys.argv[1:6]
META = 'META-INF/com/google/android/'


def build(path, wipe):
    with zipfile.ZipFile(path, 'w', zipfile.ZIP_STORED, allowZip64=True) as z:
        info = zipfile.ZipInfo(META + 'update-binary')
        info.external_attr = 0o755 << 16
        info.compress_type = zipfile.ZIP_STORED
        with open(ub, 'rb') as f:
            z.writestr(info, f.read())
        z.writestr(META + 'updater-script', '# installer: update-binary (static, see update-binary.c)\n')
        z.write(boot, 'boot.img')
        z.write(system, 'system.img')
        if wipe:
            z.writestr('wipe', 'clear /metadata and /data\n')
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for block in iter(lambda: f.read(1 << 20), b''):
            h.update(block)
    print('%s  %s' % (h.hexdigest(), os.path.basename(path)))


os.makedirs(outdir, exist_ok=True)
build(os.path.join(outdir, name + '-install.zip'), True)
build(os.path.join(outdir, name + '-update.zip'), False)
