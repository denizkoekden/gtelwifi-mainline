#!/bin/bash
# Install LineageOS from a tablet running Nura or our rescue system (as
# tools/tabflash does for the KERNEL partition). Runs on the Mac.
#
#   TABSSH=tools/nurassh lineageos/scripts/flash-system.sh system <super_SYSTEM.raw.gz> <sha256 of the raw image>
#   TABSSH=tools/nurassh lineageos/scripts/flash-system.sh metadata <metadata.raw.gz> <sha256>
#   TABSSH=tools/nurassh lineageos/scripts/flash-system.sh wipe-userdata yes-wipe
#
# system: super spans SYSTEM + CSC, but every logical partition lives on
# SYSTEM (lpdump), so only SYSTEM is written; CSC stays as it is. The
# partition is looked up by its GPT name, must not be mounted and must be
# exactly as large as the image. The image is streamed (gunzip on the
# tablet), written with fsync, read back past the page cache and compared
# with the hash of the raw image.
#
# metadata: writes an empty ext4 (mke2fs on the build host, 150 MiB) to
# HIDDEN (Samsung's preload before), checked the same way. First stage init
# never formats /metadata ("ignoring mount for formattable partition") and
# aconfigd needs it in early-init, so it has to exist before the first boot,
# as fastboot -w would leave it.
#
# wipe-userdata: zeroes the first 16 MiB of userdata (Nura's inner GPT lived
# there) and reboots at once. fs_mgr formats /data on the first boot only if
# it looks wiped (formattable + partition_wiped()). This ends Nura on the
# tablet; run it last, after KERNEL holds the LineageOS boot.img
# (tools/tabflash).
#
# Bootloader, PARAM, PIT and all other partitions are never touched.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
TABSSH=${TABSSH:-$ROOT/tools/tabssh}
mode=${1:?usage: flash-system.sh system <img.gz> <sha256> | wipe-userdata yes-wipe}

# shellcheck disable=SC2016  # expanded on the tablet
"$TABSSH" 'cat > /tmp/los-flash.sh' <<'EOF'
set -e
part() {
	for u in /sys/class/block/mmcblk0p*/uevent; do
		grep -qx "PARTNAME=$1" "$u" && { echo "/dev/$(sed -n 's/^DEVNAME=//p' "$u")"; return; }
	done
	echo "tablet: no $1 partition" >&2
	exit 1
}
bytes() { echo $(( $(cat "/sys/class/block/${1#/dev/}/size") * 512 )); }
# our rescue system re-applies read-only to every mmcblk* for 30 s after
# boot (as in tools/tabflash); Nura has no such guard
i=0
dmesg | grep -q 'init: binding gadget' &&
until dmesg | grep -q 'init: eMMC guard done'; do
	i=$((i + 1))
	[ $i -le 40 ] || { echo "tablet: eMMC guard still running"; exit 1; }
	sleep 1
done
case "$1" in
check)
	P=$(part "$2")
	[ "$(bytes "$P")" = "$3" ] || { echo "tablet: $P has $(bytes "$P") bytes, image $3"; exit 1; }
	grep -q "^$P " /proc/mounts && { echo "tablet: $P is mounted"; exit 1; }
	echo "tablet: $P is $2, $3 bytes, not mounted"
	;;
write)
	P=$(part "$2")
	disk_ro=$(blockdev --getro /dev/mmcblk0)
	part_ro=$(blockdev --getro "$P")
	blockdev --setrw /dev/mmcblk0
	blockdev --setrw "$P"
	if ! gunzip -c | dd of="$P" bs=1M conv=fsync 2>/tmp/los-dd.err; then
		cat /tmp/los-dd.err
		echo "tablet: WRITE FAILED - $2 is partial, write it again"
		exit 1
	fi
	tail -n 1 /tmp/los-dd.err
	if [ "$part_ro" = 1 ]; then blockdev --setro "$P"; fi
	if [ "$disk_ro" = 1 ]; then blockdev --setro /dev/mmcblk0; fi
	sync
	echo 3 > /proc/sys/vm/drop_caches
	head -c "$(bytes "$P")" "$P" | sha256sum | cut -d' ' -f1
	;;
wipe)
	blockdev --setrw /dev/mmcblk0
	P=$(part userdata)
	blockdev --setrw "$P"
	dd if=/dev/zero of="$P" bs=1M count=16 conv=fsync 2>/dev/null
	echo "tablet: first 16 MiB of $P (userdata) zeroed"
	sync
	echo "tablet: rebooting"
	# our kernel has no MAGIC_SYSRQ (no /proc/sysrq-trigger, 04.11.);
	# rebootcmd in the rescue system, systemd's reboot under Nura
	rebootcmd 2>/dev/null || reboot -f
	;;
esac
EOF

case "$mode" in
system|metadata)
	name=SYSTEM
	[ "$mode" = metadata ] && name=HIDDEN
	gz=${2:?image} hash=${3:?sha256 of the raw image}
	size=$(gzip -dc "$gz" | wc -c | tr -d ' ')
	echo "image: $gz, raw $size bytes"
	"$TABSSH" "sh /tmp/los-flash.sh check $name $size"
	got=$("$TABSSH" "sh /tmp/los-flash.sh write $name" < "$gz" | tail -n 1)
	if [ "$got" = "$hash" ]; then
		echo "tablet: read back ok, $name holds $gz"
	else
		echo "tablet: READ BACK MISMATCH ($got), do not reboot into LineageOS" >&2
		exit 1
	fi
	;;
wipe-userdata)
	[ "${2:-}" = yes-wipe ] || { echo "wipe-userdata needs the argument yes-wipe" >&2; exit 2; }
	"$TABSSH" 'setsid sh /tmp/los-flash.sh wipe </dev/null >/dev/null 2>&1 &' || true
	echo "wipe started, the tablet reboots"
	;;
*)
	echo "unknown mode $mode" >&2
	exit 2
	;;
esac
