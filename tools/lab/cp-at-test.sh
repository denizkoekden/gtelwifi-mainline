#!/bin/sh
# AT commands to the modem core over the stty_w channels, after cp-boot-test.sh
# style start. Runs on the tablet in the rescue system (modem kernel image).
#   cp-at-test.sh [maxN [command]]   load, start, send the command (default AT)
#                                    on stty_w0..maxN, stop
P=/proc/cpw
M=/dev/mmcblk0p8
D=/dev/mmcblk0p7
/var/tmp/pmic-wdg.sh arm 150 || echo "watchdog not armed"
dd if=$M of=$P/modem bs=65536 2>&1 | tail -1
dd if=$D of=$P/dsp bs=65536 2>&1 | tail -1
echo 1 > $P/start
sleep 12
echo "status: $(cat $P/status)"
for n in $(seq 0 ${1:-15}); do
	d=/dev/stty_w$n
	[ -c $d ] || continue
	rm -f /tmp/at.$n
	( timeout 3 cat $d > /tmp/at.$n 2>/dev/null ) &
	sleep 0.3
	printf '%s\r' "${2:-AT}" > $d 2>/dev/null
	sleep 3.2
	echo "stty_w$n: $(tr -d '\r' < /tmp/at.$n 2>/dev/null | tr '\n' '|')"
done
echo "== dmesg"; dmesg | grep -i -E "cproc|sbuf|sblock|smsg|seth|cpw|wdt" | tail -25
echo 1 > $P/stop
/var/tmp/pmic-wdg.sh disarm
echo "status: $(cat $P/status)"
