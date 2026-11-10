#
# Samsung Galaxy Tab E 9.6 (SM-T560, gtelwifi), mainline kernel - DRAFT.
#

DEVICE_PATH := device/samsung/gtelwifi

# Inherit from mainline/common
$(call inherit-product, device/mainline/common/mainline_common.mk)

# mkbootimg_sprd (Android.bp), used by mkbootimg.mk; use_memfd.rc lives in
# the kernel/mainline/configs namespace (as in device/mainline/generic)
PRODUCT_SOONG_NAMESPACES += \
    $(DEVICE_PATH) \
    kernel/mainline/configs
PRODUCT_HOST_PACKAGES += mkbootimg_sprd

# 1.5 GB RAM, 32-bit only: Android Go profile (check: "mini" vs "full"
# tablet_wifionly in vendor/lineage/config)
PRODUCT_IS_GO := true

# First boot: SwiftShader + drmfb composer, root console, SELinux
# permissive, suspend blocked (standby works under Nura, untested under
# Android). mainline/common switches all of that from this one flag.
TARGET_INITIAL_BRINGUP := true
# Parts of the bring-up set that are no longer wanted (build q, 05.11.):
# - suspend: suspend_blocker held a wake lock forever, the screen went dark
#   but the SoC never slept. Real suspend tested at runtime: 9 suspends,
#   resume by Power/Home, Wi-Fi and Bluetooth come back.
# - fbkeyboard: an on-screen keyboard on /dev/fb0 for the boot console;
#   there is no fbdev any more (kernel config) and no console to type into.
# - console as root: the console service is stopped anyway (init.gtelwifi.rc).
TARGET_SUPPORTS_SUSPEND := true
TARGET_ENABLE_FBKEYBOARD := false
TARGET_CONSOLE_AS_ROOT := false

# CPU: schedutil (init.gtelwifi.rc) lets the clock drop to 768 MHz when idle;
# the libperfmgr power HAL raises the floor to 1300 MHz on touch
# (INTERACTION) and for 3 s on app launch (configs/power/powerhint.json).
TARGET_POWER_HAL := perfmgr-lineage

# Graphics. The DISPC driver (sprd-dispc, KMS, /dev/dri/card1) and lima
# (card0 + renderD128, GLES 2.0 only) run Phosh under Nura. The first boot
# keeps the bring-up stack above: drmfb composer and minigbm-upstream, whose
# dumb backend takes any KMS driver it does not know (drv.c fallback).
# TARGET_GTELWIFI_LIMA := true moves to Mesa lima instead (checked in the
# lineage-24.0 sources, not built yet):
# - gralloc_gbm_mesa lists lima as a GPU with a separate display controller
#   and allocates on the first card with CRTCs, i.e. sprd-dispc (CMA, so the
#   DISPC can scan the buffers out)
# - Mesa 26.2 takes a KMS driver without its own entry through kmsro
#   (pipe_loader_drm.c fallback), so the driconf mapping of the Nura package
#   is not needed here
# - SurfaceFlinger's RGBA_8888 client target becomes ABGR8888, which the
#   DISPC plane does not offer; drm_hwcomposer scans it out as XBGR8888
#   (red/blue swap, offered) with avoid_using_alpha_bits_for_framebuffer
# Tried on 05.11. (build 20261105e): Mesa renders on lima (SurfaceFlinger
# "Mesa, Mali400, OpenGL ES 2.0"), but gralloc_gbm_mesa opens renderD128
# (lima buffers, not contiguous: drm_hwcomposer "Failed to import prime fd")
# and, pointed at the KMS node (vendor.gralloc.device), its first client
# becomes DRM master, so drm_hwcomposer cannot ("Failed to set drm master
# 13") and falls back to a null display. Hence the allocator and composer
# that work with sprd-dispc (minigbm dumb buffers from CMA, drmfb
# composer), with Mesa only for EGL/GLES.
# Default since build 20261105f (05.11.): boots, SurfaceFlinger on lima.
TARGET_GTELWIFI_LIMA ?= true
ifeq ($(TARGET_GTELWIFI_LIMA),true)
TARGET_GRAPHICS := mesa
TARGET_GRAPHICS_ALLOCATOR_HAL := minigbm-upstream
# Explicit rollback: TARGET_GTELWIFI_OVERLAY=false keeps the existing stack.
TARGET_GTELWIFI_OVERLAY ?= false
ifeq ($(TARGET_GTELWIFI_OVERLAY),true)
TARGET_GRAPHICS_COMPOSER_HAL := none
PRODUCT_PACKAGES += android.hardware.composer.hwc3-service.gtelwifi
PRODUCT_VENDOR_PROPERTIES += \
    vendor.hwc.backend_override=sprd-dispc \
    vendor.hwc.drm.device=/dev/dri/card% \
    vendor.gtelwifi.overlay=true
