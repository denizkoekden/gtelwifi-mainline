// SPDX-License-Identifier: GPL-2.0
/*
 * pcmplay: minimal ALSA player for the rescue system (no alsa-lib), raw
 * ioctls on /dev/snd. Plays a tone or a 16-bit WAV (stereo is mixed down,
 * the gtelwifi speaker path is mono) and sets mixer controls by name.
 * -2 sends the mono signal to both DACs (both are mixed into the speaker),
 * interleaved, or with -n non-interleaved (two DMA layouts in sprd-pcm-dma).
 *
 *   pcmplay -l                         list controls
 *   pcmplay -c 'name=v[,v]' ...        set controls, then play (optional)
 *   pcmplay [-r rate] [-t s] [-f Hz]   tone (default 48000 Hz, 3 s, 1 kHz)
 *   pcmplay [-r rate] -s [-t s]        sweep 200 Hz .. 8 kHz
 *   pcmplay file.wav | -               WAV from file or stdin
 *   pcmplay -2 [-n] ...                same signal on both channels
 *   pcmplay -k N                       N x prepare, start, drop on one hw_params
 *                                      (silence, 2..6 periods, so some stops hit a
 *                                      running transfer): the restart sequence
 *                                      PipeWire uses, for the DMA descriptor life cycle
 */

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>
#include <sound/asound.h>

#define CARD		"/dev/snd/controlC0"
#define PCM		"/dev/snd/pcmC0D0p"
#define PERIOD		960		/* frames; 1920 bytes = 3 x 640 */
#define PERIODS		4

static unsigned int nch = 1, nonint;

static void die(const char *what)
{
	fprintf(stderr, "pcmplay: %s: %s\n", what, strerror(errno));
	exit(1);
}

/* ---- controls ---- */

static int ctl_open(void)
{
	int fd = open(CARD, O_RDWR);

	if (fd < 0)
		die(CARD);
	return fd;
}

static void ctl_list(void)
{
	struct snd_ctl_elem_list list = { 0 };
	struct snd_ctl_elem_id *ids;
	unsigned int i, j;
	int fd = ctl_open();

	if (ioctl(fd, SNDRV_CTL_IOCTL_ELEM_LIST, &list) < 0)
		die("ELEM_LIST");
	ids = calloc(list.count, sizeof(*ids));
	list.space = list.count;
	list.pids = ids;
	if (ioctl(fd, SNDRV_CTL_IOCTL_ELEM_LIST, &list) < 0)
		die("ELEM_LIST");
	for (i = 0; i < list.used; i++) {
		struct snd_ctl_elem_info info = { .id = ids[i] };
		struct snd_ctl_elem_value val = { .id = ids[i] };

		if (ioctl(fd, SNDRV_CTL_IOCTL_ELEM_INFO, &info) < 0 ||
		    ioctl(fd, SNDRV_CTL_IOCTL_ELEM_READ, &val) < 0)
			continue;
		printf("%3u '%s'", ids[i].numid, (char *)ids[i].name);
		if (info.type == SNDRV_CTL_ELEM_TYPE_INTEGER)
			printf(" int %ld..%ld:", info.value.integer.min,
			       info.value.integer.max);
		else if (info.type == SNDRV_CTL_ELEM_TYPE_BOOLEAN)
			printf(" bool:");
		else
			printf(" type %d:", info.type);
		for (j = 0; j < info.count; j++)
			printf(" %ld", val.value.integer.value[j]);
		printf("\n");
	}
	close(fd);
}

static void ctl_set(const char *arg)
{
	struct snd_ctl_elem_info info = { 0 };
	struct snd_ctl_elem_value val = { 0 };
	const char *eq = strrchr(arg, '=');
	char *end;
	long v = 0;
	unsigned int i;
	int fd;

	if (!eq || eq == arg || eq - arg >= (int)sizeof(info.id.name)) {
		fprintf(stderr, "pcmplay: bad control '%s'\n", arg);
		exit(1);
	}
	fd = ctl_open();
	info.id.iface = SNDRV_CTL_ELEM_IFACE_MIXER;
	memcpy(info.id.name, arg, eq - arg);
	if (ioctl(fd, SNDRV_CTL_IOCTL_ELEM_INFO, &info) < 0)
		die(arg);
	val.id = info.id;
	end = (char *)eq;
	for (i = 0; i < info.count; i++) {
		if (*end == '=' || *end == ',')
			v = strtol(end + 1, &end, 0);
		val.value.integer.value[i] = v;
	}
	if (ioctl(fd, SNDRV_CTL_IOCTL_ELEM_WRITE, &val) < 0)
		die(arg);
	close(fd);
}

/* ---- PCM ---- */

