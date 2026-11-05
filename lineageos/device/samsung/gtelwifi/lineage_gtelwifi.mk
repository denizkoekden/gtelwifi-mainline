#
# Samsung Galaxy Tab E 9.6 (SM-T560, gtelwifi) - DRAFT
#

# PRODUCT_SHIPPING_API_LEVEL 19 (device.mk) would pull the legacy HIDL CAS and
# OMX services from base_vendor.mk, which FCM 202604 no longer allows to be
# served. Must be set before base_vendor.mk is evaluated (via full_base.mk).
TARGET_REQUIRES_HIDL_CAS_HAL := false
TARGET_SUPPORTS_OMX_SERVICE := false

# Bring-up: vendor/lineage/config/common.mk makes userdebug non-debuggable
# (PRODUCT_NOT_DEBUGGABLE_IN_USERDEBUG) unless WITH_ADB_INSECURE is set.
# Non-debuggable init ignores androidboot.selinux=permissive (built with
# ALLOW_PERMISSIVE_SELINUX=0): the first boot on 04.11. ran enforcing and
# init could not even set its own service properties. This also leaves adb
# without on-screen authorization (ro.adb.secure=0). Drop it once the
# policy is done.
WITH_ADB_INSECURE := true

# AOSP base product first, as device/mainline/generic does (full_base.mk ->
# generic_no_telephony -> handheld_*); without it the base packages are missing
$(call inherit-product, $(SRC_TARGET_DIR)/product/full_base.mk)

# Wi-Fi only tablet (check: common_mini_tablet_wifionly.mk vs full)
$(call inherit-product, vendor/lineage/config/common_mini_tablet_wifionly.mk)

$(call inherit-product, device/samsung/gtelwifi/device.mk)

PRODUCT_NAME := lineage_gtelwifi
PRODUCT_DEVICE := gtelwifi
PRODUCT_BRAND := samsung
PRODUCT_MODEL := SM-T560
PRODUCT_MANUFACTURER := samsung
