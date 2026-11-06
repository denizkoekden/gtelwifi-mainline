#!/bin/bash
# Builds our TWRP on a TWRP 12.1 tree (README: "Building"). Runs on the build
# host; the device tree and patches are copied over first:
#
#   build-twrp.sh <twrp tree> <device/samsung/gtelwifi copy> <patches dir>
#
# Installs the device tree, applies the patches under patches/<project>/ that
# are not applied yet, removes the cached recovery root (TWRP copies libraries
# into it only when it is created) and runs mka recoveryimage. The image:
# <tree>/out/target/product/gtelwifi/recovery.img
set -euo pipefail
T=$(realpath "${1:?twrp tree}")
DEV=$(realpath "${2:?device tree}")
P=$(realpath "${3:?patches dir}")
export GIT_CONFIG_GLOBAL=${GIT_CONFIG_GLOBAL:-$T/.gitconfig.twrp}

rm -rf "$T/device/samsung/gtelwifi"
mkdir -p "$T/device/samsung"
cp -a "$DEV" "$T/device/samsung/gtelwifi"

# patches/<project path with / as _>/*.patch, e.g. bootable_recovery, system_core
for d in "$P"/*/; do
	proj=$(basename "$d" | tr _ /)
	cd "$T/$proj"
	for p in "$d"*.patch; do
		if git apply -R --check "$p" 2>/dev/null; then
			echo "already applied: $proj/$(basename "$p")"
		else
			git am -3 "$p"
		fi
	done
done

cd "$T"
rm -rf out/target/product/gtelwifi/root out/target/product/gtelwifi/recovery \
	out/target/product/gtelwifi/obj/PACKAGING/recovery_intermediates
export ALLOW_MISSING_DEPENDENCIES=true
set +u
source build/envsetup.sh
lunch twrp_gtelwifi-eng
mka recoveryimage
ls -l out/target/product/gtelwifi/recovery.img
sha256sum out/target/product/gtelwifi/recovery.img
