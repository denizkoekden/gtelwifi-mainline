#!/bin/sh
# 5 suspend cycles, RTC wake after 8 s each, PMIC watchdog as safety net
sleep 2
/tmp/pmic-wdg.sh arm 240 > /dev/kmsg
for i in 1 2 3 4 5; do
	echo 0 > /sys/class/rtc/rtc0/wakealarm
	echo +8 > /sys/class/rtc/rtc0/wakealarm
	t0=$(date +%s)
	echo mem > /sys/power/state
	echo "cycle $i ret=$? $(( $(date +%s) - t0 )) s irq $(cat /sys/power/pm_wakeup_irq 2>&1)" >> /tmp/susp5.out
	sleep 3
done
/tmp/pmic-wdg.sh disarm > /dev/kmsg
echo done >> /tmp/susp5.out
