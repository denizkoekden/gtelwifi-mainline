# Built on the tablet (Alpine/Nura edge, armv7): needs sdl3-dev, gst-plugins-base-dev, pkgconf
PKGS = sdl3 sdl3-image sdl3-ttf gio-2.0 gstreamer-1.0 gstreamer-app-1.0 gstreamer-video-1.0
CFLAGS ?= -O2 -Wall
PREFIX ?= /usr/local

tabcam: tabcam.c
	$(CC) $(CFLAGS) $(shell pkg-config --cflags $(PKGS)) -o $@ $< $(shell pkg-config --libs $(PKGS)) -lm

install: tabcam
	install -Dm755 tabcam $(DESTDIR)$(PREFIX)/bin/tabcam
	install -Dm644 tabcam.desktop $(DESTDIR)$(PREFIX)/share/applications/tabcam.desktop

clean:
	rm -f tabcam
