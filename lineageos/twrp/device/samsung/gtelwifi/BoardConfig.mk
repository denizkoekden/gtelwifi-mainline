#
# TWRP for the Samsung Galaxy Tab E 9.6 (SM-T560, gtelwifi): Spreadtrum
# SC7730SE, 4x Cortex-A7, mainline Linux 7.3.1 (the kernel of the LineageOS
# 24.0 port, prebuilt/). Developed with AI assistance (Claude).
#

DEVICE_PATH := device/samsung/gtelwifi

# Architecture
TARGET_ARCH := arm
TARGET_ARCH_VARIANT := armv7-a-neon
TARGET_CPU_ABI := armeabi-v7a
TARGET_CPU_ABI2 := armeabi
TARGET_CPU_VARIANT := cortex-a7
# Mainline Linux only has the 64-bit binder ABI
TARGET_USES_64_BIT_BINDER := true

TARGET_BOARD_PLATFORM := sc8830
TARGET_BOOTLOADER_BOARD_NAME := gtelwifi
TARGET_NO_BOOTLOADER := true
TARGET_NO_RADIOIMAGE := true

# Kernel: the LineageOS kernel, its cmdline is built in (CONFIG_CMDLINE:
# androidboot.hardware=gtelwifi, boot_devices, super_partition=SYSTEM),
# because sboot builds its own and drops the header's.
TARGET_PREBUILT_KERNEL := $(DEVICE_PATH)/prebuilt/zImage
BOARD_KERNEL_IMAGE_NAME := zImage
BOARD_KERNEL_BASE := 0x00000000
BOARD_KERNEL_PAGESIZE := 2048
BOARD_KERNEL_CMDLINE := androidboot.hardware=gtelwifi
BOARD_MKBOOTIMG_ARGS := --kernel_offset 0x00008000 --ramdisk_offset 0x01000000 \
    --second_offset 0x00f00000 --tags_offset 0x00000100 --header_version 0
# sboot wants the SPRD dt.img after the second stage (mkbootimg.mk)
BOARD_CUSTOM_BOOTIMG_MK := $(DEVICE_PATH)/mkbootimg.mk

# Partitions (GPT/PIT): 16 MiB for KERNEL and RECOVERY
BOARD_FLASH_BLOCK_SIZE := 131072
BOARD_BOOTIMAGE_PARTITION_SIZE := 16777216
BOARD_RECOVERYIMAGE_PARTITION_SIZE := 16777216
TARGET_USERIMAGES_USE_EXT4 := true
TARGET_USERIMAGES_USE_F2FS := true
BOARD_USERDATAIMAGE_FILE_SYSTEM_TYPE := ext4

# Dynamic partitions of the LineageOS port: super on SYSTEM + CSC
BOARD_SUPER_PARTITION_METADATA_DEVICE := SYSTEM
BOARD_SUPER_PARTITION_BLOCK_DEVICES := SYSTEM CSC
BOARD_SUPER_PARTITION_SYSTEM_DEVICE_SIZE := 1572864000
BOARD_SUPER_PARTITION_CSC_DEVICE_SIZE := 209715200
BOARD_SUPER_PARTITION_SIZE := 1782579200
BOARD_SUPER_PARTITION_GROUPS := gtelwifi_dynamic_partitions
BOARD_GTELWIFI_DYNAMIC_PARTITIONS_SIZE := 1778384896
BOARD_GTELWIFI_DYNAMIC_PARTITIONS_PARTITION_LIST := system system_ext product vendor
# Separate logical partitions, so root/vendor etc. are mount points, not symlinks into
# /system (which clashes with the vendor files TWRP puts into the recovery root)
TARGET_COPY_OUT_SYSTEM_EXT := system_ext
TARGET_COPY_OUT_PRODUCT := product
TARGET_COPY_OUT_VENDOR := vendor
BOARD_SYSTEM_EXTIMAGE_FILE_SYSTEM_TYPE := ext4
BOARD_PRODUCTIMAGE_FILE_SYSTEM_TYPE := ext4
BOARD_VENDORIMAGE_FILE_SYSTEM_TYPE := ext4
BOARD_USES_METADATA_PARTITION := true

# Recovery
TARGET_RECOVERY_FSTAB := $(DEVICE_PATH)/recovery/root/system/etc/recovery.fstab
TARGET_RECOVERY_PIXEL_FORMAT := RGBX_8888
BOARD_HAS_NO_SELECT_BUTTON := true
BOARD_HAS_LARGE_FILESYSTEM := true
RECOVERY_SDCARD_ON_DATA := true
# 16 MiB for kernel + dt.img + ramdisk
# XZ ramdisk (the kernel has RD_XZ); gzip leaves the image ~5 MB over 16 MiB
BOARD_RAMDISK_USE_XZ := true

# TWRP
TW_THEME := portrait_hdpi
TW_BRIGHTNESS_PATH := "/sys/class/backlight/backlight/brightness"
TW_MAX_BRIGHTNESS := 255
TW_DEFAULT_BRIGHTNESS := 143
TW_USE_TOOLBOX := true
TW_EXTRA_LANGUAGES := false
TW_EXCLUDE_TWRPAPP := true
TW_EXCLUDE_PYTHON := true
# nano for the terminal (testers, 06.11.); a small lsblk script is in recovery/root
TW_EXCLUDE_NANO := false
TW_EXCLUDE_LPDUMP := true
TW_EXCLUDE_LPTOOLS := false
TW_INCLUDE_NTFS_3G := false
TW_INCLUDE_FB2PNG := false
TW_NO_SCREEN_BLANK := false
TW_INCLUDE_CRYPTO := false
TW_INPUT_BLACKLIST := "hbtp_vm"
TW_DEVICE_VERSION := gtelwifi-mainline-4
# /data is FBE-encrypted by LineageOS (adiantum); no decryption yet
TW_INCLUDE_FBE := false
# TWRP copies some props from the vendor partition (Process_ResetProps,
# external_storage.*); without libresetprop that ends in red "Unable to
# override" lines at every start.
TW_INCLUDE_LIBRESETPROP := true
# The recovery binary links libresetprop, but TWRP's relink list does not
# copy it into the recovery root (recovery would not start without it)
TW_RECOVERY_ADDITIONAL_RELINK_LIBRARY_FILES += $(TARGET_OUT_SHARED_LIBRARIES)/libresetprop.so
PLATFORM_VERSION := 16.1.0
PLATFORM_SECURITY_PATCH := 2099-12-31
VENDOR_SECURITY_PATCH := 2099-12-31
