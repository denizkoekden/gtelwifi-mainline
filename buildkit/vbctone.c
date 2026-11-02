// SPDX-License-Identifier: GPL-2.0
/*
 * vbctone: first sound on the gtelwifi without an audio driver and without
 * DMA. Powers the SC2723 codec, its DAC, the AOL mixer and the internal
 * speaker PA, then fills both halves of the VBC DA ping-pong RAM with one
 * sine period each and lets the VBC replay its RAM: 48000 / 160 = 300 Hz.
 * Register sequence from vendor-ref/research/audio-report.md (sections 2.4
 * and 4.2), i.e. the vendor 3.10 sprd-codec v4 + VBC r2p0 drivers.
 *
 * SoC registers through /dev/mem, PMIC registers through the sprd-adi
 * debugfs files (under the kernel's ADI lock). Run with the PMIC watchdog
 * armed. Usage: vbctone [seconds] [aol_gain 0-15] [dac_gain 0-7] [dma]
 *
 * With "dma" the VBC does not replay its RAM: a 440 Hz sine for the whole
 * duration lies in the bootloader framebuffer reservation (no-map, so
 * uncached through /dev/mem) and AP DMA channel 24 (0-based) feeds it to
 * the DA0 FIFO on VBC request UID 29, one 160-sample fragment per request
 * (stage 4 of vendor-ref/research/dma-report.md). No DMA driver may be bound.
 */

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define ADI_DIR		"/sys/kernel/debug/sprd-adi/"
#define AON_APB		0x402e0000
#define VBC		0x40020000
#define DP		0x40000000
#define FIFO_FRAMES	160
#define AP_AHB		0x20d00000
#define DMA		0x20100000
#define DMA_CH		0x1600		/* hardware channel 24, 1-based 25 */
#define DMA_UID29	0x2070
#define PCM_BUF		0x9f000000
#define RATE		48000

static volatile uint32_t *aon, *dp;
static volatile uint8_t *vbc;

static void die(const char *what)
{
	fprintf(stderr, "vbctone: %s: %s\n", what, strerror(errno));
	exit(1);
}

static void msleep(unsigned int ms)
{
	struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };

	nanosleep(&ts, NULL);
}

static void *map(int fd, uint32_t pa, size_t len)
{
	void *p = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, pa);

	if (p == MAP_FAILED)
		die("mmap");
	return p;
}

static uint32_t adi_read(uint32_t reg)
{
	char buf[32];
	FILE *f = fopen(ADI_DIR "address", "w");

	if (!f || fprintf(f, "0x%x", reg) < 0 || fclose(f))
		die("adi address");
	f = fopen(ADI_DIR "value", "r");
	if (!f || !fgets(buf, sizeof(buf), f))
		die("adi read");
	fclose(f);
	return strtoul(buf, NULL, 0);
}

static void adi_write(uint32_t reg, uint32_t val)
{
	FILE *f = fopen(ADI_DIR "address", "w");

	if (!f || fprintf(f, "0x%x", reg) < 0 || fclose(f))
		die("adi address");
	f = fopen(ADI_DIR "value", "w");
	if (!f || fprintf(f, "0x%x", val) < 0 || fclose(f))
		die("adi write");
}

static void adi_rmw(uint32_t reg, uint32_t clr, uint32_t set)
{
	adi_write(reg, (adi_read(reg) & ~clr) | set);
}

static void rmw32(volatile uint32_t *r, uint32_t clr, uint32_t set)
{
	*r = (*r & ~clr) | set;
}

#define VBC32(off)	(*(volatile uint32_t *)(vbc + (off)))
#define VBC16(off)	(*(volatile uint16_t *)(vbc + (off)))

static void vbc_fill(int half, const int16_t *s)
{
	int i;

	/* RAMSW_EN (bit 10) gives the CPU the half selected by RAMSW_NUMB (bit 9) */
	rmw32(&VBC32(0x18), 1u << 9, (1u << 10) | ((uint32_t)half << 9));
	for (i = 0; i < FIFO_FRAMES; i++) {
		VBC16(0x00) = s[i];
		VBC16(0x04) = s[i];
	}
	rmw32(&VBC32(0x18), (1u << 10) | (1u << 9), 0);
}