else
TARGET_GRAPHICS_COMPOSER_HAL := drmfb-composer
endif
PRODUCT_VENDOR_PROPERTIES += \
    ro.opengles.version=131072
endif

# Off-mode charger (libminui, DRM backend). Without ro.minui.pixel_format
# minui allocates RGB565 dumb buffers, which sprd-dispc (XRGB8888/XBGR8888
# only) rejects: "charger: gr_init failed", the bootloader's picture stays.
# RGBX_8888 = DRM_FORMAT_XBGR8888 (same as TARGET_RECOVERY_PIXEL_FORMAT in
# the TWRP tree).
PRODUCT_VENDOR_PROPERTIES += \
    ro.minui.pixel_format=RGBX_8888

# Audio: the mainline audio HAL (default of mainline/common) on our UCM
# profile (audio/ucm2, the Nura one with the ADC at maximum). Only our card,
# none of alsa-ucm-conf's card profiles (the APEX brings the base files).
TARGET_AUDIO_MAINLINE_UCM_PROFILES := none

# No vibration motor (camera: see "Camera" below)
TARGET_HAS_VIBRATOR := false

# Shipped with Android 4.4. Below 29 the build does not enforce the VINTF
# kernel requirements, and NetBpfLoad's "KernelUpRev" exemption
# (first_api_level <= 33) applies to the 6.2+ 32-bit userspace check.
# check: Android 17 may refuse a level this low somewhere in the build.
# Must not be overridden by TARGET_FOLLOWS_LATEST_SHIPPING_API_LEVEL
# (mainline/common optional/shipping-api-level_latest sets 37).
PRODUCT_SHIPPING_API_LEVEL := 19
# HALs from mainline/common are current AIDL versions, so the VINTF target
# level follows the latest one (independent of the shipping API level)
TARGET_FOLLOWS_LATEST_VINTF_TARGET_LEVEL := true

# Uncompressed APEXes: a compressed one is decompressed to /data and mounted
# through dm-verity, and apexd's wait for the dm link lost the race against
# the slow ueventd here (adbd and media.swcodec missing, boots of 04.11.).
# Pre-installed APEXes on /system mount over loop without dm-verity. Costs
# ~250 MiB in super, which has the room.
PRODUCT_COMPRESSED_APEX := false

# Bring-up: no RescueParty. After a few crashed boots it resets settings and
# finally reboots into recovery to offer a factory reset; the crashes here
# are bring-up bugs (flags_health_check already counted them on 04.11.).
PRODUCT_PRODUCT_PROPERTIES += \
    persist.sys.disable_rescue=true

# Shader cache priming stays off: on SwiftShader (LLVM JIT on a Cortex-A7)
# it and the boot animation kept SurfaceFlinger busy until the watchdog
# killed the first system_server of every fresh boot (04.11., builds u and
# 20261105b). Since lima renders (build f) the boot animation is back
# (build o), sized for the panel instead of LineageOS' 1080 default.
PRODUCT_VENDOR_PROPERTIES += \
    service.sf.prime_shader_cache=0
TARGET_SCREEN_WIDTH := 800
TARGET_SCREEN_HEIGHT := 1280

# Bring-up: "adb root" is switched on in init.gtelwifi.rc (/data/adbroot).
# The old LineageOS property persist.sys.root_access is not read any more
# (adbd asks adbroot_service, lineage-24.0).

# Sensors: only the IIO backend. The tablet has an accelerometer (lis2hh12)
# and nothing else; the default list (iio,input,mock) added mock gyroscope,
# magnetometer, light, proximity, pressure and temperature sensors. The
# backend list and the accelerometer orientation are in sensors.conf.
PRODUCT_COPY_FILES += \
    $(DEVICE_PATH)/configs/sensors/sensors.conf:$(TARGET_COPY_OUT_VENDOR)/etc/sensors/sensors.conf

