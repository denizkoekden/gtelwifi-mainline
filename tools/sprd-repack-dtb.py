#!/usr/bin/env python3
# tools/sprd-repack-dtb.py <in boot.img> <dtb> <out boot.img> [kernel zImage]: same kernel, ramdisk, cmdline
# and addresses, new SPRD dt.img built from <dtb> (its sprd,sc-id triples) by
# mkbootimg_sprd.py. Used 05.11.2026 for the gtel3g (SM-T561) test images: the
# DTB of a build with fdtput -t u <dtb> / sprd,sc-id 8830 <rev> 131072 ...
# (decimal: -t u reads 0x20000 as 0). The DTB must have its real FDT length,
# not the page-padded size from a dt.img table.
import struct, subprocess, sys, tempfile, os
src, dtb, out = sys.argv[1:4]
newk = sys.argv[4] if len(sys.argv) > 4 else None   # optional replacement kernel
d = open(src, 'rb').read()
(magic, ks, ka, rs, ra, ss, sa, ta, ps, dts) = struct.unpack_from('<8s9I', d, 0)
assert magic == b'ANDROID!' and ss == 0
board = d[48:64].split(b'\0')[0].decode()
cmd = d[64:64 + 512].split(b'\0')[0].decode()
pg = lambda n: (n + ps - 1) // ps * ps
k = open(newk, 'rb').read() if newk else d[ps:ps + ks]; r = d[ps + pg(ks):ps + pg(ks) + rs]
t = tempfile.mkdtemp()
open(f'{t}/k', 'wb').write(k); open(f'{t}/r', 'wb').write(r)
tool = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'lineageos/device/samsung/gtelwifi/tools/mkbootimg_sprd.py')
subprocess.run([sys.executable, tool, '--kernel', f'{t}/k', '--ramdisk', f'{t}/r', '--cmdline', cmd, '--board', board,
    '--base', '0', '--kernel_offset', hex(ka), '--ramdisk_offset', hex(ra), '--second_offset', hex(sa),
    '--tags_offset', hex(ta), '--pagesize', str(ps), '--sprd_dtb', dtb, '--output', out], check=True)
for f in ('k', 'r'): os.remove(f'{t}/{f}')
os.rmdir(t)
print(f'{out}: cmdline={cmd!r} board={board!r}')
