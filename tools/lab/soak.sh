#!/bin/sh
# 3 min all-core sha256 load with schedutil and boost, sampled every 10 s
P=/sys/devices/system/cpu/cpufreq/policy0
F=/tmp/soak.bin
dd if=/dev/urandom of=$F bs=1M count=4 2>/dev/null
ref=$(sha256sum $F | cut -c1-16)
echo ${GOV:-schedutil} > $P/scaling_governor
echo 1 > /sys/devices/system/cpu/cpufreq/boost
/tmp/pmic-wdg.sh arm 300 >/dev/null
rm -f /tmp/bad.* /tmp/stop
pids=""
for c in 0 1 2 3; do
	( while [ ! -e /tmp/stop ]; do
		[ "$(sha256sum $F | cut -c1-16)" = "$ref" ] || echo x >> /tmp/bad.$c
	  done ) &
	pids="$pids $!"
done
for s in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18; do
	sleep 10
	echo "$((s * 10)) s: cur $(cat $P/scaling_cur_freq) max $(cat $P/scaling_max_freq) cdev $(cat /sys/class/thermal/cooling_device0/cur_state) temp $(cat /sys/class/thermal/thermal_zone0/temp)"
done
touch /tmp/stop
wait $pids
echo "Fehler: $(cat /tmp/bad.* 2>/dev/null | wc -l)"
/tmp/pmic-wdg.sh disarm >/dev/null
sleep 10
echo "Leerlauf: cur $(cat $P/scaling_cur_freq) temp $(cat /sys/class/thermal/thermal_zone0/temp)"
cat $P/stats/time_in_state 2>/dev/null | tr "\n" " "
rm -f $F /tmp/stop