# Hardware facts for mainline/common. TARGET_INITIAL_BRINGUP would pick the
# fake cuttlefish battery; smb347 + sc2723-fgu are real power_supply devices
# (/sys/class/power_supply/{smb347-usb,battery}).
TARGET_HAS_BATTERY := true
TARGET_HEALTH_HAL := default-aidl
TARGET_SUPPORTS_HARDWARE_BACKED_SECURITY := false

include device/mainline/common/optional/options.mk

# Logical partitions in super (BoardConfig.mk), sizes computed from the
# content (as device/mainline/generic); without it build_image.py fails with
# KeyError 'partition_size' on vendor.img
PRODUCT_USE_DYNAMIC_PARTITIONS := true
PRODUCT_USE_DYNAMIC_PARTITION_SIZE := true

# Emulated /sdcard (FUSE), as in device/mainline/generic
$(call inherit-product, $(SRC_TARGET_DIR)/product/emulated_storage.mk)

# Boot ramdisk = first stage. generic_ramdisk.mk has these, but it forbids
# other makefiles to install into the ramdisk, and with header v0 (no
# vendor_boot) our first stage fstab has to live there.
PRODUCT_PACKAGES += \
    init_first_stage \
    ramdisk-build.prop \
    snapuserd_ramdisk \
    toolbox_ramdisk

# No ashmem in mainline: libcutils on memfd (kernel/mainline/configs)
PRODUCT_PACKAGES += \
    use_memfd.rc

# Init, fstab
PRODUCT_COPY_FILES += \
    $(DEVICE_PATH)/rootdir/etc/fstab.gtelwifi:$(TARGET_COPY_OUT_RAMDISK)/fstab.gtelwifi \
    $(DEVICE_PATH)/rootdir/etc/fstab.gtelwifi:$(TARGET_COPY_OUT_VENDOR)/etc/fstab.gtelwifi \
    $(DEVICE_PATH)/rootdir/etc/fstab.gtelwifi.noencrypt:$(TARGET_COPY_OUT_VENDOR)/etc/fstab.gtelwifi.noencrypt \
    $(DEVICE_PATH)/rootdir/etc/init.gtelwifi.rc:$(TARGET_COPY_OUT_VENDOR)/etc/init/hw/init.gtelwifi.rc \
    $(DEVICE_PATH)/rootdir/bin/gtelwifi-bootlog.sh:$(TARGET_COPY_OUT_VENDOR)/bin/gtelwifi-bootlog.sh \
    $(DEVICE_PATH)/rootdir/bin/gtelwifi-crypt.sh:$(TARGET_COPY_OUT_VENDOR)/bin/gtelwifi-crypt.sh

# Wi-Fi/Bluetooth firmware: linux-firmware-mainline has nothing for the
# 43430, so firmware/brcm/ carries the files Nura used (firmware/README).
# NVRAM from the Samsung device repo (configs/wifi/nvram_net.txt, the same
# file the Nura firmware package installs)
PRODUCT_COPY_FILES += \
    $(DEVICE_PATH)/configs/wifi/nvram_net.txt:$(TARGET_COPY_OUT_VENDOR)/firmware/brcm/brcmfmac43430-sdio.samsung,gtelwifi.txt \
    $(DEVICE_PATH)/firmware/brcm/brcmfmac43430-sdio.bin:$(TARGET_COPY_OUT_VENDOR)/firmware/brcm/brcmfmac43430-sdio.bin \
    $(DEVICE_PATH)/firmware/brcm/brcmfmac43430-sdio.clm_blob:$(TARGET_COPY_OUT_VENDOR)/firmware/brcm/brcmfmac43430-sdio.clm_blob \
    $(DEVICE_PATH)/firmware/brcm/BCM43430A1.hcd:$(TARGET_COPY_OUT_VENDOR)/firmware/brcm/BCM43430A1.hcd \
    $(DEVICE_PATH)/rootdir/bin/gtelwifi-modprobe:$(TARGET_COPY_OUT_VENDOR)/bin/gtelwifi-modprobe \
    $(DEVICE_PATH)/rootdir/bin/gtelwifi-wlanmac.sh:$(TARGET_COPY_OUT_VENDOR)/bin/gtelwifi-wlanmac.sh

