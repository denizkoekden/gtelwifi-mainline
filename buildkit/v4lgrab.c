// SPDX-License-Identifier: GPL-2.0
/*
 * v4lgrab: minimal V4L2 capture for the rescue system (no v4l-utils).
 * Sets NV16 at W x H, streams N frames through 4 mmap buffers, prints
 * sequence, size and frame interval, writes the last frame. With
 * V4L_SENSOR=<name prefix> the media link from that sensor to the video
 * node is enabled (the others disabled), with V4L_SUBDEV=/dev/v4l-subdevN
 * the sensor is set to W x H UYVY first. V4L_NV12=1 asks for NV12.
 * V4L_GUARD=1 allocates twice the image size, fills it with 0xa5 and
 * reports writes past the image. V4L_CTRL=saturation=160,contrast=128 sets
 * brightness/contrast/saturation on the V4L_SUBDEV sensor first, with
 * V4L_CTRL_LATE=1 after STREAMON instead (the running sensor).
 *
 *   v4lgrab /dev/videoN W H frames out.nv16
 */

#include <errno.h>
#include <fcntl.h>
#include <linux/media-bus-format.h>
#include <linux/media.h>
#include <linux/v4l2-subdev.h>
#include <linux/videodev2.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#define NBUF 4

static void die(const char *what)
{
	fprintf(stderr, "v4lgrab: %s: %s\n", what, strerror(errno));
	exit(1);
}

/* "name=value,..." for brightness, contrast, saturation on a subdev */
static void set_ctrls(int fd, char *list)
{
	static const struct { const char *name; __u32 id; } names[] = {
		{ "brightness", V4L2_CID_BRIGHTNESS },
		{ "contrast", V4L2_CID_CONTRAST },
		{ "saturation", V4L2_CID_SATURATION },
	};
	char *item, *save;
	unsigned int i;

	for (item = strtok_r(list, ",", &save); item;
	     item = strtok_r(NULL, ",", &save)) {
		char *eq = strchr(item, '=');
		struct v4l2_control c = { 0 };

		if (!eq)
			die("V4L_CTRL needs name=value");
		*eq = 0;
		for (i = 0; i < sizeof(names) / sizeof(names[0]); i++)
			if (!strcmp(item, names[i].name))
				c.id = names[i].id;
		if (!c.id)
			die(item);
		c.value = atoi(eq + 1);
		if (ioctl(fd, VIDIOC_S_CTRL, &c))
			die("VIDIOC_S_CTRL");
		printf("%s = %d\n", item, c.value);
	}
}

/* enable the link from the sensor named by prefix, disable the others */
static void select_sensor(const char *prefix)
{
	struct media_entity_desc ent = { 0 }, sensors[8], video = { 0 };
	int fd, i, n = 0, pass;

	fd = open("/dev/media0", O_RDWR);
	if (fd < 0)
		die("/dev/media0");
	for (ent.id = MEDIA_ENT_ID_FLAG_NEXT;
	     !ioctl(fd, MEDIA_IOC_ENUM_ENTITIES, &ent);
	     ent.id |= MEDIA_ENT_ID_FLAG_NEXT) {
		if (ent.type == MEDIA_ENT_F_CAM_SENSOR && n < 8)
			sensors[n++] = ent;
		else if (ent.type == MEDIA_ENT_F_IO_V4L)
			video = ent;
	}
	if (!video.id)
		die("no video entity");
	/* disable first, then enable: only one link may be enabled */
	for (pass = 0; pass < 2; pass++) {
		for (i = 0; i < n; i++) {
			int match = !strncmp(sensors[i].name, prefix,
					     strlen(prefix));
			struct media_link_desc l = {
				.source = { .entity = sensors[i].id,
					    .flags = MEDIA_PAD_FL_SOURCE },
				.sink = { .entity = video.id,
					  .flags = MEDIA_PAD_FL_SINK },
				.flags = match ? MEDIA_LNK_FL_ENABLED : 0,
			};

			if (match != pass)
				continue;
			if (ioctl(fd, MEDIA_IOC_SETUP_LINK, &l))
				die("SETUP_LINK");
			if (match)
				printf("link %s -> %s enabled\n",
				       sensors[i].name, video.name);
		}
	}
	close(fd);
}