int main(int argc, char **argv)
{
	int secs = argc > 1 ? atoi(argv[1]) : 3;
	unsigned int aol = argc > 2 ? atoi(argv[2]) : 6;
	unsigned int dac = argc > 3 ? atoi(argv[3]) : 5;
	int16_t sine[FIFO_FRAMES], zero[FIFO_FRAMES] = { 0 };
	int use_dma = argc > 4 && !strcmp(argv[4], "dma");
	volatile uint32_t *ahb = NULL, *dma = NULL;
	volatile int16_t *pcm = NULL;
	uint32_t len = 0;
	int fd, i;

	for (i = 0; i < FIFO_FRAMES; i++)
		sine[i] = (int16_t)(8000 * sin(2 * M_PI * i / FIFO_FRAMES));

	fd = open("/dev/mem", O_RDWR | O_SYNC);
	if (fd < 0)
		die("/dev/mem");
	aon = map(fd, AON_APB, 0x4000);

	printf("chip id %04x, EB0 %08x, VBC_CTRL %08x, WPROT %08x\n",
	       adi_read(0x93c), aon[0], aon[0x20 / 4], aon[0x3040 / 4]);

	/* SoC: VBC + DP clocks first, then ownership, 26 MHz, resets */
	rmw32(&aon[0x00 / 4], 0, (1u << 19) | (1u << 18));
	rmw32(&aon[0x20 / 4], 0x000ffff0 | 0xf, 1u << 20);
	rmw32(&aon[0x3040 / 4], 1u << 6, 0);
	rmw32(&aon[0x3044 / 4], 0, 1u << 6);
	rmw32(&aon[0x3048 / 4], 0, 1u << 6);
	rmw32(&aon[0x304c / 4], 0, 1u << 6);
	rmw32(&aon[0x18 / 4], 0, 1u << 0);
	rmw32(&aon[0x08 / 4], 0, (1u << 20) | (1u << 19) | (1u << 18));
	usleep(10);
	rmw32(&aon[0x08 / 4], (1u << 20) | (1u << 19) | (1u << 18), 0);
	vbc = map(fd, VBC, 0x1000);
	dp = map(fd, DP, 0x1000);

	/* PMIC global: audio on, analog reset, clocks */
	adi_rmw(0x800, 0, 1u << 4);
	adi_write(0x8a4, 0);
	adi_rmw(0x80c, 0, (1u << 9) | (1u << 10) | (1u << 11));
	usleep(10);
	adi_rmw(0x80c, (1u << 9) | (1u << 10) | (1u << 11), 0);
	adi_rmw(0x804, 0, (1u << 0) | (1u << 1));
	adi_rmw(0x8fc, 0, 1u << 1);
	adi_rmw(0x400, 1u << 4, 0);
	adi_rmw(0x808, 0, 1u << 4);
	adi_rmw(0x8e8, 0, 1u << 8);

	/* analog power: VB, VBO, BG, BG_IBIAS, VCM, VCM_BUF, VCMI, OVP_LDO */
	adi_rmw(0x600, 0, (1u << 9) | (1u << 8));
	adi_rmw(0x604, 0, (1u << 15) | (1u << 14) | (1u << 13) | (1u << 12) |
			  (1u << 11) | (1u << 10));
	adi_rmw(0x600, 0, 1u << 11);
	msleep(10);

	/* DP: unmute, 48 kHz, SDM, digital DAC L/R */
	rmw32(&dp[0x0c / 4], (1u << 15) | 0xf, 1);
	rmw32(&dp[0x10 / 4], 0xf << 8, 7 << 8);
	rmw32(&dp[0x00 / 4], 0, (1u << 0) | (1u << 2));

	/* analog DAC -> AOL */
	adi_rmw(0x61c, 0, (1u << 11) | (1u << 8));
	adi_rmw(0x68c, 0, (1u << 9) | (1u << 11));
	adi_rmw(0x644, 0xfc00, (dac << 13) | (dac << 10));
	adi_rmw(0x624, 0, (1u << 15) | (1u << 14));
	adi_rmw(0x634, 0, (1u << 13) | (1u << 12));
	adi_rmw(0x624, 0, 1u << 4);
	adi_rmw(0x600, 0, 1u << 15);
	adi_rmw(0x648, 0xf000, aol << 12);

	/* VBC: 160-frame halves, DA port 0, EQ/ALC off, sine in both halves */
	VBC32(0x10) = (FIFO_FRAMES - 1) << 8 | (FIFO_FRAMES - 1);
	VBC32(0x3c) = 1u << 14;
	VBC32(0x48) = 0;
	vbc_fill(0, sine);
	vbc_fill(1, sine);

	/* speaker PA as the HAL configures it (0x130): class AB, DEMI, DTRI_F 3 */
	adi_rmw(0x60c, 7u << 11, 0);
	adi_rmw(0x614, 7u << 13, 3u << 13);
	adi_rmw(0x62c, 1u << 15, 0);
	adi_write(0x618, 0x0400);
	adi_rmw(0x624, 3u << 10, (1u << 13) | (3u << 10));
	adi_rmw(0x62c, 0, 1u << 12);
	adi_rmw(0x618, (1u << 15) | (1u << 14), 0);
	adi_rmw(0x600, 1u << 14, 1u << 15);
	adi_rmw(0x62c, 3u << 13, 3u << 13);
	msleep(20);
	adi_rmw(0x600, 0, 1u << 13);

	if (use_dma) {
		/* 440 Hz, mono, 16 bit, the whole duration */
		len = secs * RATE * 2;
		pcm = map(fd, PCM_BUF, (len + 0xfff) & ~0xfff);
		for (i = 0; i < secs * RATE; i++)
			pcm[i] = (int16_t)(8000 * sin(2 * M_PI * 440 * i / RATE));
		vbc_fill(0, zero);
		vbc_fill(1, zero);

		ahb = map(fd, AP_AHB, 0x1000);
		ahb[0] |= 1u << 5;			/* DMA_EB */
		dma = map(fd, DMA, 0x4000);
#define D(off)	dma[(DMA_CH + (off)) / 4]
		D(0x00) = 0;
		D(0x04) = 0;
		D(0x08) = 0x00001000;
		D(0x0c) = 0x1f000000;
		D(0x10) = PCM_BUF;
		D(0x14) = VBC;			/* VBDA0 */
		D(0x18) = 0x50300140;		/* 2 B/2 B, FRAG_REQ, dst fixed, frag 320 */
		D(0x1c) = FIFO_FRAMES * 2;
		D(0x20) = len;
		D(0x24) = 0x00000002;		/* src step 2, dst step 0 */
		for (i = 0x28; i <= 0x3c; i += 4)
			D(i) = 0;
		dma[DMA_UID29 / 4] = 25;	/* VB_DA0 request -> channel 25 */
		D(0x08) |= 1;
		/* go: DA0 only, its DMA request, VBENABLE */
		rmw32(&VBC32(0xc8), 0, 1u << 0);
		rmw32(&VBC32(0x18), 0, (1u << 13) | (1u << 15));
	} else {
		/* go: DA0 + DA1, VBENABLE, no DMA requests */
		rmw32(&VBC32(0xc8), 0, (1u << 0) | (1u << 1));
		rmw32(&VBC32(0x18), 0, 1u << 15);
	}
	printf("playing %d s: AOL gain %u, DAC gain %u; DP TOP %08x DAC %08x, PMU0 %04x CDC2 %04x\n",
	       secs, aol, dac, dp[0], dp[0x0c / 4], adi_read(0x600), adi_read(0x624));
	if (use_dma) {
		for (i = 0; i < secs * 4; i++) {
			msleep(250);
			if (i % 4 == 3)
				printf("  dma SRC %08x left %u bytes, CFG %08x INTC %08x\n",
				       D(0x10), D(0x20), D(0x08), D(0x0c));
		}
	} else {
		sleep(secs);
	}

	/* stop: PA off first, then the VBC, clear its RAM (vendor: MUST) */
	adi_rmw(0x600, 1u << 13, 0);
	msleep(2);
	adi_rmw(0x600, 1u << 15, 0);
	adi_rmw(0x624, 1u << 4, 0);
	rmw32(&VBC32(0x18), (1u << 15) | (1u << 13), 0);
	rmw32(&VBC32(0xc8), (1u << 0) | (1u << 1), 0);
	if (use_dma) {
		/* VBC requests off (above), pause with ack bit 16, disable */
		D(0x00) = 1;
		for (i = 0; i < 100000 && !(D(0x00) & (1u << 16)); i++)
			;
		D(0x08) &= ~1u;
		D(0x00) = 0;
		D(0x0c) |= 0x1f000000;
		dma[DMA_UID29 / 4] = 0;
		ahb[0] &= ~(1u << 5);
	}
	vbc_fill(0, zero);
	vbc_fill(1, zero);
	printf("stopped\n");
	return 0;
}