# Bluetooth factory address: btbcm marks the controller's default address
# invalid, so it stays unconfigured until a public address is set.
# hardware/mainline/common's bdaddr does that over mgmt from the file named
# here; efs is mounted read-only (fstab), bluetooth/bt_addr holds exactly
# "xx:xx:xx:xx:xx:xx" (17 bytes, checked under Nura). The address itself
# never goes into the image. Wi-Fi keeps the NVRAM default for now (Android
# randomizes the MAC per network anyway).
PRODUCT_PACKAGES += \
    bdaddr
PRODUCT_VENDOR_PROPERTIES += \
    ro.vendor.bt.bdaddr_path=/mnt/vendor/efs/bluetooth/bt_addr

# Bluetooth profiles: the stack enables only those named here. Without
# them GATT was the only one, so a speaker paired but never connected
# (05.11.). BCM43430A1 is Bluetooth 4.1: no LE Audio; no telephony, so no
# HFP AG, PBAP, MAP.
PRODUCT_VENDOR_PROPERTIES += \
    bluetooth.profile.a2dp.source.enabled=true \
    bluetooth.profile.avrcp.target.enabled=true \
    bluetooth.profile.bas.client.enabled=true \
    bluetooth.profile.gatt.enabled=true \
    bluetooth.profile.hid.host.enabled=true \
    bluetooth.profile.opp.enabled=true \
    bluetooth.profile.pan.panu.enabled=true

# Audio: UCM profile for the simple-audio-card "gtelwifi", laid out like
# alsa-ucm-conf below /vendor/etc/alsa/ucm2 (conf.d/<driver>/<card>.conf)
PRODUCT_COPY_FILES += \
    $(DEVICE_PATH)/audio/ucm2/gtelwifi.conf:$(TARGET_COPY_OUT_VENDOR)/etc/alsa/ucm2/Unisoc/SC7730/gtelwifi/gtelwifi.conf \
    $(DEVICE_PATH)/audio/ucm2/HiFi.conf:$(TARGET_COPY_OUT_VENDOR)/etc/alsa/ucm2/Unisoc/SC7730/gtelwifi/HiFi.conf \
    $(DEVICE_PATH)/audio/ucm2/gtelwifi.conf:$(TARGET_COPY_OUT_VENDOR)/etc/alsa/ucm2/conf.d/simple-card/gtelwifi.conf

# Default wallpaper at panel size: the LineageOS one is 1920x1920, so
# ImageWallpaper kept a 14 MiB buffer (CMA, every buffer here is a dumb
# buffer) and SurfaceFlinger scaled a 1090x1746 crop down to 800x1280 on
# the GPU in every composited frame. Centre crop of that file, read through
# ro.config.wallpaper before the drawable (WallpaperManager).
PRODUCT_COPY_FILES += \
    $(DEVICE_PATH)/media/default_wallpaper.png:$(TARGET_COPY_OUT_PRODUCT)/media/wallpaper/gtelwifi_default.png
PRODUCT_PRODUCT_PROPERTIES += \
    ro.config.wallpaper=/product/media/wallpaper/gtelwifi_default.png

# lineage-sdk overlay: Home as a wake key
DEVICE_PACKAGE_OVERLAYS += \
    $(DEVICE_PATH)/overlay-lineage

# framework-res values read by system_server before runtime resource
# overlays apply (PowerProfile caches power_profile.xml at boot, so the
# auto-generated RRO left battery.capacity at AOSP's placeholder 2): built
# into framework-res instead of an RRO (mainline_common.mk enforces RROs)
DEVICE_PACKAGE_OVERLAYS += \
    $(DEVICE_PATH)/overlay-static
PRODUCT_ENFORCE_RRO_EXCLUDED_OVERLAYS += \
    $(DEVICE_PATH)/overlay-static

# Camera: STMicroelectronics' AIDL camera provider (camera/libcamera-provider,
# imported unchanged) around libcamera's Android HAL with the simple pipeline
# handler (camera.libcamera, external/libcamera patch). Both sensors deliver
# YUV, location and rotation come from the DT. The sensor register tables
# stay on the device; the empty directories are their bind-mount targets.
# Not mainline/common's TARGET_CAMERA_PROVIDER_HAL=libcamera: its APEX does
# not exist in lineage-24.0.
PRODUCT_PACKAGES += \
    android.hardware.camera.provider-service.libcamera \
    camera.libcamera \
    Aperture
