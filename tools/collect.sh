#!/bin/bash
# Bestandsaufnahme des SM-T560 per adb, gegen Android oder TWRP.
# Liest nur. Auf dem Gerät wird nichts geschrieben; Partitionen werden per
# "adb exec-out dd" direkt auf den Mac gestreamt (nur mit root, also TWRP).
#
# Aufruf: tools/collect.sh [zielordner]
set -u
ADB=${ADB:-adb}
ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT=${1:-$ROOT/dumps/$(date +%Y%m%d-%H%M%S)}
MAX_KB=131072	# Partitionen bis 128 MiB sichern, groessere macht TWRP

state=$($ADB get-state 2>/dev/null)
case $state in
	device|recovery) ;;
	*) echo "Kein Geraet per adb erreichbar (state='$state')." >&2; exit 1 ;;
esac
mkdir -p "$OUT"
echo "Modus: $state -> $OUT"

sh_() { $ADB shell "$@" 2>&1 | tr -d '\r'; }
cap() { local f=$1; shift; sh_ "$@" > "$OUT/$f.txt"; }

is_root=0
sh_ id | grep -q 'uid=0(' && is_root=1
echo "root: $is_root"

cap cmdline     cat /proc/cmdline
cap version     cat /proc/version
cap cpuinfo     cat /proc/cpuinfo
cap meminfo     cat /proc/meminfo
cap iomem       cat /proc/iomem
cap interrupts  cat /proc/interrupts
cap partitions  cat /proc/partitions
cap mounts      cat /proc/mounts
cap by-name     'ls -l /dev/block/platform/*/by-name/'
cap getprop     getprop
cap dmesg       dmesg
cap last_kmsg   'cat /proc/last_kmsg'

# Device Tree, wie ihn der Bootloader an den laufenden Kernel uebergeben hat
$ADB pull /proc/device-tree/ "$OUT/device-tree" >/dev/null 2>&1
[ "$is_root" = 1 ] && $ADB pull /sys/firmware/fdt "$OUT/fdt.dtb" >/dev/null 2>&1
if [ -d "$OUT/device-tree" ] && command -v dtc >/dev/null; then
	dtc -q -I fs -O dts -o "$OUT/device-tree.dts" "$OUT/device-tree" 2>/dev/null
fi

if [ "$is_root" = 1 ]; then
	mkdir -p "$OUT/backup"
	# "NAME -> /dev/block/mmcblk0pN" aus beiden ls-Varianten (toolbox/toybox)
	grep -- ' -> ' "$OUT/by-name.txt" |
		sed -E 's/.* ([^ ]+) -> ([^ ]+)$/\1 \2/' > "$OUT/by-name.map"
	for hw in mmcblk0boot0 mmcblk0boot1; do
		echo "$hw /dev/block/$hw"
	done >> "$OUT/by-name.map"

	while read -r name dev; do
		kb=$(awk -v b="${dev##*/}" '$4 == b { print $3 }' "$OUT/partitions.txt")
		[ -n "$kb" ] || continue
		if [ "$kb" -gt "$MAX_KB" ]; then
			echo "  skip $name ($((kb / 1024)) MiB)"
			continue
		fi
		f="$OUT/backup/$name.img"
		$ADB exec-out "dd if=$dev bs=1048576 2>/dev/null" > "$f" < /dev/null
		got=$(stat -f %z "$f")
		if [ "$got" -eq $((kb * 1024)) ]; then
			echo "  ok   $name ($dev, $((kb / 1024)) MiB)"
		else
			echo "  FEHLER $name: $got statt $((kb * 1024)) Bytes" >&2
		fi
	done < "$OUT/by-name.map"
	(cd "$OUT/backup" && shasum -a 256 *.img > SHA256SUMS)
else
	echo "Kein root: keine Partitions-Backups (dafuer in TWRP erneut laufen lassen)."
fi

echo
echo "== Zusammenfassung =="
echo "cmdline: $(cat "$OUT/cmdline.txt")"
tr ' ' '\n' < "$OUT/cmdline.txt" | grep -E '^(lcd_base|lcd_id)=' | sed 's/^/  /'
if [ -f "$OUT/device-tree/sprd,sc-id" ]; then
	echo "sprd,sc-id: $(xxd -p "$OUT/device-tree/sprd,sc-id")  (chipset, board-rev, soc-rev)"
fi
[ -f "$OUT/device-tree/model" ] && echo "model: $(tr -d '\0' < "$OUT/device-tree/model")"
grep -iE ' (boot|kernel|recovery) -> ' "$OUT/by-name.txt" | sed 's/^/  /'
for b in KERNEL BOOT boot kernel; do
	if [ -s "$OUT/backup/$b.img" ]; then
		echo
		python3 "$ROOT/tools/bootimg_info.py" "$OUT/backup/$b.img" \
			--extract "$OUT/stock-boot"
		break
	fi
done
