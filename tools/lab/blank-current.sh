#!/bin/sh
# awake current with the screen blanked: blankcur.sh <panel_power_off 0|1> <seconds>
cd /sys/kernel/debug/sprd-adi
rd() { echo $1 > address; cat value; }
clb() { lo=$(rd 0x56c); hi=$(rd 0x568); echo $(( (hi << 16) | lo )); }
ps=$(ls -d /sys/class/power_supply/smb347* | head -1)
echo $1 > /sys/module/sprd_dispc/parameters/panel_power_off
echo inhibit-charge > $ps/charge_behaviour
echo 1 > /sys/class/graphics/fb0/blank
sleep 3
g=$(grep -E "gpio-(152|232|233) " /sys/kernel/debug/gpio | awk '{print $1 "=" $5}' | tr '\n' ' ')
c0=$(clb); sleep $2; c1=$(clb)
echo 0 > /sys/class/graphics/fb0/blank
echo auto > $ps/charge_behaviour
echo "panel_off=$1 gpios: $g d=$((c1 - c0)) in $2 s = $(( (c1 - c0) * 1000 / ($2 * 2 * 2707) )) mA"
