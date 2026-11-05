#!/bin/sh
# Rescue system: one suspend with the sleep-mode experiment parameters of
# sc7730.c, battery current with the charger input suspended (SMB358 CMD_A
# bit 2), panel off. Results go to /tmp/sleepmode.txt and to the kernel log
# (ramoops keeps it over a watchdog reset). Positive mA = charging.
#   pw-sleepmode.sh <core_gate 0|1> <mcu_pause bits> <wake_mux bits> [seconds]
cd /sys/kernel/debug/sprd-adi
rd() { echo $1 > address; cat value; }
clb() { lo=$(rd 0x56c); hi=$(rd 0x568); echo $(( (hi << 16) | lo )); }
ma() { echo $(( ($2 - $1) * 1000 / ($3 * 2 * 2707) )); }
P=/sys/module/sc7730/parameters
t=${4:-45}
tag="core_gate=$1 mcu_pause=$2 wake_mux=$3"
a=$(i2cget -f -y 3 0x6a 0x30)
echo 1 > /sys/module/sprd_dispc/parameters/panel_power_off
echo 1 > /sys/class/graphics/fb0/blank
sleep 3
echo "sleepmode start $tag" > /dev/kmsg
echo $1 > $P/suspend_core_gate
echo $2 > $P/suspend_mcu_pause
echo $3 > $P/suspend_wake_mux
i2cset -f -y 3 0x6a 0x30 $(printf 0x%02x $((a | 0x04)))
/tmp/pmic-wdg.sh arm $((t + 90)) > /dev/kmsg
echo 0 > /sys/class/rtc/rtc0/wakealarm
echo +$t > /sys/class/rtc/rtc0/wakealarm
c0=$(clb); t0=$(date +%s)
echo mem > /sys/power/state
c1=$(clb); t1=$(date +%s)
/tmp/pmic-wdg.sh disarm > /dev/kmsg
r="sleepmode $tag: $(ma $c0 $c1 $((t1 - t0))) mA over $((t1 - t0)) s, CMD_A $(i2cget -f -y 3 0x6a 0x30)"
echo 0 > $P/suspend_core_gate
echo 0 > $P/suspend_mcu_pause
echo 0 > $P/suspend_wake_mux
i2cset -f -y 3 0x6a 0x30 $a
echo 0 > /sys/class/graphics/fb0/blank
echo "$r" > /dev/kmsg
echo "$r" >> /tmp/sleepmode.txt
