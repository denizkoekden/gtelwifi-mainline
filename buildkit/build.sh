#!/bin/sh
# Build the gtelwifi boot image from a Linux v7.3.1 tree with our patches.
# Needs: arm-linux-gnueabihf-gcc, dtc, python3, a static armhf busybox.
# Usage: ./build.sh /path/to/linux
# In Docker: docker/run.sh env O=/src/kout OVERLAY=/work/kernel ./buildkit/build.sh /src/linux
# (O= in the volume: the bind-mounted project dir is case-insensitive APFS;
#  OVERLAY= is copied over the tree first, so kernel files can be edited on the host)
# LLVM=1 builds the kernel with clang/lld; EXTRA_CONFIG= adds a fragment
# (e.g. buildkit/cfi.config, which needs clang).
set -e
K=$1; HERE=$(cd "$(dirname "$0")" && pwd); O=${O:-$HERE/kout}
BUSYBOX=${BUSYBOX:-/bin/busybox}
MAKE="make -C $K ARCH=arm CROSS_COMPILE=arm-linux-gnueabihf- O=$O ${LLVM:+LLVM=$LLVM}"

[ -n "$OVERLAY" ] && cp -a "$OVERLAY"/. "$K"/

# rescue initramfs: SSH logins with your public key only, host key made once
if [ ! -s "$HERE/ssh/authorized_keys" ]; then
	echo "put your SSH public key into $HERE/ssh/authorized_keys" \
	     "(e.g. ~/.ssh/id_ed25519.pub); the rescue system allows key logins only" >&2
	exit 1
fi
[ -s "$HERE/ssh/dropbear_ed25519_host_key" ] ||
	dropbearkey -t ed25519 -f "$HERE/ssh/dropbear_ed25519_host_key" >/dev/null

arm-linux-gnueabihf-gcc -static -O2 -Wall -o "$HERE/fbstatus" "$HERE/init.c"
arm-linux-gnueabihf-strip "$HERE/fbstatus"
arm-linux-gnueabihf-gcc -static -O2 -Wall -o "$HERE/rngseed" "$HERE/rngseed.c"
arm-linux-gnueabihf-strip "$HERE/rngseed"
arm-linux-gnueabihf-gcc -static -O2 -Wall -o "$HERE/rebootcmd" "$HERE/rebootcmd.c"
arm-linux-gnueabihf-strip "$HERE/rebootcmd"
sed -e "s|@HERE@|$HERE|g" -e "s|@BUSYBOX@|$BUSYBOX|g" \
	"$HERE/initramfs.list" > "$HERE/initramfs.local"
# dynamically linked Debian armhf binaries plus their libraries
python3 "$HERE/libdeps.py" /usr/sbin/dropbear:/sbin/dropbear >> "$HERE/initramfs.local"
sed "s|@INITRAMFS@|$HERE/initramfs.local|" "$HERE/gtelwifi.config" > "$HERE/config.local"
[ -n "$EXTRA_CONFIG" ] && cat "$EXTRA_CONFIG" >> "$HERE/config.local"

$MAKE allnoconfig
"$K/scripts/kconfig/merge_config.sh" -m -O "$O" "$O/.config" "$HERE/config.local"
$MAKE olddefconfig
# merge_config only warns; fail if a requested option did not stick
# (the last value of an option counts, as in merge_config)
grep -E '^CONFIG_[A-Z0-9_]+=' "$HERE/config.local" |
	awk -F= '{ v[$1] = $0; if (!($1 in o)) { o[$1] = n++; k[n - 1] = $1 } }
		END { for (i = 0; i < n; i++) print v[k[i]] }' |
	grep -v '=n$' | while IFS= read -r want; do
	grep -qxF "$want" "$O/.config" || { echo "config: $want did not stick" >&2; exit 1; }
done
$MAKE -j"$(nproc)" zImage dtbs
python3 "$HERE/mkimage.py" --zimage "$O/arch/arm/boot/zImage" \
  --dtb "$O/arch/arm/boot/dts/unisoc/sc7730-samsung-gtelwifi.dtb" -o "$HERE/gtelwifi-mainline.img"
