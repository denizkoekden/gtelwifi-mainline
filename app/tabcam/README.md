# tabcam

A small camera for the Samsung Galaxy Tab E 9.6 (SM-T560, "gtelwifi") on
mainline Linux with Phosh.

GNOME Snapshot renders its preview on the CPU on this tablet: GTK 4 needs
GLES 3, the Mali-400 only has GLES 2. That keeps the four Cortex-A7 at about
70 % for 11 fps. tabcam pulls the NV12 frames from PipeWire (libcamera) with
a GStreamer appsink and draws them with SDL3's GLES 2 renderer, which
converts NV12 in a shader: the full sensor rate (rear 15-16 fps indoors,
front 24 fps) at about 2 % CPU.

## Controls

- main bar (bottom in portrait, right in landscape): last photo (tap opens
  it), shutter, camera switch
- tool bar (top in portrait, left in landscape): rotation lock (toggles
  Phosh's `org.gnome.settings-daemon.peripherals.touchscreen
  orientation-lock`), self-timer (off, 3 s, 10 s), thirds grid
- the volume keys also release the shutter, if Phosh passes them on

Photos go to `~/Pictures/tabcam-YYYYMMDD-HHMMSS.jpg`, at the preview size
(rear 640x480, front 640x480), upright by the accelerometer even with the
rotation locked, never mirrored. The camera, timer and grid settings are
kept in `~/.config/tabcam/state`.

## Orientation

- the preview follows the display rotation that Phosh applies, read from its
  `org.gnome.Mutter.DisplayConfig` (SDL's display orientation does not
  follow it on Phosh)
- photos and the icons follow the accelerometer (iio-sensor-proxy), so they
  stay upright with the rotation locked
- the DT `rotation` of the sensors counts counter-clockwise (as in
  libcamera): the picture is turned clockwise by 360 - rotation - display
  rotation
- the front camera preview is mirrored; SDL flips before it rotates, so at
  90 and 270 degrees that is a top-bottom flip of the texture

## Build

On the tablet (Alpine/Nura edge, armv7):

    apk add --virtual .tabcam-build build-base pkgconf sdl3-dev sdl3_image-dev \
        sdl3_ttf-dev gst-plugins-base-dev
    make && make install PREFIX=/usr/local
    apk del .tabcam-build

Runtime: sdl3, sdl3_image, sdl3_ttf, gstreamer, gst-plugins-base,
gst-plugins-good (jpegenc), pipewire's GStreamer plugin, the Adwaita icon
theme and font. The PipeWire node names and mounting rotations of the two
cameras are fixed in `cams[]` for the gtelwifi.

`TABCAM_DEBUG=1` prints camera starts, the first frame and the rotations.
