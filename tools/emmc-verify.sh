#!/bin/bash
# Check the eMMC read path of our kernel: hash every small partition on the
# tablet and compare with the raw dumps TWRP made (backup/raw/SHA256SUMS).
# KERNEL is skipped: it holds our test image now, not the stock dump.
#   tools/emmc-verify.sh
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
SUMS=$ROOT/backup/raw/SHA256SUMS
ok=0 bad=0

remote=$("$ROOT/tools/tabssh" '
	for u in /sys/class/block/mmcblk0p*/uevent /sys/class/block/mmcblk0boot*/uevent; do
		dev=$(sed -n "s/^DEVNAME=//p" $u); name=$(sed -n "s/^PARTNAME=//p" $u)
		size=$(cat ${u%/uevent}/size)
		[ $size -le 40960 ] || continue            # 20 MiB in 512-byte sectors
		echo "$(sha256sum /dev/$dev | cut -d" " -f1) ${name:-$dev} $dev"
	done') || { echo "tabssh failed"; exit 1; }

while read -r hash name dev; do
	[ "$name" = KERNEL ] && continue
	want=$(awk -v f="$name.img" '$2 == f || $2 == "*"f { print $1 }' "$SUMS")
	if [ -z "$want" ]; then
		echo "  ?    $name ($dev): no dump"
	elif [ "$hash" = "$want" ]; then
		echo "  ok   $name ($dev)"; ok=$((ok + 1))
	else
		echo "  DIFF $name ($dev)"; bad=$((bad + 1))
	fi
done <<< "$remote"
echo "== $ok identical, $bad different"
[ "$bad" = 0 ]
