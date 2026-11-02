#!/usr/bin/env python3
"""
Smoke test: boot the gtelwifi kernel on QEMU 'virt' (Cortex-A7, GICv2,
arch timer) with lcd_base pointing into RAM, then dump that memory twice
a few seconds apart. Only the SoC-independent parts are exercised:
decompressor, boot stripes, initramfs, our /init and its drawing code.
"""
import socket
import subprocess
import sys
import time

ZIMAGE = sys.argv[1]
OUT = sys.argv[2]          # prefix for raw dumps
SOCK = "/tmp/gtel-qmon.sock"
LCD = 0x7E000000
SIZE = 800 * 1280 * 4

qemu = subprocess.Popen([
    "qemu-system-arm", "-M", "virt", "-cpu", "cortex-a7", "-m", "1024",
    "-kernel", ZIMAGE, "-append", f"lcd_base={LCD:x} console=tty0",
    "-display", "none", "-serial", "none", "-nic", "none",
    "-monitor", f"unix:{SOCK},server,nowait"],
    stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

try:
    time.sleep(3)
    s = socket.socket(socket.AF_UNIX)
    s.connect(SOCK)
    s.settimeout(1)

    def drain():
        out = b""
        try:
            while True:
                d = s.recv(65536)
                if not d:
                    break
                out += d
        except socket.timeout:
            pass
        return out

    def cmd(c, wait=2):
        s.sendall(c.encode() + b"\n")
        time.sleep(wait)
        return drain()

    drain()
    for i, delay in enumerate((15, 4)):
        time.sleep(delay)
        cmd(f"pmemsave {LCD:#x} {SIZE} \"{OUT}{i + 1}.raw\"", 3)
    cmd("quit", 1)
finally:
    time.sleep(1)
    if qemu.poll() is None:
        qemu.kill()
print("done")
