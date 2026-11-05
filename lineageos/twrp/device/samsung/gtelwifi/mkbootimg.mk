# Recovery image for sboot: header v0 with the CAF dt_size field and the
# SPRD dt.img (tools/mkbootimg_sprd.py, as in the LineageOS device tree).

MKBOOTIMG_SPRD := $(HOST_OUT_EXECUTABLES)/mkbootimg_sprd$(HOST_EXECUTABLE_SUFFIX)
GTELWIFI_DTB := device/samsung/gtelwifi/prebuilt/sc7730-samsung-gtelwifi.dtb

$(INSTALLED_BOOTIMAGE_TARGET): $(MKBOOTIMG_SPRD) $(INTERNAL_BOOTIMAGE_FILES) $(INSTALLED_KERNEL_TARGET)
	$(call pretty,"Target boot image: $@")
	$(hide) $(MKBOOTIMG_SPRD) --kernel $(INSTALLED_KERNEL_TARGET) $(INTERNAL_BOOTIMAGE_ARGS) $(BOARD_MKBOOTIMG_ARGS) --sprd_dtb $(GTELWIFI_DTB) --output $@

$(INSTALLED_RECOVERYIMAGE_TARGET): $(MKBOOTIMG_SPRD) $(recovery_ramdisk) $(recovery_kernel)
	@echo "----- Making recovery image ------"
	$(hide) $(MKBOOTIMG_SPRD) --kernel $(recovery_kernel) $(INTERNAL_RECOVERYIMAGE_ARGS) $(BOARD_MKBOOTIMG_ARGS) --sprd_dtb $(GTELWIFI_DTB) --output $@
	$(hide) $(call assert-max-image-size,$@,$(BOARD_RECOVERYIMAGE_PARTITION_SIZE))
	@echo "Made recovery image: $@"
