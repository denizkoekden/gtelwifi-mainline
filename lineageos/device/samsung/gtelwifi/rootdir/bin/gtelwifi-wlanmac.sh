#!/vendor/bin/sh
#
# Give wlan0 the factory address from efs. The NVRAM only carries
# Broadcom's default address, which brcmfmac replaces with a random one
# on every driver load, and Android uses the interface address when MAC
# randomization is off: the router saw a new device after every boot.
# The address itself stays on the device (efs), never in the image.

f=/mnt/vendor/efs/wifi/.mac.info

for i in $(/system/bin/seq 1 120); do
	[ -e /sys/class/net/wlan0 ] && break
	/system/bin/sleep 0.5
done
[ -e /sys/class/net/wlan0 ] && [ -r "$f" ] || exit 0

mac=$(/system/bin/tr -d '\r\n ' < "$f" | /system/bin/tr A-F a-f)
case "$mac" in
??:??:??:??:??:??) ;;
*) exit 0 ;;
esac

# Only while Android has not brought the interface up yet: taking it
# down under a running Wi-Fi stack switches Wi-Fi off
flags=$(/system/bin/cat /sys/class/net/wlan0/flags)
[ $((flags & 1)) -eq 0 ] || exit 0

/system/bin/ip link set wlan0 address "$mac"