int main(int argc, char **argv)
{
	struct v4l2_requestbuffers req = { 0 };
	struct v4l2_format fmt = { 0 };
	struct v4l2_buffer b;
	void *mem[NBUF];
	size_t len[NBUF];
	double t0 = 0, last = 0;
	int fd, i, n, type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	FILE *out;

	if (argc != 6) {
		fprintf(stderr, "usage: v4lgrab /dev/videoN W H frames out.nv16\n");
		return 2;
	}
	if (getenv("V4L_SENSOR"))
		select_sensor(getenv("V4L_SENSOR"));
	if (getenv("V4L_SUBDEV")) {
		struct v4l2_subdev_format sf = {
			.which = V4L2_SUBDEV_FORMAT_ACTIVE,
			.format.width = atoi(argv[2]),
			.format.height = atoi(argv[3]),
			.format.code = MEDIA_BUS_FMT_UYVY8_1X16,
		};

		fd = open(getenv("V4L_SUBDEV"), O_RDWR);
		if (fd < 0 || ioctl(fd, VIDIOC_SUBDEV_S_FMT, &sf))
			die("SUBDEV_S_FMT");
		printf("sensor %ux%u code %#x\n", sf.format.width,
		       sf.format.height, sf.format.code);
		if (getenv("V4L_CTRL") && !getenv("V4L_CTRL_LATE"))
			set_ctrls(fd, getenv("V4L_CTRL"));
		close(fd);
	}
	fd = open(argv[1], O_RDWR);
	if (fd < 0)
		die(argv[1]);
	fmt.type = type;
	fmt.fmt.pix.width = atoi(argv[2]);
	fmt.fmt.pix.height = atoi(argv[3]);
	fmt.fmt.pix.pixelformat = getenv("V4L_NV12") ? V4L2_PIX_FMT_NV12 :
						      V4L2_PIX_FMT_NV16;
	if (ioctl(fd, VIDIOC_S_FMT, &fmt))
		die("S_FMT");
	printf("format %ux%u %.4s bpl %u size %u\n", fmt.fmt.pix.width,
	       fmt.fmt.pix.height, (char *)&fmt.fmt.pix.pixelformat,
	       fmt.fmt.pix.bytesperline, fmt.fmt.pix.sizeimage);

	if (getenv("V4L_GUARD")) {
		/* buffers twice the image size, to see the DMA overrun */
		struct v4l2_create_buffers cb = { .count = NBUF,
						  .memory = V4L2_MEMORY_MMAP,
						  .format = fmt };

		cb.format.fmt.pix.sizeimage *= 2;
		if (ioctl(fd, VIDIOC_CREATE_BUFS, &cb) || cb.count != NBUF)
			die("CREATE_BUFS");
	} else {
		req.count = NBUF;
		req.type = type;
		req.memory = V4L2_MEMORY_MMAP;
		if (ioctl(fd, VIDIOC_REQBUFS, &req) || req.count != NBUF)
			die("REQBUFS");
	}
	for (i = 0; i < NBUF; i++) {
		memset(&b, 0, sizeof(b));
		b.type = type;
		b.memory = V4L2_MEMORY_MMAP;
		b.index = i;
		if (ioctl(fd, VIDIOC_QUERYBUF, &b))
			die("QUERYBUF");
		mem[i] = mmap(NULL, b.length, PROT_READ | PROT_WRITE, MAP_SHARED,
			      fd, b.m.offset);
		if (mem[i] == MAP_FAILED)
			die("mmap");
		len[i] = b.length;
		memset(mem[i], 0xa5, b.length);
		if (ioctl(fd, VIDIOC_QBUF, &b))
			die("QBUF");
	}
	if (ioctl(fd, VIDIOC_STREAMON, &type))
		die("STREAMON");
	if (getenv("V4L_CTRL") && getenv("V4L_CTRL_LATE")) {
		int sfd = open(getenv("V4L_SUBDEV"), O_RDWR);

		if (sfd < 0)
			die("V4L_SUBDEV");
		set_ctrls(sfd, getenv("V4L_CTRL"));
		close(sfd);
	}

	n = atoi(argv[4]);
	for (i = 0; i < n; i++) {
		double t;

		memset(&b, 0, sizeof(b));
		b.type = type;
		b.memory = V4L2_MEMORY_MMAP;
		if (ioctl(fd, VIDIOC_DQBUF, &b))
			die("DQBUF");
		t = b.timestamp.tv_sec + b.timestamp.tv_usec / 1e6;
		if (!i)
			t0 = t;
		printf("frame %2d: buf %u seq %u bytes %u flags %#x dt %.1f ms\n",
		       i, b.index, b.sequence, b.bytesused, b.flags,
		       i ? (t - last) * 1000 : 0.0);
		last = t;
		if (getenv("V4L_GUARD")) {
			const unsigned char *p = mem[b.index];
			size_t k, touched = 0;

			for (k = fmt.fmt.pix.sizeimage; k < len[b.index]; k++)
				touched += p[k] != 0xa5;
			printf("  guard: %zu of %zu bytes after the image changed\n",
			       touched, len[b.index] - fmt.fmt.pix.sizeimage);
		}
		if (i == n - 1) {
			out = fopen(argv[5], "wb");
			if (!out || fwrite(mem[b.index], 1, b.bytesused, out) !=
			    b.bytesused || fclose(out))
				die(argv[5]);
		} else if (ioctl(fd, VIDIOC_QBUF, &b)) {
			die("QBUF");
		}
	}
	if (n > 1)
		printf("%.2f fps\n", (n - 1) / (last - t0));
	if (ioctl(fd, VIDIOC_STREAMOFF, &type))
		die("STREAMOFF");
	return 0;
}
