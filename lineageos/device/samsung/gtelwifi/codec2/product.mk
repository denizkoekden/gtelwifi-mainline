# SPDX-License-Identifier: Apache-2.0
# Experimental until the MediaCodec / player acceptance tests pass. Software
# codecs remain available. Private libraries are supplied by extract-vsp.sh.
TARGET_GTELWIFI_VSP ?= false
ifeq ($(TARGET_GTELWIFI_VSP),true)
PRODUCT_VENDOR_PROPERTIES += media.c2.hal.selection=aidl
PRODUCT_PACKAGES += android.hardware.media.c2-sprd-service media_codecs_sprd.xml
PRODUCT_COPY_FILES += \
    $(DEVICE_PATH)/proprietary/vsp/libomx_avcdec_hw_sprd.so:$(TARGET_COPY_OUT_VENDOR)/lib/libomx_avcdec_hw_sprd.so \
    $(DEVICE_PATH)/proprietary/vsp/libomx_vpxdec_hw_sprd.so:$(TARGET_COPY_OUT_VENDOR)/lib/libomx_vpxdec_hw_sprd.so \
    $(DEVICE_PATH)/proprietary/vsp/libomx_m4vh263dec_hw_sprd.so:$(TARGET_COPY_OUT_VENDOR)/lib/libomx_m4vh263dec_hw_sprd.so
endif
