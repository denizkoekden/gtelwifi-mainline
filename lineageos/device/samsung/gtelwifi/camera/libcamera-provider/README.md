# hardware-camera #

This module contains the STMicroelectronics android.hardware.camera source code.
It is part of the STMicroelectronics delivery for Android.

## Description ##

This module implements android.hardware.camera.provider AIDL version 1 and
a camera device based on libcamera.

Please see the Android delivery release notes for more details.

## Documentation ##

* The [release notes][] provide information on the release.
[release notes]: https://wiki.st.com/stm32mpu/wiki/Android-based_OpenSTDroid_ecosystem_release_note_-_v6.2.1

## Dependencies ##

This module can't be used alone. It is part of the STMicroelectronics delivery for Android.
It also depends on libcamera.

```
PRODUCT_PACKAGES += \
    android.hardware.camera.provider-service.libcamera
```

## Contents ##

This directory contains the sources and associated Android makefile to generate the camera provider service binary.

## License ##

This module is distributed under the Apache License, Version 2.0 found in the [LICENSE](./LICENSE) file.
