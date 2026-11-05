#!/system/bin/sh
# Bring-up only: keep the kernel log and logcat of the last two boots on
# /metadata (HIDDEN, not encrypted), so a boot that dies before adb comes up
# can be read from the rescue system:
#   mount -o ro,noload /dev/block/by-name/HIDDEN /mnt; ls /mnt/gtelwifi
# kmsg.0/logcat.0* = this boot, *.1 = the boot before. Bounded so a long
# running system cannot fill /metadata. Started from init.gtelwifi.rc.
d=/metadata/gtelwifi
mkdir -p $d
rm -f $d/kmsg.1 $d/logcat.1 $d/logcat.1.* $d/pstore.1.*
for f in $d/kmsg.0 $d/logcat.0 $d/logcat.0.* $d/pstore.0.*; do
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
(while :; do
	sleep 1
	echo "<6>gtelwifi-bootlog: alive $(cut -d' ' -f1 /proc/uptime)" > /dev/kmsg
	sync
done) &
until /system/bin/logcat -b all -v threadtime -f $d/logcat.0 -r 4096 -n 2; do
	sleep 1
done
