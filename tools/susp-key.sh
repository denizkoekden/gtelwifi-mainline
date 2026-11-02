#!/bin/sh
# Sleep until a key wakes the tablet: each round suspends with an RTC
# fallback of 60 min and the PMIC watchdog at 70 min as safety net. Stops
# after a round that a key (Power, Volume Down, Home, Volume Up) woke, or
# after 8 rounds. Log in /tmp/susp-key.log.
irqcount() { awk -v n="$1" '$NF == n { s = 0; for (i = 2; i <= 5; i++) s += $i; print s }' /proc/interrupts; }
keys() { echo "$(irqcount Power) $(irqcount Down) $(irqcount 40250000.keypad)"; }
sleep 2
log=/tmp/susp-key.log
echo "start $(date +%s) keys $(keys)" > $log
for r in 1 2 3 4 5 6 7 8; do
	before=$(keys)
	/tmp/pmic-wdg.sh arm 4200 > /dev/kmsg
	echo 0 > /sys/class/rtc/rtc0/wakealarm
	echo +3600 > /sys/class/rtc/rtc0/wakealarm
	t0=$(date +%s)
	echo mem > /sys/power/state
	ret=$?
	t1=$(date +%s)
	/tmp/pmic-wdg.sh disarm > /dev/kmsg
	after=$(keys)
	echo "round $r ret=$ret slept $((t1 - t0)) s wake irq $(cat /sys/power/pm_wakeup_irq 2>&1) keys $before -> $after" >> $log
	echo 0 > /sys/class/rtc/rtc0/wakealarm
	[ "$before" != "$after" ] && { echo "woken by a key" >> $log; break; }
	sleep 5
done
echo "end $(date +%s)" >> $log
