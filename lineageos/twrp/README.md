# TWRP 3.7.1 for the SM-T560 (gtelwifi) on mainline Linux

Recovery image built from TWRP 12.1 with the LineageOS kernel (Linux 7.3.1 + our patches). Unlike the old
TWRP 3.5.2 (`omni_gtel3g`, Samsung 3.10 kernel) it has a working `adb shell`, MTP and the same partition view as
LineageOS (dynamic partitions on SYSTEM, `/metadata` on HIDDEN).

## Building

On a TWRP 12.1 tree (`repo init --depth=1 -u
https://github.com/minimal-manifest-twrp/platform_manifest_twrp_aosp.git -b twrp-12.1`, `repo sync -c --no-tags`;
33 GB, on the build host in `~/twrp` since 06.11.2026):

1. Kernel: `build-kernel.sh <lineage tree> <out dir>` builds the LineageOS kernel with the `.config` of a LineageOS
   build, XZ-compressed; copy `arch/arm/boot/zImage` to `prebuilt/zImage` and the device tree blob
   (`sc7730-samsung-gtelwifi.dtb` of the same build) to `prebuilt/`.
2. `build-twrp.sh <tree> <copy of device/samsung/gtelwifi> <copy of patches/>` installs the device tree, applies the
   patches that are not applied yet (`git am` in `bootable/recovery`), clears the cached recovery root and runs
   `lunch twrp_gtelwifi-eng; mka recoveryimage` (4 min from scratch, 35 s after that). The image:
   `out/target/product/gtelwifi/recovery.img`.
3. The gtel3g (SM-T561) test image is the same image with the dt.img for all board revisions:
   `tools/sprd-repack-dtb.py <image> out/gtel3g/allrev.dtb <out>` (main repository).

The result has to fit the 16 MiB RECOVERY/KERNEL partition (15.5 MB): XZ kernel (5.7 MB instead of 7.4 MB with
gzip) and XZ ramdisk (`BOARD_RAMDISK_USE_XZ`). `mkbootimg.mk` packs it like the LineageOS boot image (boot image v0
with the SPRD `dt.img`, `tools/mkbootimg_sprd.py`).

Config-only changes (the files under `recovery/root/`) do not need a TWRP tree: `tools/twrp-repack.py <image>
<out>` (in the main repository) puts them into an existing image and keeps kernel, cmdline and every other
ramdisk file. With an unchanged overlay its output is byte-identical to the input (checked for both released
images). The gtel3g test image: `--dtb out/gtel3g/allrev.dtb`.

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
| microSD not mountable (users on XDA, 06.11.: "can't mount my sdcard, it's FAT32") | `twrp.flags` had `mmcblk1` from the vendor kernel (the official TWRP 3.7.0 for gtel3g has it too). Mainline numbers the MMC hosts by the DT aliases: `mmc1` is the WLAN SDIO, the card is `mmcblk2` |
| "USB OTG" offered but dead | the dwc2 runs as peripheral only (`dr_mode`), entry removed |
| "Super" listed twice in Backup | TWRP adds `/super` on its own (`Setup_Super_Partition`, whenever `by-name/SYSTEM` exists); our `twrp.flags` entry removed |
| red `Unable to override 'external_storage.{casefold,sdcardfs}.enabled' due to missing libresetprop` at every start | `TW_INCLUDE_LIBRESETPROP := true`; TWRP's relink list does not copy the library into the recovery root, so also `TW_RECOVERY_ADDITIONAL_RELINK_LIBRARY_FILES` (without it `recovery` would not start: it links `libresetprop.so`) |
| red `failed to mount loop: …apex: Value too large for defined data type` → `Unable to load apex images from /system_root/system/apex` (users: "/system_root was failing") | TWRP bug, `patches/bootable_recovery/0002`: `twrpApex::loadApexImage()` took the size with `lseek()` on the already closed file and passed the -1 as `lo_sizelimit`, Linux 7.3 rejects that with `EOVERFLOW` (`loop_set_status_from_info()`) |
| Reboot → Recovery only restarted TWRP (no Samsung logo, an image just flashed to RECOVERY did not come up; testers, 06.11.) | Android 12 init first writes "boot-recovery" into the BCB on `/misc` and returns without rebooting when that fails; this device has no misc. `patches/system_core/0001` (same change as LineageOS `system_core` 0001): reboot anyway when there is no misc partition, sboot takes recovery from INFORM3 |
| first ROM install over stock or LineageOS 16 aborts: "Unable to find partition for path '/system_root'", "Failed unmounting System" (testers) | "Unmount System before installing a ZIP" (default on) failed when there is no system partition, which is the case before the first install (no super metadata yet). `patches/bootable_recovery/0004`: skip the unmount then. Workaround for older builds: untick that option in Settings |
| red `Unable to find partition for path '/system_root'` before the first install (tester photo, mainline-3) | `patches/bootable_recovery/0005`: three places touched the missing system root with a visible error: `UnMount_Main_Partitions()` at start, `Partition_Property_Get()` (the TWRP app check reads `ro.build.version.sdk`) and `mountsystemtoggle` ("Keep System Read Only?"); now they skip a partition that does not exist |
| Home does not wake the blanked screen (testers) | TWRP woke it only with Power; `patches/bootable_recovery/0003`: Home (`KEY_HOME`/`KEY_HOMEPAGE`) too |
| wish for CLI tools (testers) | `bash`, toybox and `sgdisk` were there; `nano` switched on (`TW_EXCLUDE_NANO := false`), small `lsblk` script in `recovery/root/system/bin`. No busybox/vim in the TWRP tree |

