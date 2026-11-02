#!/usr/bin/env python3
"""
Run shell commands on the tablet over the USB CDC-ACM console (Test 2+).

  tools/tabsh.py 'dmesg | grep dwc2'        one command, prints its output
  tools/tabsh.py -t 30 'sleep 20; date'     longer timeout (seconds)
  TABSH_DEV=/dev/cu.usbmodemXYZ tools/tabsh.py ...

The device side is busybox sh on /dev/ttyGS0 with echo on. We switch echo
off, then frame every command with a marker the echo cannot produce.
Interactive use: screen /dev/cu.usbmodem* 115200 (quit: Ctrl-A K).
"""
import argparse
import glob
import os
import select
import sys
import termios
import time

MARK = "__TABSH_END__"


def open_port(dev):
    fd = os.open(dev, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    attrs = termios.tcgetattr(fd)
    attrs[0] = 0                                  # iflag
    attrs[1] = 0                                  # oflag
    attrs[2] = termios.CS8 | termios.CREAD | termios.CLOCAL
    attrs[3] = 0                                  # lflag: raw
    attrs[4] = attrs[5] = termios.B115200
    termios.tcsetattr(fd, termios.TCSANOW, attrs)
    return fd


def read_until(fd, needle, timeout):
    buf, end = b"", time.time() + timeout
    while time.time() < end:
        r, _, _ = select.select([fd], [], [], 0.2)
        if r:
            try:
                chunk = os.read(fd, 65536)
            except BlockingIOError:
                continue
            if not chunk:
                break
            buf += chunk
            if needle in buf:
                break
    return buf


def run(fd, cmd, timeout):
    # '""' splits the marker so the (possible) echo of this line never matches
    line = f'{cmd}\necho {MARK[:7]}""{MARK[7:]}$?\n'
    os.write(fd, line.encode())
    out = read_until(fd, MARK.encode(), timeout).decode(errors="replace")
    out = out.replace("\r\n", "\n")
    body, _, rest = out.partition(MARK)
    status = rest.split("\n", 1)[0].strip()
    return body, status


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("cmd")
    ap.add_argument("-t", "--timeout", type=float, default=10)
    a = ap.parse_args()

    devs = [os.environ["TABSH_DEV"]] if "TABSH_DEV" in os.environ else \
        sorted(glob.glob("/dev/cu.usbmodem*"))
    if not devs:
        sys.exit("no /dev/cu.usbmodem* - is Test 2 booted and the cable in?")
    fd = open_port(devs[0])
    # wake the shell, turn off echo, drop whatever was pending
    os.write(fd, b"\nstty -echo; PS1=; export PS1\n")
    time.sleep(0.5)
    read_until(fd, b"\0", 0.3)
    body, status = run(fd, a.cmd, a.timeout)
    sys.stdout.write(body.lstrip("\n"))
    if status != "0":
        sys.stderr.write(f"[exit {status or 'timeout'}]\n")
    sys.exit(0 if status == "0" else 1)


if __name__ == "__main__":
    main()
