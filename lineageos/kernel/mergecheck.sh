#!/bin/bash
# Merge the LineageOS kernel config fragments against the 7.3.1 tree in the
# gtelwifi-linux volume the way buildkit/build.sh does (allnoconfig, merge,
# olddefconfig) and list what does not come through. Read-only: the
# volume and the project are mounted ro, the output dir lives in the
# container and only .config is copied back (to lineageos/kernel/out/).
# savedefconfig of the result goes to the device tree as the one base config
# (allnoconfig + fragments, so kernel.mk does not depend on Kconfig defaults).
#
# Order follows LineageOS device/mainline/generic (Generic_arm64/BoardConfig.mk):
# device config, kernel/mainline/configs "android-base-pre", AOSP
# android-base (android17-release, b/android-6.12), the conditional
# fragments whose condition holds here (CONFIG_ARM, EXT4_FS, F2FS_FS),
# kernel/mainline/configs common, then our overrides. condcheck.py then
# checks the result against AOSP's conditional list itself (EXT4_FS_POSIX_ACL
# was missing on 04.11.: vold could not set the ACL on /data/media/0).
# Usage: lineageos/kernel/mergecheck.sh
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
LOS=/work/lineageos/ref/LineageOS_android_kernel_mainline_configs/fragments
mkdir -p "$ROOT/lineageos/kernel/out"
docker run --rm -v gtelwifi-linux:/src:ro -v "$ROOT:/work:ro" \
	-v "$ROOT/lineageos/kernel/out:/out" -e ARCH=arm -e LLVM=1 -e LOS="$LOS" gtelwifi-build sh -c '
	cd /src/linux && mkdir -p /tmp/k
	# Same steps as buildkit/build.sh, which builds the kernel tested under
	# Nura: allnoconfig, merge, olddefconfig. Options that allnoconfig could
	# not see yet keep their Kconfig default. merge_config -n (allnoconfig
	# with all fragments at once) set every visible option to n instead; with
	# EXPERT=y from AOSP that included ARM_THUMB, and Android userspace is
	# Thumb-2 (first boot 04.11.: /init failed, kernel panic, boot loop).
	frags="/work/buildkit/gtelwifi.config /work/buildkit/cfi.config
		$LOS/android-base-pre/common.config
		/work/lineageos/kernel/aosp/android17-release_b-6.12_android-base.config
		$LOS/android-base-conditional/CONFIG_ARM-y.config
		$LOS/android-base-conditional/CONFIG_EXT4_FS-y.config
		$LOS/android-base-conditional/CONFIG_F2FS_FS-y.config
		$LOS/common.config
		/work/lineageos/kernel/gtelwifi-android.config"
	make -s O=/tmp/k allnoconfig
	scripts/kconfig/merge_config.sh -m -O /tmp/k /tmp/k/.config $frags >/tmp/log 2>&1
	make -s O=/tmp/k olddefconfig
	# requested value (the last one counts) against the result
	cat $frags | grep -E "^(CONFIG_[A-Z0-9_]+=|# CONFIG_[A-Z0-9_]+ is not set)" |
		sed -E "s/^# (CONFIG_[A-Z0-9_]+) is not set/\1=n/" |
		awk -F= "{ v[\$1] = \$0; if (!(\$1 in o)) { o[\$1] = n++; k[n - 1] = \$1 } }
			END { for (i = 0; i < n; i++) print v[k[i]] }" |
	while IFS= read -r want; do
		sym=${want%%=*}
		got=$(grep -E "^$sym=" /tmp/k/.config || true)
		case "$want" in
		*=n) [ -z "$got" ] || echo "$want -> $got" ;;
		*) [ "$got" = "$want" ] || echo "$want -> ${got:-not set}" ;;
		esac
	done
	cp /tmp/k/.config /out/merged.config
	make -s O=/tmp/k savedefconfig && cp /tmp/k/defconfig /out/gtelwifi_android_defconfig'
cp "$ROOT/lineageos/kernel/out/gtelwifi_android_defconfig" "$ROOT/lineageos/device/samsung/gtelwifi/configs/kernel/"
echo "merged config: lineageos/kernel/out/merged.config ($(grep -c '^CONFIG_' "$ROOT/lineageos/kernel/out/merged.config") options)"
echo "defconfig:     lineageos/device/samsung/gtelwifi/configs/kernel/gtelwifi_android_defconfig"
python3 -I "$ROOT/lineageos/kernel/condcheck.py" \
	"$ROOT/lineageos/kernel/aosp/android17-release_b-6.12_android-base-conditional.xml" \
	"$ROOT/lineageos/kernel/out/merged.config"
