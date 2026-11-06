#!/bin/bash
# Flash bundle and TWRP zips from a finished build. Runs on the build host in
# the LineageOS tree (push-to-buildhost.sh installs it there with mkzip.py and
# the static update-binary):
#
#   /mnt/datengrab/lineage/mkflash.sh <tag>      e.g. 20261105aj
#
# Writes flash-<tag>/ with boot.img, super_SYSTEM.raw.gz (for
# flash-system.sh), SHA256SUMS (boot.img and the raw SYSTEM image, whose hash
# flash-system.sh checks) and the TWRP packages
# lineage-24.0-<tag>-gtelwifi-{install,update}.zip with twrp.sha256.
# super spans SYSTEM + CSC, but every logical partition lives on SYSTEM, so
# only super_SYSTEM of the split super image is used.
set -euo pipefail
T=${1:?usage: mkflash.sh <tag>}
cd /mnt/datengrab/lineage
P=out/target/product/gtelwifi; B=out/host/linux-x86/bin; export PATH=$PWD/$B:$PATH
rm -rf $P/super_split; mkdir -p $P/super_split; build_super_image $P/super_split.info $P/super_split >/dev/null 2>&1
simg2img $P/super_split/super_SYSTEM.img $P/super_split/super_SYSTEM.raw
F=flash-$T; mkdir -p $F; cp $P/boot.img $F/; gzip -1 -c $P/super_split/super_SYSTEM.raw > $F/super_SYSTEM.raw.gz
{ (cd $F && sha256sum boot.img); (cd $P/super_split && sha256sum super_SYSTEM.raw); } > $F/SHA256SUMS
cat $F/SHA256SUMS
python3 mkzip.py update-binary $F/boot.img $P/super_split/super_SYSTEM.img $F lineage-24.0-$T-gtelwifi > $F/twrp.sha256
cat $F/twrp.sha256
