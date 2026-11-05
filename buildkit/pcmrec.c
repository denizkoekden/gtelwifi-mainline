// SPDX-License-Identifier: GPL-2.0
/*
 * pcmrec: minimal ALSA recorder for the rescue system (no alsa-lib), raw
 * ioctls on /dev/snd, the counterpart of pcmplay. Records 16-bit PCM, prints
 * peak and RMS per channel and the level of one frequency (Goertzel), and
 * writes a WAV file if one is given.
 *
 *   pcmrec [-r rate] [-c channels] [-t s] [-f Hz] [-n] [out.wav]
 *
 * -n records non-interleaved (the second DMA layout in sprd-pcm-dma).
 * Mixer controls are set with pcmplay -c.
 */

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <sound/asound.h>

#define PCM		"/dev/snd/pcmC0D0c"
#define PERIOD		960
#define PERIODS		4

static void die(const char *what)
{
	fprintf(stderr, "pcmrec: %s: %s\n", what, strerror(errno));
	exit(1);
}

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

static void put32(unsigned char *b, uint32_t v)
{
	b[0] = v; b[1] = v >> 8; b[2] = v >> 16; b[3] = v >> 24;
}

static void wav_header(FILE *f, unsigned int rate, unsigned int nch,
		       uint32_t bytes)
{
	unsigned char h[44] = "RIFF....WAVEfmt ....\1\0..........\0\0data....";

	put32(h + 4, 36 + bytes);
	put32(h + 16, 16);
	h[22] = nch;
	put32(h + 24, rate);
	put32(h + 28, rate * nch * 2);
	h[32] = nch * 2;
	h[34] = 16;
	put32(h + 40, bytes);
	fseek(f, 0, SEEK_SET);
	fwrite(h, 1, sizeof(h), f);
}

int main(int argc, char **argv)
{
	unsigned int rate = 48000, nch = 1, nonint = 0, i, c;
	double secs = 3, freq = 1000;
	struct snd_pcm_hw_params p;
	static int16_t buf[2][PERIOD * 2];
	double sum[2] = { 0 }, s1[2] = { 0 }, s2[2] = { 0 }, coeff;
	int peak[2] = { 0 }, xruns = 0, fd, n, opt;
	unsigned long total, done = 0;
	FILE *out = NULL;

	while ((opt = getopt(argc, argv, "r:c:t:f:n")) != -1) {
		switch (opt) {
		case 'r': rate = atoi(optarg); break;
		case 'c': nch = atoi(optarg) == 2 ? 2 : 1; break;
		case 't': secs = atof(optarg); break;
		case 'f': freq = atof(optarg); break;
		case 'n': nonint = 1; break;
		default:
			fprintf(stderr, "usage: pcmrec [-r rate] [-c 1|2] [-t s] [-f Hz] [-n] [out.wav]\n");
			return 2;
		}
	}
	if (optind < argc) {
		out = fopen(argv[optind], "wb");
		if (!out)
			die(argv[optind]);
		wav_header(out, rate, nch, 0);
	}

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
	if (ioctl(fd, SNDRV_PCM_IOCTL_START) < 0)
		die("START");

	coeff = 2 * cos(2 * M_PI * freq / rate);
	total = secs * rate;
	while (done < total) {
		unsigned int frames = total - done < PERIOD ? total - done : PERIOD;
		void *bufs[2] = { buf[0], buf[1] };
		struct snd_xfern xn = { .bufs = bufs, .frames = frames };
		struct snd_xferi xi = { .buf = buf[0], .frames = frames };
		int r = nonint ? ioctl(fd, SNDRV_PCM_IOCTL_READN_FRAMES, &xn) :
				 ioctl(fd, SNDRV_PCM_IOCTL_READI_FRAMES, &xi);

		if (r < 0) {
			if (errno != EPIPE)
				die("READ");
			xruns++;
			if (ioctl(fd, SNDRV_PCM_IOCTL_PREPARE) < 0 ||
			    ioctl(fd, SNDRV_PCM_IOCTL_START) < 0)
				die("restart");
			continue;
		}
		frames = nonint ? xn.result : xi.result;
		for (i = 0; i < frames; i++) {
			for (c = 0; c < nch; c++) {
				int v = nonint ? buf[c][i] : buf[0][i * nch + c];
				double s0 = v + coeff * s1[c] - s2[c];

				s2[c] = s1[c];
				s1[c] = s0;
				sum[c] += (double)v * v;
				if (abs(v) > peak[c])
					peak[c] = abs(v);
			}
			if (out && nonint) {
				for (c = 0; c < nch; c++)
					fwrite(&buf[c][i], 2, 1, out);
			}
		}
		if (out && !nonint)
			fwrite(buf[0], 2 * nch, frames, out);
		done += frames;
	}
	close(fd);
	if (out) {
		wav_header(out, rate, nch, done * 2 * nch);
		fclose(out);
	}

	printf("%lu frames %u Hz %u ch, xruns %d\n", done, rate, nch, xruns);
	for (c = 0; c < nch; c++) {
		double rms = sqrt(sum[c] / done);
		double mag = sqrt(s1[c] * s1[c] + s2[c] * s2[c] -
				  coeff * s1[c] * s2[c]) * 2 / done;

		printf("ch%u: peak %d (%.1f dBFS) rms %.1f (%.1f dBFS), %.0f Hz %.1f dBFS\n",
		       c, peak[c], 20 * log10((peak[c] + 1e-9) / 32768), rms,
		       20 * log10((rms + 1e-9) / 32768), freq,
		       20 * log10((mag + 1e-9) / 32768));
	}
	return 0;
}
