#
# Samsung Galaxy Tab E 9.6 3G (SM-T561, gtel3g) - DRAFT, not built yet.
# As lineage_gtelwifi.mk, but with telephony.
#

# Same reasons as in lineage_gtelwifi.mk (HIDL CAS/OMX, adb in bring-up)
TARGET_REQUIRES_HIDL_CAS_HAL := false
TARGET_SUPPORTS_OMX_SERVICE := false
WITH_ADB_INSECURE := true

# AOSP base with telephony (full_base.mk + telephony.mk: Telephony,
# TeleService, CarrierConfig, ...) instead of full_base.mk
$(call inherit-product, $(SRC_TARGET_DIR)/product/full_base_telephony.mk)

# LineageOS "mini" with telephony (vendor/lineage/config/telephony.mk:
# apns-conf.xml from vendor/lineage, Messaging) instead of
# common_mini_tablet_wifionly.mk.
# check: name in lineage-24.0 (common_mini_phone.mk vs. a tablet variant)
$(call inherit-product, vendor/lineage/config/common_mini_phone.mk)

$(call inherit-product, device/samsung/gtel3g/device.mk)

PRODUCT_NAME := lineage_gtel3g
PRODUCT_DEVICE := gtel3g
PRODUCT_BRAND := samsung
PRODUCT_MODEL := SM-T561
PRODUCT_MANUFACTURER := samsung
