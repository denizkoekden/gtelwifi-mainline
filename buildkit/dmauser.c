// SPDX-License-Identifier: GPL-2.0
/*
 * dmauser: AP DMA (r4p0) bring-up from user space on the gtelwifi, stages
 * 1 and 2 of vendor-ref/research/dma-report.md. No DMA driver may be bound
 * and SPI 50 must not be requested, so INTC enables cannot raise an IRQ.
 * Buffers live in the bootloader framebuffer reservation (no-map, so
 * /dev/mem maps them uncached and no cache maintenance is needed), unused
 * since the DISPC driver flips to CMA buffers.
 *
 *   run 1: 256 bytes on full channel 24, 4-byte width, TRANS mode
 *   run 2: the same with only the TRANS interrupt enabled (raw vs masked)
 *   run 3: 2 MiB, pause right after the start (ack bit 16 or 2?)
 *
 * Run with the PMIC watchdog armed. Breadcrumbs in IRAM0 0xfb0.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define AP_AHB		0x20d00000
#define DMA		0x20100000
#define BUF		0x9f000000
#define BUF_LEN		0x400000
#define CH		0x1600		/* hardware channel 24 (0-based) */

static volatile uint32_t *ahb, *dma, *iram;
static volatile uint8_t *buf;

#define R(off)		dma[(off) / 4]
#define C(off)		dma[(CH + (off)) / 4]

static void die(const char *what)
{
	fprintf(stderr, "dmauser: %s: %s\n", what, strerror(errno));
	exit(1);
}

static void *map(int fd, uint32_t pa, size_t len)
{
	void *p = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, pa);

	if (p == MAP_FAILED)
		die("mmap");
	return p;
}

static void crumb(uint32_t v)
{
	iram[0] = 0x6d000000 | v;	/* IRAM0 0xfb0 */
}

static void dump_ch(const char *tag)
{
	printf("%s: CH PAUSE %08x REQ %08x CFG %08x INTC %08x SRC %08x DES %08x FRG %08x BLK %08x TRSC %08x\n",
	       tag, C(0x00), C(0x04), C(0x08), C(0x0c), C(0x10), C(0x14),
	       C(0x18), C(0x1c), C(0x20));
	printf("%s: INT_RAW %08x INT_MSK_STS %08x REQ_STS %08x EN_STS %08x\n",
	       tag, R(0x10), R(0x14), R(0x18), R(0x1c));
}

/* full channel, TRANS mode, 4-byte width and step, frag/blk 64 */
static void program(uint32_t src, uint32_t dst, uint32_t len, uint32_t intc)
{
	C(0x00) = 0;
	C(0x04) = 0;
	C(0x08) = 0x00001000;		/* priority 1, not enabled */
	C(0x0c) = 0x1f000000 | intc;	/* clear old status, set enables */
	C(0x10) = src;
	C(0x14) = dst;
	C(0x18) = 0xa2000040;		/* 4 B/4 B, TRANS_REQ, frag 64 */
	C(0x1c) = 0x40;
	C(0x20) = len;
	C(0x24) = 0x00040004;
	C(0x28) = 0;
	C(0x2c) = 0;
	C(0x30) = 0;
	C(0x34) = 0;
	C(0x38) = 0;
	C(0x3c) = 0;
}

static int wait_done(uint32_t mask, int max)
{
	int i;

	for (i = 0; i < max; i++) {
		if (C(0x0c) & mask)
			return i;
		usleep(10);
	}
	return -1;
}

static int check(uint32_t len)
{
	uint32_t i, bad = 0, guard = 0;

	for (i = 0; i < len; i++)
		if (buf[0x1000 + i] != buf[i])
			bad++;
	for (i = len; i < len + 0x400; i++)
		if (buf[0x1000 + i] != 0xee)
			guard++;
	printf("  data: %u of %u bytes wrong, %u guard bytes overwritten\n",
	       bad, len, guard);
	return bad || guard;
}

static void fill(uint32_t len)
{
	uint32_t i;

	for (i = 0; i < len; i++)
		buf[i] = (uint8_t)(i * 7 + 3);
	memset((void *)(buf + 0x1000), 0xee, len + 0x400);
}

int main(int argc, char **argv)
{
	int fd, i, n;
	uint32_t v;

	fd = open("/dev/mem", O_RDWR | O_SYNC);
	if (fd < 0)
		die("/dev/mem");
	ahb = map(fd, AP_AHB, 0x4000);
	iram = map(fd, 0, 0x1000);
	iram += 0xfb0 / 4;
	buf = map(fd, BUF, BUF_LEN);

	/* stage 1: gate and reset state before touching the block */
	printf("AHB_EB %08x AHB_RST %08x AUTO_SLEEP %08x MCU_PAUSE %08x\n",
	       ahb[0], ahb[1], ahb[0x10 / 4], ahb[0x3004 / 4]);
	if (ahb[1] & (1u << 8)) {
		printf("DMA in reset, stop\n");
		return 1;
	}
	crumb(1);
	ahb[0] |= 1u << 5;
	dma = map(fd, DMA, 0x4000);
	crumb(2);
	printf("globals:");
	for (i = 0; i <= 0x24; i += 4)
		printf(" %08x", R(i));
	printf("\n");
	dump_ch("ch24 reset");
	printf("ch0 0x20-0x3c:");
	for (i = 0x20; i <= 0x3c; i += 4)
		printf(" %08x", dma[(0x1000 + i) / 4]);
	printf("\nREQ_CID non-zero:");
	for (i = 0; i < 43; i++)
		if ((v = R(0x2000 + 4 * i)))
			printf(" uid%d=%u", i + 1, v);
	printf("\n");

	/* stage 2 run 1 */
	crumb(3);
	fill(0x100);
	program(BUF, BUF + 0x1000, 0x100, 0x1f);
	C(0x08) |= 1;
	C(0x04) = 1;
	n = wait_done(0x1f0000, 10000);
	printf("run 1: done after %d polls\n", n);
	dump_ch("run 1");
	check(0x100);
	C(0x0c) |= 0x1f000000;
	usleep(1000000);
	printf("run 1 +1 s after clear: INTC %08x INT_RAW %08x CFG %08x REQ %08x\n",
	       C(0x0c), R(0x10), C(0x08), C(0x04));
	C(0x08) &= ~1u;

	/* run 2: only the TRANS interrupt enabled */
	crumb(4);
	fill(0x100);
	program(BUF, BUF + 0x1000, 0x100, 0x04);
	C(0x08) |= 1;
	C(0x04) = 1;
	n = wait_done(0x1f0000, 10000);
	printf("run 2: done after %d polls\n", n);
	dump_ch("run 2");
	check(0x100);
	C(0x0c) |= 0x1f000000;
	C(0x08) &= ~1u;

	/* run 3: pause a 2 MiB transfer right after the start */
	crumb(5);
	program(BUF, BUF + 0x200000, 0x200000, 0x04);
	C(0x08) |= 1;
	C(0x04) = 1;
	C(0x00) = 1;
	for (i = 0; i < 100000; i++) {
		v = C(0x00);
		if (v & ((1u << 16) | (1u << 2)))
			break;
	}
	printf("run 3: PAUSE %08x after %d reads (bit 16 = vendor ack, bit 2 = mainline ack), SRC %08x\n",
	       v, i, C(0x10));
	C(0x08) &= ~1u;
	C(0x00) = 0;
	C(0x0c) |= 0x1f000000;
	dump_ch("run 3 stopped");

	crumb(6);
	ahb[0] &= ~(1u << 5);
	crumb(7);
	printf("done, DMA gate off again\n");
	return 0;
}
