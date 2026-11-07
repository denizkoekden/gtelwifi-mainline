#!/system/bin/sh
# Pick the fstab for mount_all (init.gtelwifi.rc, "on fs"): /data with
# file-based encryption (default) or without it, as chosen in the TWRP
# installer, which leaves /metadata/gtelwifi/noencrypt for the latter.
# /metadata is mounted in the first stage, before "on fs".
if [ -e /metadata/gtelwifi/noencrypt ]; then
	setprop vendor.gtelwifi.fstab gtelwifi.noencrypt
	echo "gtelwifi-crypt: /data without encryption" > /dev/kmsg
else
	setprop vendor.gtelwifi.fstab gtelwifi
fi