static struct snd_mask *mask(struct snd_pcm_hw_params *p, int n)
{
	return &p->masks[n - SNDRV_PCM_HW_PARAM_FIRST_MASK];
}

static struct snd_interval *ival(struct snd_pcm_hw_params *p, int n)
{
	return &p->intervals[n - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL];
}

static void set_mask(struct snd_pcm_hw_params *p, int n, unsigned int bit)
{
	struct snd_mask *m = mask(p, n);

	memset(m, 0, sizeof(*m));
	m->bits[bit >> 5] = 1u << (bit & 31);
}

static void set_int(struct snd_pcm_hw_params *p, int n, unsigned int v)
{
	struct snd_interval *i = ival(p, n);

	i->min = i->max = v;
	i->integer = 1;
}

static int pcm_open(unsigned int rate)
{
	struct snd_pcm_hw_params p;
	int fd, n;

	fd = open(PCM, O_RDWR);
	if (fd < 0)
		die(PCM);
	memset(&p, 0, sizeof(p));
	for (n = SNDRV_PCM_HW_PARAM_FIRST_MASK;
	     n <= SNDRV_PCM_HW_PARAM_LAST_MASK; n++)
		memset(mask(&p, n), 0xff, sizeof(struct snd_mask));
	for (n = SNDRV_PCM_HW_PARAM_FIRST_INTERVAL;
	     n <= SNDRV_PCM_HW_PARAM_LAST_INTERVAL; n++)
		ival(&p, n)->max = ~0u;
	p.rmask = ~0u;
	p.info = ~0u;
	set_mask(&p, SNDRV_PCM_HW_PARAM_ACCESS,
		 nonint ? SNDRV_PCM_ACCESS_RW_NONINTERLEAVED :
			  SNDRV_PCM_ACCESS_RW_INTERLEAVED);
	set_mask(&p, SNDRV_PCM_HW_PARAM_FORMAT, SNDRV_PCM_FORMAT_S16_LE);
	set_mask(&p, SNDRV_PCM_HW_PARAM_SUBFORMAT, SNDRV_PCM_SUBFORMAT_STD);
	set_int(&p, SNDRV_PCM_HW_PARAM_SAMPLE_BITS, 16);
	set_int(&p, SNDRV_PCM_HW_PARAM_FRAME_BITS, 16 * nch);
	set_int(&p, SNDRV_PCM_HW_PARAM_CHANNELS, nch);
	set_int(&p, SNDRV_PCM_HW_PARAM_RATE, rate);
	set_int(&p, SNDRV_PCM_HW_PARAM_PERIOD_SIZE, PERIOD);
	set_int(&p, SNDRV_PCM_HW_PARAM_PERIODS, PERIODS);
	if (ioctl(fd, SNDRV_PCM_IOCTL_HW_PARAMS, &p) < 0)
		die("HW_PARAMS");
	if (ioctl(fd, SNDRV_PCM_IOCTL_PREPARE) < 0)
		die("PREPARE");
	return fd;
}

static void pcm_write(int fd, int16_t *buf, unsigned int frames)
{
	static int16_t two[PERIOD * 2];
	void *bufs[2] = { buf, buf };
	struct snd_xfern xn = { .bufs = bufs, .frames = frames };
	struct snd_xferi xi = { .buf = buf, .frames = frames };
	unsigned int i;

	if (nch == 2 && !nonint) {
		for (i = 0; i < frames; i++)
			two[2 * i] = two[2 * i + 1] = buf[i];
		xi.buf = two;
	}
	while (nonint ? ioctl(fd, SNDRV_PCM_IOCTL_WRITEN_FRAMES, &xn) :
			ioctl(fd, SNDRV_PCM_IOCTL_WRITEI_FRAMES, &xi)) {
		if (errno != EPIPE)
			die("write");
		fprintf(stderr, "pcmplay: underrun\n");
		if (ioctl(fd, SNDRV_PCM_IOCTL_PREPARE) < 0)
			die("PREPARE");
	}
}

/* ---- restart test ---- */

static void restart_loop(unsigned int rate, unsigned int n)
{
	static int16_t silence[PERIOD];
	int fd = pcm_open(rate);
	unsigned int i, j;

	for (i = 0; i < n; i++) {
		if (i && ioctl(fd, SNDRV_PCM_IOCTL_PREPARE) < 0)
			die("PREPARE");
		/* more than the 4-period buffer blocks until periods have played */
		for (j = 0; j < 2 + i % 5; j++)
			pcm_write(fd, silence, PERIOD);
		if (ioctl(fd, SNDRV_PCM_IOCTL_DROP) < 0)
			die("DROP");
		if (i % 100 == 99)
			printf("%u restarts\n", i + 1);
	}
	close(fd);
}

/* ---- sources ---- */

