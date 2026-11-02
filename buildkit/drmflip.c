// SPDX-License-Identifier: GPL-2.0
/*
 * drmflip: page-flip test for the DISPC takeover driver. Two dumb buffers
 * with a moving bar, flipped with DRM_MODE_PAGE_FLIP_EVENT for N seconds;
 * prints the flip rate and the time between events. Raw ioctls, no libdrm.
 * With access to /dev/mem it also reads the DISPC scan-out position
 * (DPI_STS0: line << 16 | pixel) when each flip event arrives, to show
 * where in the frame the new buffer was latched.
 *
 *   drmflip [/dev/dri/cardN] [seconds] [h|v]
 *     h: horizontal bar moving down (default)
 *     v: vertical bar moving right, 24 px per frame; a tear shows as a
 *        horizontal break in the bar
 */

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include <drm/drm.h>
#include <drm/drm_mode.h>

struct buf {
	uint32_t handle, pitch, fb;
	uint64_t size;
	uint32_t *map;
};

static int fd;
static int vertical;
static volatile uint32_t *dispc;

static void die(const char *what)
{
	fprintf(stderr, "drmflip: %s: %s\n", what, strerror(errno));
	exit(1);
}

static double now(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

static void make_buf(struct buf *b, uint32_t w, uint32_t h)
{
	struct drm_mode_create_dumb c = { .width = w, .height = h, .bpp = 32 };
	struct drm_mode_fb_cmd2 f = { .width = w, .height = h,
				      .pixel_format = 0x34325258 /* XR24 */ };
	struct drm_mode_map_dumb m = { 0 };

	if (ioctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &c))
		die("create dumb");
	b->handle = c.handle;
	b->pitch = c.pitch;
	b->size = c.size;
	f.handles[0] = c.handle;
	f.pitches[0] = c.pitch;
	if (ioctl(fd, DRM_IOCTL_MODE_ADDFB2, &f))
		die("addfb2");
	b->fb = f.fb_id;
	m.handle = c.handle;
	if (ioctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &m))
		die("map dumb");
	b->map = mmap(NULL, c.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd,
		      m.offset);
	if (b->map == MAP_FAILED)
		die("mmap");
}

static void draw(struct buf *b, uint32_t w, uint32_t h, uint32_t frame)
{
	uint32_t bar = (frame * 8) % h, x, y;

	if (vertical) {
		uint32_t vb = (frame * 24) % w;

		for (y = 0; y < h; y++) {
			uint32_t *row = b->map + y * (b->pitch / 4);

			for (x = 0; x < w; x++)
				row[x] = (x >= vb && x < vb + 24) ? 0xffffff :
					 0x202020;
		}
		return;
	}
	{
	static const uint32_t col[] = { 0xff0000, 0x00ff00, 0x0000ff, 0xffffff };

	for (y = 0; y < h; y++) {
		uint32_t *row = b->map + y * (b->pitch / 4);
		uint32_t c = (y >= bar && y < bar + 40) ? 0xffffff :
			     col[(x = y * 4 / h)] & 0x3f3f3f;

		for (x = 0; x < w; x++)
			row[x] = c;
	}
	}
}

