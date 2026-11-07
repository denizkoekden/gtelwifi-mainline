#
# Samsung Galaxy Tab E 9.6 3G (SM-T561, gtel3g) - DRAFT, built blind
# Same board as gtelwifi plus the SC7730 modem core (SIPC, rmnet0-2).
#
include device/samsung/gtelwifi/BoardConfig.mk

GTEL3G_PATH := device/samsung/gtel3g

# Kernel: gtelwifi defconfig + modem fragment, DTB with board revisions 0-11
TARGET_KERNEL_CONFIG_EXT += $(GTEL3G_PATH)/configs/kernel/modem.config
GTELWIFI_DTB_NAME := sc7730-samsung-gtel3g

# Radio
TARGET_NO_RADIOIMAGE := true

# SELinux labels for the radio HAL (permissive build, labels only)
BOARD_VENDOR_SEPOLICY_DIRS += $(GTEL3G_PATH)/sepolicy/vendor