static void play_tone(unsigned int rate, double secs, double f0, int sweep)
{
	int16_t buf[PERIOD];
	unsigned long total = secs * rate, done = 0;
	double ph = 0, f = f0;
	int fd = pcm_open(rate);
	unsigned int i;

	while (done < total) {
		for (i = 0; i < PERIOD; i++, done++) {
			if (sweep)	/* exponential 200 Hz .. 8 kHz */
				f = 200 * pow(40, (double)done / total);
			/* 3 ms fade in/out against clicks */
			double env = 1, t = (double)done / rate,
			       rest = (double)(total - done) / rate;

			if (t < 0.003)
				env = t / 0.003;
			if (rest < 0.003)
				env = rest / 0.003;
			buf[i] = done < total ?
				 (int16_t)(env * 16000 * sin(ph)) : 0;
			ph += 2 * M_PI * f / rate;
			if (ph > 2 * M_PI)
				ph -= 2 * M_PI;
		}
		pcm_write(fd, buf, PERIOD);
	}
	ioctl(fd, SNDRV_PCM_IOCTL_DRAIN);
	close(fd);
}

static int rd(int fd, void *p, size_t n)
{
	size_t got = 0;
	ssize_t r;

	while (got < n) {
		r = read(fd, (char *)p + got, n - got);
		if (r <= 0)
			return got;
		got += r;
	}
	return got;
}

static uint32_t le32(const uint8_t *p)
{
	return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
}

static void play_wav(const char *name)
{
	int in = strcmp(name, "-") ? open(name, O_RDONLY) : 0;
	uint8_t h[12], ck[8], fmt[16];
	unsigned int ch = 0, rate = 0, bits = 0, i, n;
	int16_t raw[PERIOD * 8], buf[PERIOD];
	uint32_t len;
	int fd;

	if (in < 0)
		die(name);
	if (rd(in, h, 12) != 12 || memcmp(h, "RIFF", 4) || memcmp(h + 8, "WAVE", 4)) {
		fprintf(stderr, "pcmplay: not a WAV file\n");
		exit(1);
	}
	for (;;) {
		if (rd(in, ck, 8) != 8) {
			fprintf(stderr, "pcmplay: no data chunk\n");
			exit(1);
		}
		len = le32(ck + 4);
		if (!memcmp(ck, "fmt ", 4)) {
			rd(in, fmt, 16);
			ch = fmt[2] | fmt[3] << 8;
			rate = le32(fmt + 4);
			bits = fmt[14] | fmt[15] << 8;
			for (len -= 16; len; len--)
				rd(in, ck, 1);
		} else if (!memcmp(ck, "data", 4)) {
			break;
		} else {
			for (len += len & 1; len; len--)
				rd(in, ck, 1);
		}
	}
	if (bits != 16 || ch < 1 || ch > 8) {
		fprintf(stderr, "pcmplay: need 16-bit PCM, got %u bit %u ch\n",
			bits, ch);
		exit(1);
	}
	printf("pcmplay: %u Hz, %u ch, %u s\n", rate, ch, len / 2 / ch / rate);
	fd = pcm_open(rate);
	for (;;) {
		n = rd(in, raw, PERIOD * ch * 2) / 2 / ch;
		if (!n)
			break;
		for (i = 0; i < n; i++) {
			int s = 0, c;

			for (c = 0; c < (int)ch; c++)
				s += raw[i * ch + c];
			buf[i] = s / (int)ch;
		}
		if (n < PERIOD)
			memset(buf + n, 0, (PERIOD - n) * 2);
		pcm_write(fd, buf, PERIOD);
		if (n < PERIOD)
			break;
	}
	ioctl(fd, SNDRV_PCM_IOCTL_DRAIN);
	close(fd);
}

int main(int argc, char **argv)
{
	unsigned int rate = 48000;
	double secs = 3, freq = 1000;
	int opt, sweep = 0, ctls = 0, restarts = 0;

	while ((opt = getopt(argc, argv, "lc:r:t:f:s2nk:")) != -1) {
		switch (opt) {
		case '2':
			nch = 2;
			break;
		case 'n':
			nonint = 1;
			break;
		case 'l':
			ctl_list();
			return 0;
		case 'c':
			ctl_set(optarg);
			ctls++;
			break;
		case 'r':
			rate = atoi(optarg);
			break;
		case 't':
			secs = atof(optarg);
			break;
		case 'f':
			freq = atof(optarg);
			break;
		case 's':
			sweep = 1;
			break;
		case 'k':
			restarts = atoi(optarg);
			break;
		default:
			return 2;
		}
	}
	if (restarts)
		restart_loop(rate, restarts);
	else if (optind < argc)
		play_wav(argv[optind]);
	else if (!ctls || sweep || secs != 3 || freq != 1000)
		play_tone(rate, secs, freq, sweep);
	return 0;
}
