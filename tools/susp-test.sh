#!/bin/sh
# suspend test on the tablet: susp.sh <rtc seconds|0> <tag> [pm_test level]
sleep 2
echo N > /sys/module/printk/parameters/console_suspend
echo 1 > /sys/power/pm_debug_messages
/tmp/pmic-wdg.sh arm 120 > /dev/kmsg
cat /proc/interrupts > /tmp/irq0
echo 0 > /sys/class/rtc/rtc0/wakealarm
[ "$1" -gt 0 ] && echo +$1 > /sys/class/rtc/rtc0/wakealarm
echo ${3:-none} > /sys/power/pm_test
echo "susp-test $2 start $(date +%s)" > /dev/kmsg
date +%s > /tmp/susp.$2
echo mem > /sys/power/state
echo "ret=$?" >> /tmp/susp.$2
echo none > /sys/power/pm_test
date +%s >> /tmp/susp.$2
/tmp/pmic-wdg.sh disarm > /dev/kmsg
cat /sys/power/pm_wakeup_irq >> /tmp/susp.$2 2>&1
cat /proc/interrupts > /tmp/irq1
echo "susp-test $2 done" > /dev/kmsg
