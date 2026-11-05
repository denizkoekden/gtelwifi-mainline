#!/bin/bash
# Kernel for the TWRP image (prebuilt/zImage): the LineageOS kernel with the
# exact .config of a LineageOS build, only compressed with XZ instead of gzip
# (7.4 -> 5.7 MB). With gzip the recovery image does not fit the 16 MiB
# RECOVERY partition. Runs on the build host, in the LineageOS tree:
#
#   build-kernel.sh <lineage tree> <out dir>
#
# The environment mirrors vendor/lineage/config/BoardConfigKernel.mk (prebuilt
# make, clang, flex/bison, bc), except: no glibc 2.17 sysroot for host tools
# (gen_init_cpio needs copy_file_range) and the host's xz (the prebuilt one
# lacks the ARM BCJ filter that xz_wrap.sh asks for).
set -euo pipefail
B=$(cd "${1:?lineage tree}" && pwd)
O=${2:?out dir}
mkdir -p "$O"
cp "$B/out/target/product/gtelwifi/obj/KERNEL_OBJ/.config" "$O/.config"
export HIP_PATH=none PERL5LIB=$B/prebuilts/tools-lineage/common/perl-base \
	BISON_PKGDATADIR=$B/prebuilts/build-tools/common/bison
export PATH=$B/prebuilts/kernel-build-tools/linux-x86/bin:$B/prebuilts/tools-lineage/linux-x86/bin:$B/prebuilts/build-tools/linux-x86/bin:$B/prebuilts/build-tools/path/linux-x86:$B/prebuilts/clang/host/linux-x86/clang-r596125/bin:$B/prebuilts/clang-tools/linux-x86/bin:$PATH
export LD_LIBRARY_PATH=$B/prebuilts/clang/host/linux-x86/clang-r596125/lib64
K=$B/prebuilts/kernel-build-tools/linux-x86
T=$B/prebuilts/build-tools/linux-x86/bin
mk() {
	"$T/make" -j"$(nproc)" -C "$B/kernel/samsung/gtelwifi" O="$O" ARCH=arm LLVM=1 LLVM_IAS=1 XZ=/usr/bin/xz \
		HOSTCFLAGS="-I$K/include" \
		HOSTLDFLAGS="-Wl,-rpath,$K/lib64 -L $K/lib64 -fuse-ld=lld --rtlib=compiler-rt" \
		LZ4="$K/bin/lz4" LEX="$T/flex" YACC="$T/bison" M4="$T/m4" "$@"
}
"$B/kernel/samsung/gtelwifi/scripts/config" --file "$O/.config" -d KERNEL_GZIP -e KERNEL_XZ
mk olddefconfig
diff <(grep -v '^#' "$B/out/target/product/gtelwifi/obj/KERNEL_OBJ/.config" | sort) \
	<(grep -v '^#' "$O/.config" | sort) || true
mk zImage
ls -l "$O/arch/arm/boot/zImage"
