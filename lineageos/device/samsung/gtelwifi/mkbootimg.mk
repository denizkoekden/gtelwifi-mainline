# BOARD_CUSTOM_BOOTIMG_MK for gtelwifi - DRAFT, never built.
#
# sboot wants a v0 header with the CAF "dt_size" field (offset 40) and the
# SPRD dt.img behind the second stage. AOSP/LineageOS mkbootimg.py only knows
# --dtb (header v2+), so boot and recovery are packed by tools/mkbootimg_sprd.py
# (host module mkbootimg_sprd, Android.bp). It builds the dt.img from the one
# DTB (all eight sprd,sc-id triples of patch 0100 -> one blob) and reproduces
# the two boot images sboot is known to boot byte for byte (tools/selftest.py).

MKBOOTIMG_SPRD := $(HOST_OUT_EXECUTABLES)/mkbootimg_sprd$(HOST_EXECUTABLE_SUFFIX)
# gtel3g (SM-T561) sets GTELWIFI_DTB_NAME := sc7730-samsung-gtel3g in its BoardConfig
GTELWIFI_DTB_NAME ?= sc7730-samsung-gtelwifi
GTELWIFI_DTB := $(TARGET_OUT_INTERMEDIATES)/KERNEL_OBJ/arch/arm/boot/dts/unisoc/$(GTELWIFI_DTB_NAME).dtb

$(INSTALLED_BOOTIMAGE_TARGET): $(MKBOOTIMG_SPRD) $(INTERNAL_BOOTIMAGE_FILES) $(INSTALLED_KERNEL_TARGET)
	$(call pretty,"Target boot image: $@")
	$(hide) $(MKBOOTIMG_SPRD) --kernel $(INSTALLED_KERNEL_TARGET) $(INTERNAL_BOOTIMAGE_ARGS) $(BOARD_MKBOOTIMG_ARGS) --sprd_dtb $(GTELWIFI_DTB) --dt_out $(PRODUCT_OUT)/dt.img --output $@
	$(hide) $(call assert-max-image-size,$@,$(BOARD_BOOTIMAGE_PARTITION_SIZE))
	@echo "Made boot image: $@"

ifneq ($(INSTALLED_RECOVERYIMAGE_TARGET),)
$(INSTALLED_RECOVERYIMAGE_TARGET): $(MKBOOTIMG_SPRD) $(recovery_ramdisk) $(recovery_kernel) $(INSTALLED_KERNEL_TARGET)
	@echo "----- Making recovery image ------"
	$(hide) $(MKBOOTIMG_SPRD) --kernel $(recovery_kernel) $(INTERNAL_RECOVERYIMAGE_ARGS) $(BOARD_MKBOOTIMG_ARGS) --sprd_dtb $(GTELWIFI_DTB) --output $@
	$(hide) $(call assert-max-image-size,$@,$(BOARD_RECOVERYIMAGE_PARTITION_SIZE))
	@echo "Made recovery image: $@"
endif
