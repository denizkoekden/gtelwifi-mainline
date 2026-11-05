# TWRP 3.7.1 for the SM-T560 (gtelwifi) on mainline Linux

Recovery image built from TWRP 12.1 with the LineageOS kernel (Linux 7.3.1 + our patches). Unlike the old
TWRP 3.5.2 (`omni_gtel3g`, Samsung 3.10 kernel) it has a working `adb shell`, MTP and the same partition view as
LineageOS (dynamic partitions on SYSTEM, `/metadata` on HIDDEN).

## Building

On a TWRP 12.1 tree
(`repo init -u https://github.com/minimal-manifest-twrp/platform_manifest_twrp_aosp.git -b twrp-12.1`, `repo sync`):

1. Copy `device/samsung/gtelwifi` to `device/samsung/gtelwifi` in the tree.
2. Apply `patches/bootable_recovery/*.patch` with `git am` in `bootable/recovery`.
3. Kernel: `build-kernel.sh <lineage tree> <out dir>` builds the LineageOS kernel with the `.config` of a LineageOS
   build, XZ-compressed; copy `arch/arm/boot/zImage` to `prebuilt/zImage` and the device tree blob
   (`sc7730-samsung-gtelwifi.dtb` of the same build) to `prebuilt/`.
4. `export ALLOW_MISSING_DEPENDENCIES=true; source build/envsetup.sh; lunch twrp_gtelwifi-eng; mka recoveryimage`.

The result has to fit the 16 MiB RECOVERY/KERNEL partition (15.5 MB): XZ kernel (5.7 MB instead of 7.4 MB with
gzip) and XZ ramdisk (`BOARD_RAMDISK_USE_XZ`). `mkbootimg.mk` packs it like the LineageOS boot image (boot image v0
with the SPRD `dt.img`, `tools/mkbootimg_sprd.py`).

TWRP copies the libraries into the recovery root only when the root is created: after changing a library
(e.g. the patch) remove `out/target/product/gtelwifi/{root,recovery,obj/PACKAGING/recovery_intermediates}` before
building again, or the old library stays in the image.

## What it needed

| Problem | Fix |
|---|---|
| `libbinder.so`: version script symbols `…PKyj` not defined | `TARGET_USES_64_BIT_BINDER := true` (mainline has only the 64-bit binder ABI) |
| `rsync`: `could not make way for new symlink: root/vendor` | `TARGET_COPY_OUT_{VENDOR,PRODUCT,SYSTEM_EXT}` as own partitions (as in LineageOS) |
| image 21.7 MB > 16 MiB | XZ kernel + XZ ramdisk |
| no picture, recovery crash loop (SIGSEGV after `cannot open fb0`) | TWRP's atomic DRM backend assumes two layer mixers (Qualcomm SDE); sprd-dispc has one plane → `patches/bootable_recovery/0001` |
| SIGABRT while loading the theme (`ashmem_create_region failed`) | mainline has no ashmem; libcutils takes memfd only with `ro.vndk.version` ≥ 29 and `sys.use_memfd=true` (`init.recovery.gtelwifi.rc`) |
| USB gone once the UI is up | TWRP switches to `mtp,adb`, the recovery init.rc has no configfs rule for it → FunctionFS MTP + rule in `init.recovery.gtelwifi.rc` |

## Tested (05.11.2026, booted from KERNEL and from RECOVERY)

Display (DRM, 800×1280), touch, `adb shell`, MTP (`18d1:4ee2`), dynamic partitions mapped, `/data` mounted.
Not possible: decrypting `/data` of LineageOS (FBE, no keymaster in TWRP) — MTP shows the encrypted names; use a
microSD card or ADB sideload for zips. The LineageOS install/update zips (`../installer/`) do not need `/data`.

Test it by writing it to KERNEL first (it boots like a boot image), then put the LineageOS `boot.img` back. Before
writing it to RECOVERY, back up the old recovery (`dd if=/dev/block/by-name/RECOVERY` from Android as root).
