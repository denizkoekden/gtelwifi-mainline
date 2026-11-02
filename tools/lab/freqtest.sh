#!/bin/sh
# cpufreq step test: set each frequency, measure it, read back the clock
# registers, then 4-core sha256 stress against a reference hash.
P=/sys/devices/system/cpu/cpufreq/policy0
F=/tmp/stress.bin
dd if=/dev/urandom of=$F bs=1M count=4 2>/dev/null
ref=$(sha256sum $F | cut -c1-16)
echo userspace > $P/scaling_governor
for f in ${FREQS:-1200000 1000000 768000 1300000 1000000 1200000 768000 1300000}; do
	/tmp/pmic-wdg.sh arm ${WDG:-90} >/dev/null
	echo $f > $P/scaling_setspeed
	hz=$(/tmp/cpuhz | sed 's/.*~//')
	ckg=$(devmem 0x20d03000 32); mp=$(devmem 0x402e3070 32)
	rm -f /tmp/bad.*
	pids=""
	for c in 0 1 2 3; do
		( i=0; while [ $i -lt ${N:-25} ]; do
			[ "$(sha256sum $F | cut -c1-16)" = "$ref" ] || echo x >> /tmp/bad.$c
			i=$((i + 1))
		  done ) &
		pids="$pids $!"
	done
	t0=$(cut -d. -f1 /proc/uptime)
	wait $pids
	t1=$(cut -d. -f1 /proc/uptime)
	bad=$(cat /tmp/bad.* 2>/dev/null | wc -l)
	uv=""
	for r in /sys/class/regulator/regulator.*; do [ "$(cat $r/name)" = vddarm ] && uv=$(cat $r/microvolts); done
	echo "$f kHz: cur $(cat $P/cpuinfo_cur_freq), gemessen $hz, CKG $ckg MPLL $mp, vddarm $uv (ADC $(/tmp/vmeas.sh 2>/dev/null)), Last $((t1 - t0)) s, Fehler $bad, $(cat /sys/class/thermal/thermal_zone0/temp)"
	/tmp/pmic-wdg.sh disarm >/dev/null
done
echo performance > $P/scaling_governor
rm -f $F
