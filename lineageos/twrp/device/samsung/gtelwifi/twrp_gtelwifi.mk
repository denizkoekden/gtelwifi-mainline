#
# TWRP for the Samsung Galaxy Tab E 9.6 (SM-T560, gtelwifi) on the mainline
# kernel of the LineageOS port. Developed with AI assistance (Claude).
#

$(call inherit-product, $(SRC_TARGET_DIR)/product/base.mk)
$(call inherit-product, vendor/twrp/config/common.mk)

PRODUCT_DEVICE := gtelwifi
PRODUCT_NAME := twrp_gtelwifi
PRODUCT_BRAND := samsung
PRODUCT_MODEL := SM-T560
PRODUCT_MANUFACTURER := samsung

PRODUCT_HOST_PACKAGES += mkbootimg_sprd
