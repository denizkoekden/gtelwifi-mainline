#!/system/bin/sh
# Bring-up only: keep the kernel log and logcat of the last two boots on
# /metadata (HIDDEN, not encrypted), so a boot that dies before adb comes up
# can be read from the rescue system:
#   mount -o ro,noload /dev/block/by-name/HIDDEN /mnt; ls /mnt/gtelwifi
# kmsg.0/logcat.0* = this boot, *.1 = the boot before. Bounded so a long
# running system cannot fill /metadata. Started from init.gtelwifi.rc.
d=/metadata/gtelwifi
mkdir -p $d
rm -f $d/kmsg.1 $d/logcat.1 $d/logcat.1.* $d/pstore.1.* $d/state.1 $d/screen.1.png
for f in $d/kmsg.0 $d/logcat.0 $d/logcat.0.* $d/pstore.0.* $d/state.0 $d/screen.0.png; do
	[ -e "$f" ] && mv "$f" "$d/$(basename "$f" | sed 's/\.0/.1/')"
done
# Console of the boot before, if it ended in a warm reset (ramoops)
for f in /sys/fs/pstore/*; do
	[ -f "$f" ] && cp "$f" "$d/pstore.0.$(basename "$f")"
done
cat /dev/kmsg | head -c 8388608 > $d/kmsg.0 &
# A hang freezes writeback with it: without this the files ended up to
# 30 s (dirty_expire) before the last thing that happened. The heartbeat
# tells a stalled userspace (beats go on) from a stalled kernel (they stop).
# sync() writes out every file system, /data included. Once a second it
# competes with dex2oat, PackageManager and the first-boot writes on the
# slow eMMC: every 5th beat while booting, every 15th afterwards
# (sys.boot_completed is looked at every 5th beat only, no fork per second).
(n=0; boot=0
while :; do
	sleep 1
	echo "<6>gtelwifi-bootlog: alive $(cut -d' ' -f1 /proc/uptime)" > /dev/kmsg
	n=$((n + 1))
	if [ $((n % 5)) -eq 0 ] && [ $boot -eq 0 ] && [ "$(/system/bin/getprop sys.boot_completed)" = 1 ]; then
		boot=1
	fi
	step=15
	[ $boot -eq 0 ] && step=5
	[ $((n % step)) -eq 0 ] && sync
done) &
# Display/boot state some time after boot_completed (issue #9: SM-T561 shows
# the boot animation, then stays black). Tells "Android runs but the panel is
# dark" (screencap shows the UI, wakefulness, backlight, DRM state) from
# "Android is stuck or crash looping", and records how long the command line
# was (CONFIG_CMDLINE_EXTEND cuts at 1023 characters).
(
	n=0
	while [ "$(/system/bin/getprop sys.boot_completed)" != 1 ] && [ $n -lt 600 ]; do
		sleep 1
		n=$((n + 1))
	done
	sleep 45
	{
		echo "== uptime $(cut -d' ' -f1 /proc/uptime), boot_completed wait ${n}s"
		echo "== cmdline $(wc -c < /proc/cmdline) chars"; cat /proc/cmdline
		echo "== getenforce $(/system/bin/getenforce)"
		/system/bin/getprop | grep -E "ro\.boot\.|ro\.hardware|sys\.boot|init\.svc\.(zygote|surfaceflinger|bootanim)"
		echo "== backlight"; for f in brightness bl_power actual_brightness; do
			echo "$f $(cat /sys/class/backlight/backlight/$f)"; done
		echo "panel_power_off $(cat /sys/module/sprd_dispc/parameters/panel_power_off)"
		echo "gpu_khz $(cat /sys/module/sc7730/parameters/gpu_khz)"
		echo "== dri state"; cat /sys/kernel/debug/dri/*/state 2>&1 | head -60
		echo "== power"; /system/bin/dumpsys power | grep -E "mWakefulness|Display Power|mHoldingDisplay|mScreenOn"
		echo "== display"; /system/bin/dumpsys display | grep -E "mState|mScreenState|Display Power|STATE|mBrightness" | head -20
		echo "== SurfaceFlinger"; /system/bin/dumpsys SurfaceFlinger | head -120
		echo "== crashes"; ls -l /data/tombstones /data/anr 2>&1 | head -20
		echo "== iomem"; cat /proc/iomem
		echo "== top"; /system/bin/top -b -n 1 -m 12 | head -25
		echo "== interrupts"; cat /proc/interrupts
	} > $d/state.0 2>&1
	/system/bin/screencap -p $d/screen.0.png 2>/dev/null
	sync
) &
until /system/bin/logcat -b all -v threadtime -f $d/logcat.0 -r 4096 -n 2; do
	sleep 1
done
