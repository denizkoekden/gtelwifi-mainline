#!/bin/bash
# los-logwatch.sh <dir> [minutes]: wait for the tablet on adb (LineageOS,
# not the Poco "mondrian", not TWRP) and record logcat (all buffers) plus
# dmesg into <dir>/<n>-{logcat,dmesg}.txt, one pair per boot, until the time
# runs out. A reboot ends both streams; the loop then waits for the next boot.
# Never prints the adb serial (= device serial number).
set -u
dir=$1 mins=${2:-20}
mkdir -p "$dir"
end=$(( $(date +%s) + mins * 60 ))
n=0
tab() { adb devices -l 2>/dev/null | awk 'NR>1 && $2=="device" && !/mondrian/ {print $1; exit}'; }
while [ "$(date +%s)" -lt "$end" ]; do
	s=$(tab)
	if [ -z "$s" ]; then sleep 1; continue; fi
	n=$((n + 1))
	echo "$(date +%T) boot $n: adb up"
	adb -s "$s" logcat -b all -v threadtime > "$dir/$n-logcat.txt" 2>&1 &
	lp=$!
	adb -s "$s" shell 'su 0 dmesg -w 2>/dev/null || dmesg -w' > "$dir/$n-dmesg.txt" 2>&1 &
	dp=$!
	adb -s "$s" shell 'cat /sys/fs/pstore/console-ramoops-0 2>/dev/null' > "$dir/$n-pstore.txt" 2>&1
	wait "$lp"
	kill "$dp" 2>/dev/null
	echo "$(date +%T) boot $n: adb gone ($(wc -l < "$dir/$n-logcat.txt") logcat lines)"
done
echo "$(date +%T) done"
