#!/bin/sh
# Modem core (CP) start test, runs on the tablet in the rescue system with the
# kernel of buildkit/gtelwifi-modem.img (SIPC + cproc, DT with the modem nodes
# enabled). The Wi-Fi model has no radio but the same chip and the same
# partitions as the 3G model (MODEM p8, WDSP p7), so the core can be started
# and the IPC to it observed.
#
#   cp-boot-test.sh probe          what the drivers found (no hardware access)
#   cp-boot-test.sh start          load MODEM/WDSP, start, watch 20 s
#
# The PMIC watchdog (tools/pmic-wdg.sh) is armed for 120 s before "start":
# starting the core could hang the bus (it takes PLLs and power domains).
set -u
P=/proc/cpw
M=/dev/mmcblk0p8	# MODEM
D=/dev/mmcblk0p7	# WDSP

probe() {
	echo "== drivers"
	dmesg | grep -i -E "sipc|cproc|cpw|spipe|slog|seth|smsg|sblock|sbuf" | head -40
	echo "== /proc/cpw"; ls -l $P 2>&1
	echo "== devices"; ls -l /dev/spipe_w* /dev/slog_w* /dev/stty_w0 2>&1 | head -20
	echo "== net"; ls /sys/class/net
	echo "== interrupts"; grep -i -E "sipc|cpw" /proc/interrupts
}

start() {
	[ -w $P/start ] || { echo "no $P/start (drivers missing?)"; exit 1; }
	for f in $M $D; do [ -b $f ] || { echo "no $f"; exit 1; }; done
	# the first bytes of the images, to see that these are the right partitions
	echo "== MODEM head: $(dd if=$M bs=16 count=1 2>/dev/null | od -An -tx4)"
	echo "== WDSP head:  $(dd if=$D bs=16 count=1 2>/dev/null | od -An -tx4)"
	/var/tmp/pmic-wdg.sh arm 120 || echo "watchdog not armed"
	echo "== load modem"; dd if=$M of=$P/modem bs=65536 2>&1 | tail -1
	echo "== load dsp";   dd if=$D of=$P/dsp   bs=65536 2>&1 | tail -1
	echo "== start";      echo 1 > $P/start
	i=0
	while [ $i -lt 20 ]; do
		sleep 1; i=$((i + 1))
		echo "t=$i status=$(cat $P/status 2>&1) irq: $(grep -i sipc /proc/interrupts | tr -s ' ')"
	done
	echo "== dmesg (last 60)"; dmesg | tail -60
	echo "== stop"; echo 1 > $P/stop
	/var/tmp/pmic-wdg.sh disarm
	echo "status: $(cat $P/status)"
}

case ${1:-probe} in
probe) probe ;;
start) start ;;
*) echo "usage: $0 probe|start"; exit 1 ;;
esac
