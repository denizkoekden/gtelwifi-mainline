# LineageOS 24.0 on mainline Linux for the Galaxy Tab E 9.6 (SM-T560)

> **Developed with AI assistance** (Claude, by Anthropic), like the rest of this repository. Check the policy of any
> project before you submit code from here to it.

This branch (`lineageos`) carries everything needed to build LineageOS 24.0 (Android 17, armv7) for the SM-T560
(`gtelwifi`) on mainline Linux 7.3.1:

| Path | Content |
|---|---|
| `patches/` | the kernel patch series against Linux v7.3.1 (0001-0117). 0001-0079 except 0014 carry a Signed-off-by, 0014 and 0080-0117 do not yet |
| `lineageos/device/samsung/gtelwifi/` | the device tree (`device/samsung/gtelwifi`) |
| `lineageos/device/samsung/gtel3g/` | the SM-T561 (3G) variant on top of it, with a radio HAL that talks AT commands to the modem (experimental, never tested with a SIM) |
| `lineageos/kernel/` | the Android kernel config fragment and the check scripts that merge it with the LineageOS/AOSP fragments |
| `lineageos/patches/<project>/` | patches for other projects of the LineageOS tree (`git am` in that project): libcamera, minigbm, system/core, frameworks/base and a few more |
| `lineageos/local_manifests/gtelwifi.xml` | the local manifest (device/mainline/common and its dependencies) |
| `lineageos/scripts/` | `mkrepos.sh` (device and kernel repositories for the manifest), `build.sh`, `mkflash.sh` (flash bundle and TWRP zips), `flash-system.sh` |
| `lineageos/installer/` | the TWRP installer: `update-binary.c` (static armhf, writes KERNEL and SYSTEM), `mkzip.py` (builds the install and update zips) and `mkmetadata.py` (empty `/metadata` image) |
| `lineageos/twrp/` | TWRP 3.7.1 (TWRP 12.1) for this tablet on the same mainline kernel: device tree, one patch for `bootable/recovery`, `build-kernel.sh`; see its README |

## Hardware support

Working: display, touch, keys (Home wakes), GPU (Mesa lima, GLES 2.0, GPU at 512 MHz), Wi-Fi, Bluetooth incl. A2DP,
speaker, headphones, internal microphone, rear camera up to 5 MP with autofocus and tap to focus, front camera at
640x480, video recording, accelerometer, charging, fuel gauge, suspend with the panel powered off, game controllers,
CPU scaling with touch boost (libperfmgr), USB (MTP, file transfer notification), microSD also as internal
(adoptable) storage, gesture or 3-button navigation, power-off.

Limitations: GLES 2.0 only (Mali-400), no continuous autofocus, photo preview at ~8 fps while a 5 MP still stream
runs, standby ~85 mA (no deep sleep of the CPU cluster), 32-bit only, SELinux permissive (userdebug).

## Proprietary files

Nothing proprietary from Samsung is in this repository. The build picks up the files below from the device tree's
`proprietary/` directory (`device/samsung/gtelwifi/proprietary/` in the LineageOS checkout, not in git) and puts them
into the vendor image; without them the ROM builds and boots, but the cameras and GPS do not work:

- **Camera sensor tables** (S5K4ECGX, SR200PC20M): `proprietary/camera/<sensor>/*.bin`, extracted from a TWRP
  backup of the stock system with `tools/camtables.py --firmware`. They end up in `/vendor/firmware/<sensor>/`.
- **GPS** (Broadcom glgps): `proprietary/gps/` holds the stock `gpsd`, `gps.default.so`, `libsprd_agps_agent.so`
  and `gps.xml`; see `lineageos/device/samsung/gtelwifi/gnss/README` for the two DT_NEEDED edits. They end up in
  `/vendor/lib/gpsblobs/`, and init copies them to `/data/vendor/gps/blobs/` at boot (gpsd needs the system linker
  namespace).

The Wi-Fi/Bluetooth firmware comes from linux-firmware (Cypress/Broadcom licences, redistributable); the Wi-Fi NVRAM
is the one of the public Samsung device trees for this tablet.

## Building

On a LineageOS 24.0 checkout (`repo init -u https://github.com/LineageOS/android.git -b lineage-24.0`):

1. Create the device and kernel repositories with `lineageos/scripts/mkrepos.sh <dir> 7.3.1 <sha512 of linux-7.3.1.tar.xz>`
   (it applies `patches/` to the kernel.org tarball) and point the `gtelwifi` remote in
   `lineageos/local_manifests/gtelwifi.xml` at `<dir>`. Copy the manifest to `.repo/local_manifests/` and `repo sync`.
2. Apply `lineageos/patches/<project>/*.patch` with `git am` in the matching projects (`external_libcamera` →
   `external/libcamera`, and so on).
3. `source build/envsetup.sh && breakfast gtelwifi && m`.

The SoC's bootloader (sboot) loads an Android boot image v0 with an SPRD `dt.img` from the KERNEL partition; the
device tree builds it with `tools/mkbootimg_sprd.py`. The system is a dynamic-partition "super" image written to the
SYSTEM partition (`androidboot.super_partition=SYSTEM`), `/metadata` lives on HIDDEN. Bootloader, PARAM, PIT, EFS and
RECOVERY are never written, so Download mode always stays available.

## Installing

Each build gives two TWRP zips (`lineageos/installer/mkzip.py`): `-install.zip` also clears `/metadata` and `/data`
(clean install), `-update.zip` keeps them. The zip carries a static `update-binary` (`update-binary.c`), so it does
not depend on the shell of the recovery; it checks the GPT names and sizes of KERNEL and SYSTEM, writes `boot.img`
with a read-back check and the sparse `system.img` to SYSTEM. Apart from /metadata and /data (install zip only) nothing else is written.
The install zip asks whether /data should be encrypted: Volume Up = encrypted (default, also after 30 s),
Volume Down = not encrypted (TWRP can then read /data).
