#!/bin/sh
# Rescue system: battery current in suspend with and without DDR
# self-refresh (sc7730.ddr_selfref), panel powered off, charger input
# suspended (SMB358 CMD_A bit 2), 60 s per run, FGU coulomb counter.
# Needs /tmp/pmic-wdg.sh. Result in /tmp/selfref.txt. Positive mA = charging:
# CMD_A is set again before every run and logged after it, because the
# smb347 driver may write its cached CMD_A back (then the run is invalid).
cd /sys/kernel/debug/sprd-adi
rd() { echo $1 > address; cat value; }
clb() { lo=$(rd 0x56c); hi=$(rd 0x568); echo $(( (hi << 16) | lo )); }
ma() { echo $(( ($2 - $1) * 1000 / ($3 * 2 * 2707) )); }
out=/tmp/selfref.txt
p=/sys/module/sc7730/parameters/ddr_selfref
a=$(i2cget -f -y 3 0x6a 0x30)
i2cset -f -y 3 0x6a 0x30 $(printf 0x%02x $((a | 0x04)))
echo "CMD_A $a -> $(i2cget -f -y 3 0x6a 0x30)" > $out
echo 1 > /sys/module/sprd_dispc/parameters/panel_power_off
echo 1 > /sys/class/graphics/fb0/blank
sleep 5
for s in 0 1 0 1 0 1; do
	echo $s > $p
	i2cset -f -y 3 0x6a 0x30 $(printf 0x%02x $((a | 0x04)))
	/tmp/pmic-wdg.sh arm 150 > /dev/kmsg
	echo 0 > /sys/class/rtc/rtc0/wakealarm
	echo +60 > /sys/class/rtc/rtc0/wakealarm
	c0=$(clb); t0=$(date +%s)
	echo mem > /sys/power/state
	c1=$(clb); t1=$(date +%s)
	/tmp/pmic-wdg.sh disarm > /dev/kmsg
	echo "suspend ddr_selfref=$s: $(ma $c0 $c1 $((t1 - t0))) mA over $((t1 - t0)) s, CMD_A after $(i2cget -f -y 3 0x6a 0x30)" >> $out
	sleep 3
done
echo 1 > $p
echo 0 > /sys/class/graphics/fb0/blank
i2cset -f -y 3 0x6a 0x30 $a
echo "restored CMD_A $(i2cget -f -y 3 0x6a 0x30)" >> $out
echo done >> $out