## Tested

05.11.2026, booted from KERNEL and from RECOVERY: display (DRM, 800×1280), touch, `adb shell`, MTP (`18d1:4ee2`),
dynamic partitions mapped, `/data` mounted. The microSD was not tested then (it could not work, see above).

06.11.2026, image with the `twrp.flags` fixes booted from KERNEL (`dumps/twrp-20261106/`): microSD (64 GB exFAT,
mounted through `exfat-fuse`) mount, write, delete; FAT32 through the kernel's vfat as TWRP mounts it (64 MB image
on a loop device, `codepage=437,iocharset=iso8859-1`; non-ASCII file names come out garbled, ASCII zip names are
fine); a Boot backup to the microSD; `/system_root` mounts (erofs) with LineageOS installed; one "Super" entry.

06.11.2026, rebuilt `3.7.1_12-gtelwifi-mainline-2` (patches 0001 + 0002, libresetprop) from KERNEL: no `E:` line
in `recovery.log` at start and after all tests, the props set from `/vendor/build.prop`, `twrp.apex.loaded=true`
(tzdata, media.swcodec, cts.shim under `/apex`), microSD mount/write/delete, `/system_root` (erofs), Boot backup to
the microSD.

06.11.2026, `3.7.1_12-gtelwifi-mainline-3` (+ system_core 0001, bootable_recovery 0003/0004, nano, lsblk; 16.2 MB)
from KERNEL: no `E:` line; `lsblk` and nano 6.0 work; screen blanked after 60 s (`brightness` 0) and came back with
an injected Home press (`sendevent` on `sc8830-keypad`); `twrp reboot recovery` gave a real reboot (new `boot_id`,
the older TWRP in RECOVERY came up). The zip install without a system partition (patch 0004) was confirmed by a
tester (install over the old system runs through, photo 06.11.).

06.11.2026, `3.7.1_12-gtelwifi-mainline-4` (+ 0005) from KERNEL, normal and with a test `recovery.fstab` without
the logical partitions (simulates the old layout, built with `tools/twrp-repack.py --overlay`): 0 visible
`Unable to find partition` lines (mainline-3 in the same simulation: 3), 0 `E:`. The installer's dry run
(`-DINSTALLER_DRY_RUN`) in that TWRP: zip in `/tmp` → `/sdcard` and `/data` unmounted; zip on `/data` → `/sdcard`
unmounted, `/data` remounted read-only.

Known:
- Before the first installation (stock or LineageOS 16 still on the tablet) there is no super metadata on SYSTEM:
  TWRP drops System, System_EXT, Product and Vendor and cannot mount `/system_root`. Expected; the install zip writes
  the raw images (since mainline-3 without the unmount abort, see above). "Super" in Backup saves the whole SYSTEM
  partition, also the old system.
- TWRP shows the time in its default zone (US Central) until a zone is set in its settings.

Not possible: decrypting `/data` of LineageOS (FBE, no keymaster in TWRP) — MTP shows the encrypted names; use a
microSD card or ADB sideload for zips. The LineageOS install/update zips (`../installer/`) do not need `/data`.

Test it by writing it to KERNEL first (it boots like a boot image), then put the LineageOS `boot.img` back. Before
writing it to RECOVERY, back up the old recovery (`dd if=/dev/block/by-name/RECOVERY` from Android as root).
