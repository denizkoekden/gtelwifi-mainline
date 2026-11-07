#
# Samsung Galaxy Tab E 9.6 3G (SM-T561, gtel3g) - DRAFT, not built yet.
# gtelwifi plus the SC7730 modem core: 3G data and SMS, no voice (the
# tablet is not voice capable, no audio path through the CP).
#
# Own variable, not DEVICE_PATH: gtelwifi's device.mk sets that one.
GTEL3G_PATH := device/samsung/gtel3g

$(call inherit-product, device/samsung/gtelwifi/device.mk)

PRODUCT_SOONG_NAMESPACES += \
    $(GTEL3G_PATH)

# Radio: AIDL HAL straight on the CP's AT channel (radio/, README.md), no
# rild/libril, no vendor RIL blobs
PRODUCT_PACKAGES += \
    android.hardware.radio-service.gtel3g

# Telephony features: telephony.gsm.xml declares android.hardware.telephony
# and .gsm; Android 14+ checks the sub-features before it lets apps use the
# telephony APIs. No .calling (no voice), no .ims.
# check: file names in frameworks/native/data/etc of lineage-24.0
PRODUCT_COPY_FILES += \
    frameworks/native/data/etc/android.hardware.telephony.gsm.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.telephony.gsm.xml \
    frameworks/native/data/etc/android.hardware.telephony.radio.access.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.telephony.radio.access.xml \
    frameworks/native/data/etc/android.hardware.telephony.subscription.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.telephony.subscription.xml \
    frameworks/native/data/etc/android.hardware.telephony.data.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.telephony.data.xml \
    frameworks/native/data/etc/android.hardware.telephony.messaging.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.telephony.messaging.xml

# config_voice_capable=false, SMS and mobile data on
DEVICE_PACKAGE_OVERLAYS += \
    $(GTEL3G_PATH)/overlay

# Modem core start, HAL start, SIPC device rights. ueventd: mainline/common
# imports /vendor/etc/ueventd.${ro.hardware}.rc, ro.hardware is gtelwifi
# (kernel CMDLINE) on the T561 too; gtelwifi itself ships no such file.
PRODUCT_COPY_FILES += \
    $(GTEL3G_PATH)/rootdir/etc/init.gtel3g.rc:$(TARGET_COPY_OUT_VENDOR)/etc/init/init.gtel3g.rc \
    $(GTEL3G_PATH)/rootdir/etc/ueventd.gtel3g.rc:$(TARGET_COPY_OUT_VENDOR)/etc/ueventd.gtelwifi.rc \
    $(GTEL3G_PATH)/rootdir/bin/gtel3g-modem.sh:$(TARGET_COPY_OUT_VENDOR)/bin/gtel3g-modem.sh

PRODUCT_VENDOR_PROPERTIES += \
    persist.vendor.gtel3g.modem=1 \
    persist.vendor.gtel3g.nvitemd=1 \
    persist.vendor.radio.at_device=/dev/stty_w0 \
    persist.vendor.radio.at_log=0 \
    ro.telephony.default_network=0

# nvitemd (optional blob, below) reads the LOS16 modem properties
# (out/gtel3g-ril/ANALYSE.md, section 2). ro.product.partitionpath stays
# unset on purpose: with it nvitemd writes runtime NV back to the tester's
# NV partitions.
PRODUCT_VENDOR_PROPERTIES += \
    persist.modem.w.enable=1 \
    persist.modem.w.nvp=w \
    ro.radio.modemtype=w \
    ro.modem.w.dev=/proc/cpw \
    ro.modem.w.tty=/dev/stty_w \
    ro.modem.w.eth=rmnet \
    ro.modem.w.id=0 \
    ro.modem.w.count=1 \
    ro.modem.w.loop=/dev/spipe_w0 \
    ro.modem.w.nv=/dev/spipe_w1 \
    ro.modem.w.assert=/dev/spipe_w2 \
    ro.modem.w.fixnv_size=0x40000 \
    ro.modem.w.runnv_size=0x60000

# Proprietary, optional: nvitemd from the LOS16 ROM with the libraries it
# needs that the vendor image lacks (libstdc++.so of that ROM). LOCAL ONLY:
# lineageos/proprietary/gtel3g/{bin,lib}/, which push-to-buildhost.sh copies
# to device/samsung/gtelwifi/proprietary/gtel3g/ with the rest of
# lineageos/proprietary. Never in git or the public export. Without the
# files the image builds and init only logs that nvitemd is missing.
GTEL3G_PROPRIETARY := device/samsung/gtelwifi/proprietary/gtel3g
PRODUCT_COPY_FILES += \
    $(foreach f,$(wildcard $(GTEL3G_PROPRIETARY)/bin/*), \
        $(f):$(TARGET_COPY_OUT_VENDOR)/bin/$(notdir $(f))) \
    $(foreach f,$(wildcard $(GTEL3G_PROPRIETARY)/lib/*), \
        $(f):$(TARGET_COPY_OUT_VENDOR)/lib/$(notdir $(f)))
