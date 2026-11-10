#!/bin/bash
# Runs on the build host (called by push-to-buildhost.sh): local bare repos
# for device/samsung/gtelwifi and kernel/samsung/gtelwifi under $1, branch
# lineage-24.0, as lineageos/local_manifests/gtelwifi.xml expects them.
# The device tree keeps its history (one commit per change), the kernel is
# rebuilt from the kernel.org tarball + patch series only if the series changed.
set -euo pipefail
SRC=$1 KVER=$2 KSHA512=$3
STAGE=$SRC/stage BR=lineage-24.0
export GIT_CONFIG_GLOBAL=${GIT_CONFIG_GLOBAL:-/mnt/datengrab/lineage/.gitconfig.lineage}
work=$(mktemp -d -p "$SRC"); trap 'rm -rf "$work"' EXIT

bare() {
	[ -d "$SRC/$1.git" ] || git init -q --bare -b $BR "$SRC/$1.git"
	echo "$SRC/$1.git"
}

# Device tree
dev=$(bare android_device_samsung_gtelwifi)
git clone -q "$dev" "$work/device" 2>/dev/null || git init -q -b $BR "$work/device"
git -C "$work/device" checkout -q -B $BR
rsync -a --delete --exclude .git "$STAGE/device/" "$work/device/"
git -C "$work/device" add -A
if git -C "$work/device" diff --cached --quiet && git -C "$work/device" rev-parse -q --verify HEAD >/dev/null; then
	echo "device: unchanged"
else
	git -C "$work/device" commit -q -m "gtelwifi: device tree for lineage-24.0 (draft)"
	git -C "$work/device" push -q "$dev" HEAD:refs/heads/$BR
fi
echo "device: $(git -C "$dev" log -1 --format='%h %s' $BR)"

# Kernel
kernel_patches=("$STAGE"/patches/*.patch)
if [ -f "$STAGE/kernel-followups/series" ]; then
	while IFS= read -r patch; do
		case "$patch" in ''|'#'*) continue ;; esac
		[[ "$patch" != */* && "$patch" == *.patch ]] || { echo "Invalid kernel follow-up: $patch" >&2; exit 1; }
		kernel_patches+=("$STAGE/kernel-followups/$patch")
	done < "$STAGE/kernel-followups/series"
fi
stamp=$(cat "${kernel_patches[@]}" | sha256sum | cut -c1-16)
ker=$(bare android_kernel_samsung_gtelwifi)
if [ "$(git -C "$ker" log -1 --format=%b $BR 2>/dev/null | sed -n 's/^Series: //p')" = "$stamp" ]; then
	echo "kernel: unchanged (series $stamp)"
else
	tarball=$SRC/linux-$KVER.tar.xz
	[ -f "$tarball" ] || curl -sSfL -o "$tarball" "https://cdn.kernel.org/pub/linux/kernel/v${KVER%%.*}.x/linux-$KVER.tar.xz"
	echo "$KSHA512  $tarball" | sha512sum -c --quiet
	tar -C "$work" -xf "$tarball"
	k=$work/linux-$KVER
	git -C "$k" init -q -b $BR
	git -C "$k" add -A
	git -C "$k" commit -q -m "Linux $KVER"
	git -C "$k" am -q "${kernel_patches[@]}"
	git -C "$k" commit -q --allow-empty -m "gtelwifi: patch series stamp" -m "Series: $stamp"
	git -C "$k" push -q --force "$ker" HEAD:refs/heads/$BR
	echo "kernel: $(git -C "$k" rev-list --count HEAD) commits, series $stamp"
fi
