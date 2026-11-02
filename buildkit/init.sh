#!/bin/busybox sh
# gtelwifi /init: set up the USB gadget (CDC-ACM shell + CDC-NCM network),
# then switch to the Alpine root filesystem on userdata. If that is missing
# or failed three boots in a row, stay here as a rescue system: status
# screen, shell on the ACM port, DHCP for the Mac, dropbear SSH.
# Everything we print goes to the kernel log, so it shows up on the
# status screen and in dmesg.

/bin/busybox mkdir -p /sbin /usr/bin /usr/sbin /dev /proc /sys /tmp /run
/bin/busybox --install -s
export PATH=/bin:/sbin:/usr/bin:/usr/sbin

mount -t proc proc /proc
mount -t sysfs sysfs /sys
mount -t devtmpfs devtmpfs /dev
mkdir -p /dev/pts
mount -t devpts devpts /dev/pts
mount -t configfs configfs /sys/kernel/config
mount -t pstore pstore /sys/fs/pstore
mount -t debugfs debugfs /sys/kernel/debug

exec >/dev/kmsg 2>&1
echo "init: userspace up, $(cat /proc/uptime)"

# USB gadget: CDC-ACM shell (macOS: /dev/cu.usbmodem*) + CDC-NCM network.
# Fixed MAC addresses so macOS keeps the same interface across boots.
# The configfs setup lives in the kernel and survives switch_root.
serial=$(sed -n 's/.*androidboot.serialno=\([^ ]*\).*/\1/p' /proc/cmdline)
G=/sys/kernel/config/usb_gadget/g1
mkdir -p $G/strings/0x409 $G/functions/acm.usb0 $G/functions/ncm.usb0 \
	$G/configs/c.1/strings/0x409
echo 0x1d6b > $G/idVendor		# Linux Foundation
echo 0x0104 > $G/idProduct		# Multifunction Composite Gadget
echo 0x0200 > $G/bcdUSB
echo 0xef > $G/bDeviceClass		# miscellaneous: functions use IADs
echo 0x02 > $G/bDeviceSubClass
echo 0x01 > $G/bDeviceProtocol
echo Samsung > $G/strings/0x409/manufacturer
echo "SM-T560 mainline" > $G/strings/0x409/product
echo "${serial:-gtelwifi}" > $G/strings/0x409/serialnumber
echo "ACM shell + NCM" > $G/configs/c.1/strings/0x409/configuration
echo 100 > $G/configs/c.1/MaxPower
echo 02:00:00:73:56:01 > $G/functions/ncm.usb0/dev_addr
echo 02:00:00:73:56:02 > $G/functions/ncm.usb0/host_addr
ln -s $G/functions/acm.usb0 $G/configs/c.1/
ln -s $G/functions/ncm.usb0 $G/configs/c.1/

udc=$(ls /sys/class/udc 2>/dev/null | head -n 1)
if [ -n "$udc" ]; then
	echo "init: binding gadget to UDC $udc"
	echo "$udc" > $G/UDC
	# Fallback for the B-session override (see sc7730.dtsi)
	for r in /sys/class/usb_role/*/role; do
		[ -e "$r" ] && echo device > "$r" && echo "init: $r = $(cat $r)"
	done
else
	echo "init: no UDC, USB device controller did not probe"
fi

# Root filesystem on userdata. The eMMC shows up asynchronously.
ROOTDEV=/dev/mmcblk0p26
for i in $(seq 1 50); do
	[ -b $ROOTDEV ] && break
	sleep 0.1
done
mkdir -p /newroot
why=
if ! [ -b $ROOTDEV ]; then
	why="no $ROOTDEV"
elif ! mount -t ext4 -o ro $ROOTDEV /newroot 2>/dev/null; then
	why="$ROOTDEV has no ext4"
elif ! [ -f /newroot/etc/gtelwifi-rootfs ] || ! [ -x /newroot/sbin/init ]; then
	why="no gtelwifi rootfs on $ROOTDEV"
else
	# Count boots; the rootfs resets this once it is up (local.d)
	n=$(cat /newroot/var/lib/gtelwifi/bootcount 2>/dev/null || echo 0)
	if [ "$n" -ge 3 ]; then
		why="rootfs did not come up $n times in a row"
	else
		mount -o remount,rw /newroot &&
			echo $((n + 1)) > /newroot/var/lib/gtelwifi/bootcount
		mount -o remount,ro /newroot

		# Every other eMMC partition read-only. The whole disk stays
		# writable: a read-only disk makes its partitions read-only too.
		for d in /dev/mmcblk0p* /dev/mmcblk0boot*; do
			[ "$d" = $ROOTDEV ] || blockdev --setro "$d"
		done
		echo "init: eMMC guard done, all but $ROOTDEV read-only"
		echo "init: switching to $(cat /newroot/etc/gtelwifi-rootfs) (boot $((n + 1)))"

		mount --move /dev /newroot/dev
		mount --move /proc /newroot/proc
		mount --move /sys /newroot/sys
		exec switch_root -c /dev/console /newroot /sbin/init
	fi
fi
umount /newroot 2>/dev/null
echo "init: rescue system: $why"

# The status painter owns the screen, take fbcon off it
for b in /sys/class/vtconsole/vtcon*/bind; do
	grep -q 'frame buffer' "${b%/bind}/name" && echo 0 > "$b"
done
/bin/fbstatus &

# eMMC stays read-only here. BLKROSET is per block device since 5.12, and
# late nodes may still appear, so keep catching every mmcblk* for a while.
# tools/tabflash and tools/tabrootfs make their partition writable briefly.
(
	for i in $(seq 1 60); do
		for d in /dev/mmcblk*; do
			[ -b "$d" ] && [ "$(blockdev --getro "$d")" = 0 ] &&
				blockdev --setro "$d"
		done
		sleep 0.5
	done
	n=0
	for d in /dev/mmcblk*; do
		[ -b "$d" ] && [ "$(blockdev --getro "$d")" = 1 ] && n=$((n + 1))
	done
	echo "init: eMMC guard done, $n block devices read-only"
) &

# Network: tablet 10.55.0.1, the Mac gets 10.55.0.2 by DHCP (no router,
# so the Mac keeps its default route). IPv6 link-local works as well.
ip link set lo up		# gets 127.0.0.1 by itself
ifname=$(cat $G/functions/ncm.usb0/ifname 2>/dev/null)
if [ -n "$ifname" ] && ip link show "$ifname" >/dev/null 2>&1; then
	ip addr add 10.55.0.1/24 dev "$ifname"
	ip link set "$ifname" up
	cat > /run/udhcpd.conf <<-EOF
		start 10.55.0.2
		end 10.55.0.2
		interface $ifname
		max_leases 1
		lease_file /run/udhcpd.leases
		option subnet 255.255.255.0
		option lease 86400
	EOF
	touch /run/udhcpd.leases
	udhcpd /run/udhcpd.conf
	echo "init: $ifname up, 10.55.0.1/24, udhcpd for 10.55.0.2"
else
	echo "init: no NCM interface ('$ifname')"
fi

# SSH: key login only (authorized_keys baked into the initramfs)
dropbear -r /etc/dropbear/dropbear_ed25519_host_key -s -E -P /run/dropbear.pid &&
	echo "init: dropbear listening on :22"

# Shell on the ACM port, respawned when the host closes it
(
	while :; do
		if [ -c /dev/ttyGS0 ]; then
			setsid -c sh -l <>/dev/ttyGS0 >&0 2>&1
		fi
		sleep 1
	done
) &

while :; do
	sleep 3600
done