PRODUCT_VENDOR_PROPERTIES += \
    ro.hardware.camera=libcamera
# libcamera's BufferAdapter resolves camera3 buffers through the gralloc0
# perform() calls of minigbm (GRALLOC_DRM_GET_*); the default gralloc0
# module has none and the provider crashed on the first capture request
PRODUCT_PACKAGES += \
    gralloc.minigbm_upstream
PRODUCT_VENDOR_PROPERTIES += \
    ro.hardware.gralloc=minigbm_upstream
PRODUCT_COPY_FILES += \
    $(DEVICE_PATH)/camera/camera_hal.yaml:$(TARGET_COPY_OUT_VENDOR)/etc/camera/libcamera/camera_hal.yaml \
    frameworks/native/data/etc/android.hardware.camera.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.camera.xml \
    frameworks/native/data/etc/android.hardware.camera.front.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.camera.front.xml

# GPS: AIDL bridge to the stock legacy HAL plus the shim for the glgps
# blobs (gnss/README)
PRODUCT_PACKAGES += \
    android.hardware.gnss-service.gtelwifi \
    libgpsd_shim
PRODUCT_COPY_FILES += \
    frameworks/native/data/etc/android.hardware.location.gps.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.location.gps.xml

# Proprietary camera sensor tables and GPS files from the stock firmware:
# only on the build host (proprietary/, copied there from the local
# lineageos/proprietary by push-to-buildhost.sh), never in the public
# repository. Without them the image builds, but the cameras and GPS do
# not work. The legacy GPS HAL loads from /vendor/lib/gpsblobs; init copies
# gpsd to /data (init.gtelwifi.rc), which needs libssl/libcrypto/libstdc++
# from /system.
GTELWIFI_PROPRIETARY := $(DEVICE_PATH)/proprietary
PRODUCT_COPY_FILES += \
    $(foreach s,s5k4ecgx sr200pc20m, \
        $(foreach f,$(wildcard $(GTELWIFI_PROPRIETARY)/camera/$(s)/*.bin), \
            $(f):$(TARGET_COPY_OUT_VENDOR)/firmware/$(s)/$(notdir $(f)))) \
    $(foreach f,$(wildcard $(GTELWIFI_PROPRIETARY)/gps/*), \
        $(f):$(TARGET_COPY_OUT_VENDOR)/lib/gpsblobs/$(notdir $(f)))

# Key layout of the matrix keypad (Home wakes the device)
PRODUCT_COPY_FILES += \
    $(DEVICE_PATH)/configs/keylayout/sc8830-keypad.kl:$(TARGET_COPY_OUT_VENDOR)/usr/keylayout/sc8830-keypad.kl

# Screen
PRODUCT_AAPT_CONFIG := normal
PRODUCT_AAPT_PREF_CONFIG := mdpi
PRODUCT_CHARACTERISTICS := tablet

# Adoptable storage (SD card as internal storage, fstab encryptable=userdata).
# No metadata encryption here, so vold sets the card up with dm-crypt
# (cryptfs.cpp, default aes-128-cbc) plus fscrypt on top; Adiantum for both,
# as for /data (Cortex-A7 without AES instructions)
PRODUCT_VENDOR_PROPERTIES += \
    ro.crypto.fde_algorithm=adiantum \
    ro.crypto.volume.options=adiantum:adiantum:v2

# /data with or without encryption: default fstab.gtelwifi, the TWRP
# installer's choice switches to fstab.gtelwifi.noencrypt (gtelwifi-crypt.sh)
PRODUCT_VENDOR_PROPERTIES += \
    vendor.gtelwifi.fstab=gtelwifi

# Optional SC7730 VSP Codec2 service; software fallback remains available.
$(call inherit-product, $(DEVICE_PATH)/codec2/product.mk)

# Device settings (Settings > System): CPU overclocking switch. Not in the
# 20261110 pre-release (never run on the tablet yet): build with
# TARGET_GTELWIFI_PARTS=true to include it.
TARGET_GTELWIFI_PARTS ?= false
ifeq ($(TARGET_GTELWIFI_PARTS),true)
PRODUCT_PACKAGES += \
    GtelwifiParts
endif
