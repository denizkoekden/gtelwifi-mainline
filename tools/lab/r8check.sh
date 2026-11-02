#!/bin/sh
# run on the tablet under Nura
echo "== kernel"; uname -rv; nproc; systemctl is-system-running; systemctl --failed --no-legend
echo "== cfi"; dmesg | grep -ci "cfi failure"
echo "== warn/err"; dmesg | grep -iE "warn|error|fail" | grep -viE "cfi failure" | head -30
echo "== fgu"; dmesg | grep -iE "sc2723|fgu" | head
echo "== battery"; for f in status capacity temp health voltage_now current_now; do printf '%s=' $f; cat /sys/class/power_supply/battery/$f 2>&1; done
echo "== charger"; for f in online status charge_behaviour; do printf '%s=' $f; cat /sys/class/power_supply/smb347-usb/$f 2>&1; done
echo "== backlight"; ls /sys/class/backlight; for d in /sys/class/backlight/*; do echo "$d $(cat $d/brightness)/$(cat $d/max_brightness) bl_power=$(cat $d/bl_power)"; done
echo "== mmc"; ls /sys/bus/mmc/devices; lsblk -o NAME,SIZE,FSTYPE,MOUNTPOINTS 2>/dev/null | grep -v loop
echo "== bt"; dmesg | grep -iE "bluetooth|hci|bcm" | tail -8; ls /sys/class/bluetooth 2>&1
echo "== iio"; for d in /sys/bus/iio/devices/iio:device*; do echo "$d $(cat $d/name)"; done
echo "== modules"; lsmod | awk 'NR>1{print $1}' | sort | tr '\n' ' '; echo
echo "== upower"; upower -d 2>/dev/null | grep -E "native-path|percentage|temperature|state" | head
echo "== sensor-proxy"; systemctl is-active iio-sensor-proxy 2>&1