int main(int argc, char **argv)
{
	const char *dev = argc > 1 ? argv[1] : "/dev/dri/card0";
	double secs = argc > 2 ? atof(argv[2]) : 5, t0, tlast, gap, maxgap = 0;
	struct drm_mode_card_res res = { 0 };
	uint32_t conn_id, crtc_id, enc_id, w, h, frames = 0, i;
	struct drm_mode_get_connector conn = { 0 };
	struct drm_mode_modeinfo mode;
	struct drm_mode_get_encoder enc = { 0 };
	struct drm_mode_crtc set = { 0 };
	struct buf b[2];
	uint32_t ids[4], crtcs[4], encs[4];
	char ev[1024];
	uint32_t lmin = ~0u, lmax = 0, hist[12] = { 0 }, l, memfd;
	uint32_t seq, lastseq = 0, seqjumps = 0;

	vertical = argc > 3 && argv[3][0] == 'v';
	memfd = open("/dev/mem", O_RDONLY | O_SYNC);
	if ((int)memfd >= 0) {
		void *p = mmap(NULL, 4096, PROT_READ, MAP_SHARED, memfd,
			       0x20800000);

		if (p != MAP_FAILED)
			dispc = p;
	}

	fd = open(dev, O_RDWR | O_CLOEXEC);
	if (fd < 0)
		die(dev);
	if (ioctl(fd, DRM_IOCTL_SET_MASTER, 0))
		die("set master");

	res.connector_id_ptr = (uintptr_t)ids;
	res.count_connectors = 4;
	res.crtc_id_ptr = (uintptr_t)crtcs;
	res.count_crtcs = 4;
	res.encoder_id_ptr = (uintptr_t)encs;
	res.count_encoders = 4;
	if (ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res))
		die("getresources");
	conn_id = ids[0];
	crtc_id = crtcs[0];

	conn.connector_id = conn_id;
	conn.modes_ptr = (uintptr_t)&mode;
	conn.count_modes = 1;
	if (ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &conn) || !conn.count_modes)
		die("getconnector");
	enc_id = conn.encoder_id ? conn.encoder_id : encs[0];
	enc.encoder_id = enc_id;
	ioctl(fd, DRM_IOCTL_MODE_GETENCODER, &enc);
	w = mode.hdisplay;
	h = mode.vdisplay;
	printf("%s: connector %u, crtc %u, mode %s %ux%u@%u, %u x %u mm\n", dev,
	       conn_id, crtc_id, mode.name, w, h, mode.vrefresh,
	       conn.mm_width, conn.mm_height);

	make_buf(&b[0], w, h);
	make_buf(&b[1], w, h);
	draw(&b[0], w, h, 0);

	set.crtc_id = crtc_id;
	set.fb_id = b[0].fb;
	set.set_connectors_ptr = (uintptr_t)&conn_id;
	set.count_connectors = 1;
	set.mode = mode;
	set.mode_valid = 1;
	if (ioctl(fd, DRM_IOCTL_MODE_SETCRTC, &set))
		die("setcrtc");

	t0 = tlast = now();
	for (i = 1; now() - t0 < secs; i++) {
		struct drm_mode_crtc_page_flip flip = {
			.crtc_id = crtc_id, .fb_id = b[i & 1].fb,
			.flags = DRM_MODE_PAGE_FLIP_EVENT,
		};

		draw(&b[i & 1], w, h, i);
		if (ioctl(fd, DRM_IOCTL_MODE_PAGE_FLIP, &flip))
			die("page flip");
		if (read(fd, ev, sizeof(ev)) <= 0)	/* blocks until the event */
			die("read event");
		/* vblank sequence: +1 per flip when every frame flips */
		seq = ((struct drm_event_vblank *)ev)->sequence;
		if (frames && seq - lastseq != 1)
			seqjumps++;
		lastseq = seq;
		if (dispc) {
			l = dispc[0x8c / 4] >> 16;
			if (l < lmin)
				lmin = l;
			if (l > lmax)
				lmax = l;
			hist[l * 12 / 1322 < 12 ? l * 12 / 1322 : 11]++;
		}
		gap = now() - tlast;
		tlast += gap;
		if (gap > maxgap)
			maxgap = gap;
		frames++;
	}
	printf("%u flips in %.2f s = %.1f Hz, longest gap %.1f ms\n", frames,
	       now() - t0, frames / (now() - t0), maxgap * 1000);
	printf("vblank sequence: last %u, %u steps != 1\n", lastseq, seqjumps);
	if (dispc) {
		printf("scan-out line at flip event: min %u max %u (of 1322)\n",
		       lmin, lmax);
		for (i = 0; i < 12; i++)
			printf("  lines %4u-%4u: %u\n", i * 1322 / 12,
			       (i + 1) * 1322 / 12 - 1, hist[i]);
	}
	return 0;
}
