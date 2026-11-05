#!/bin/sh
# battery current with the charger input suspended (SMB358 CMD_A bit 2),
# screen blanked awake and in suspend, panel left on vs powered off
cd /sys/kernel/debug/sprd-adi
rd() { echo $1 > address; cat value; }
clb() { lo=$(rd 0x56c); hi=$(rd 0x568); echo $(( (hi << 16) | lo )); }
ma() { echo $(( ($2 - $1) * 1000 / ($3 * 2 * 2707) )); }
out=/tmp/pw.txt
a=$(i2cget -f -y 3 0x6a 0x30)
i2cset -f -y 3 0x6a 0x30 $(printf 0x%02x $((a | 0x04)))
echo "CMD_A $a -> $(i2cget -f -y 3 0x6a 0x30)" > $out
for p in 0 1 0 1; do
	echo $p > /sys/module/sprd_dispc/parameters/panel_power_off
	echo 1 > /sys/class/graphics/fb0/blank
	sleep 5
	c0=$(clb); sleep 20; c1=$(clb)
	echo "awake blank panel_off=$p: $(ma $c0 $c1 20) mA" >> $out
	/tmp/pmic-wdg.sh arm 150 > /dev/kmsg
	echo 0 > /sys/class/rtc/rtc0/wakealarm
	echo +60 > /sys/class/rtc/rtc0/wakealarm
	c0=$(clb); t0=$(date +%s)
	echo mem > /sys/power/state
	c1=$(clb); t1=$(date +%s)
	/tmp/pmic-wdg.sh disarm > /dev/kmsg
	echo "suspend panel_off=$p: $(ma $c0 $c1 $((t1 - t0))) mA over $((t1 - t0)) s" >> $out
	echo 0 > /sys/class/graphics/fb0/blank
	sleep 3
done
echo 0 > /sys/module/sprd_dispc/parameters/panel_power_off
i2cset -f -y 3 0x6a 0x30 $a
echo "restored CMD_A $(i2cget -f -y 3 0x6a 0x30)" >> $out
echo done >> $out
