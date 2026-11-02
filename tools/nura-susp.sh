#!/bin/sh
# suspend test under Nura: nura-susp.sh <rtc s> <tag> [pm_test level]
# Goes through systemd (logind/NetworkManager sleep hooks) with an RTC
# wake and the PMIC watchdog as safety net. Log in /var/tmp/susp.<tag>.
o=/var/tmp/susp.$2
sleep 2
/var/tmp/pmic-wdg.sh arm 240 > /dev/kmsg
echo ${3:-none} > /sys/power/pm_test
echo 0 > /sys/class/rtc/rtc0/wakealarm
echo +$1 > /sys/class/rtc/rtc0/wakealarm
echo "start $(date +%s)" > $o
echo "susp-test $2 start" > /dev/kmsg
n0=$(cat /sys/power/suspend_stats/success)
systemctl suspend
# systemctl returns before the sleep: wait for the resume
i=0
while [ "$(cat /sys/power/suspend_stats/success)" = "$n0" ] && [ $i -lt 600 ]; do
	sleep 0.5; i=$((i + 1))
done
sleep 5
echo "end $(date +%s) wake irq $(cat /sys/power/pm_wakeup_irq 2>&1)" >> $o
echo none > /sys/power/pm_test
/var/tmp/pmic-wdg.sh disarm > /dev/kmsg
echo "susp-test $2 done" > /dev/kmsg
