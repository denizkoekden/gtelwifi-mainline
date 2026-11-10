#!/bin/bash
# Audio register dump of the T560 under Android (adb root), before/after a sound.
# Usage: tools/lab/audio-regdump.sh <tag>   -> dumps/audio-regdump-<tag>.txt
# PMIC (SC2723) via regmap debugfs: analog codec 0x600-0x6ac, globals 0x800-0x9fc.
# VBC (0x40020000) and codec digital part DP (0x40000000) via devmem; both are
# clocked from the VBC probe on, so reading them is safe (no MM block).
set -e
S=38019a9d7112a300
tag=${1:?tag}
out=$(dirname "$0")/../../dumps/audio-regdump-$tag.txt
adb -s $S shell 'mountpoint -q /sys/kernel/debug || mount -t debugfs none /sys/kernel/debug
echo "== $(uname -v) uptime $(cut -d" " -f1 /proc/uptime) boot_id $(cat /proc/sys/kernel/random/boot_id)"
echo "== dapm"; cat /sys/devices/platform/sound/vbc-playback-sc2723-hifi/dapm_widget
echo "== pmic"; awk -F: "{a=strtonum(\"0x\"\$1)} a>=0x600 && a<0xa00" /sys/kernel/debug/regmap/pmic/registers 2>/dev/null ||
  grep -E "^(6|7|8|9)[0-9a-f][0-9a-f]:" /sys/kernel/debug/regmap/pmic/registers
echo "== vbc"; o=0; while [ $o -le 828 ]; do
  if [ $o -le 200 ] || [ $o -ge 256 ]; then printf "%03x: %s\n" $o $(devmem $((0x40020000+o)) 4); fi; o=$((o+4)); done
echo "== dp"; o=0; while [ $o -le 60 ]; do printf "%02x: %s\n" $o $(devmem $((0x40000000+o)) 4); o=$((o+4)); done' > "$out"
echo "$out: $(wc -l < "$out") Zeilen"
