// SPDX-License-Identifier: GPL-2.0
/*
 * camgrab: camera milestone M2 on the gtelwifi from user space. Frames
 * from the front SR200PC20M (D-PHY C, 1 lane, 192 Mbps at 800x600) or the
 * rear S5K4ECGX (D-PHY A, 2 lanes, 344 Mbps) through CSI-2 and DCAM path 1
 * (scaler bypass, YUV422: Y plane + interleaved UV plane, U first) into the
 * unused part of the vendor framebuffer carve-out.
 *
 * Sequence and registers: vendor-ref/research/camera-m2-report.md, M2
 * steps 1-12. Power-up as camprobe (M1). Sensor tables come from the
 * stock HAL via tools/camtables.py (not distributed).
 *
 *   camgrab front|rear /dev/i2c-N /dev/gpiochipM init.bin mode.bin \
 *           out.nv16 W H [frames]
 *
 * The front tables end by starting the stream, so the sensor is put to
 * sleep before CSI/PHY come up and woken afterwards. The rear has no
 * stream-off; CSI/PHY come up right after power-up instead, while the
 * sensor still idles in LP-11, and its init table starts the stream.
 *
 * Keeps streaming into the same buffer until [frames] path1 frames are
 * done (default 20, about 1 s: lets exposure and white balance settle).
 *
 * Run with the PMIC watchdog armed.
 */

#include <errno.h>
#include <fcntl.h>
#include <linux/gpio.h>
#include <linux/i2c.h>
#include <linux/i2c-dev.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define BUF		0x9ee2c000u	/* framebuffer carve-out, pages 1-2 */
#define BUF_MAX		0x7c8000u
#define BUF_LEN		(W * H * 2)

static unsigned int W, H;
static int rear;

#define ADI_DIR		"/sys/kernel/debug/sprd-adi/"
#define LDO_PD		0x814
#define PD_CAMA		(1u << 4)
#define PD_CAMIO	(1u << 6)
#define PD_CAMMOT	(1u << 7)
#define V_CAMIO		0x81c
#define V_CAMA		0x830

/* MM AHB 0x60d00000 */
#define AHB_EB		0x00
#define AHB_RST		0x04
#define GEN_CKG		0x08
#define CSI2_CTRL	0x0c
/* DCAM 0x60800000 */
#define DCAM_CFG	0x00
#define DCAM_CONTROL	0x04
#define PATH1_CFG	0x10
#define PATH1_SRC	0x14
#define PATH1_DST	0x18
#define PATH1_TRIM_ST	0x1c
#define PATH1_TRIM_SZ	0x20
#define INT_STS		0x3c
#define INT_MASK	0x40
#define INT_CLR		0x44
#define INT_RAW		0x48
#define FRM_ADDR1	0x50
#define FRM_ADDR2	0x54
#define ENDIAN_SEL	0x6c
#define AHBM_STS	0x70
#define CAP_CTRL	0x128
#define CAP_FRM_CTRL	0x12c
#define CAP_START	0x130
#define CAP_END		0x134
#define CAP_DECI	0x138
/* CSI-2 host 0x60c00000 */
#define CSI_VERSION	0x00
#define CSI_N_LANES	0x04
#define CSI_SHUTDOWNZ	0x08
#define CSI_DPHY_RSTZ	0x0c
#define CSI_RESETN	0x10
#define CSI_PHY_STATE	0x14
#define CSI_ERR1	0x20
#define CSI_ERR2	0x24
#define CSI_MASK1	0x28
#define CSI_MASK2	0x2c
#define CSI_TST0	0x30
#define CSI_TST1	0x34

enum { G_RST_R, G_STBY_R, G_RST_F, G_STBY_F, G_DVDD, G_NUM };
static const unsigned int gpio_nr[G_NUM] = { 186, 188, 238, 187, 193 };
static int line_fd = -1, i2c_fd = -1, mem_fd = -1;
static volatile uint32_t *mmahb, *mmclk, *dcam, *csi, *pmu, *aon, *iram;
static volatile uint8_t *buf;

#define R(b, o)		((b)[(o) / 4])

static void msleep(unsigned int ms)
{
	struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };

	nanosleep(&ts, NULL);
}

static void usleep1(void)
{
	struct timespec ts = { 0, 2000 };

	nanosleep(&ts, NULL);
}

static void die(const char *what)
{
	fprintf(stderr, "camgrab: %s: %s\n", what, strerror(errno));
	exit(1);
}

static void *map(uint32_t pa, size_t len)
{
	void *p = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, mem_fd, pa);

	if (p == MAP_FAILED)
		die("mmap");
	return p;
}

static void rmw(volatile uint32_t *b, uint32_t off, uint32_t clr, uint32_t set)
{
	R(b, off) = (R(b, off) & ~clr) | set;
}

/* IRAM0 0xfb0 breadcrumb, survives the PMIC watchdog reset */
static void crumb(uint32_t v)
{
	R(iram, 0xfb0) = 0xca000000 | v;
}

static uint32_t adi_read(uint32_t reg)
{
	char b[32];
	FILE *f = fopen(ADI_DIR "address", "w");

	if (!f || fprintf(f, "0x%x", reg) < 0 || fclose(f))
		die("adi address");
	f = fopen(ADI_DIR "value", "r");
	if (!f || !fgets(b, sizeof(b), f))
		die("adi read");
	fclose(f);
	return strtoul(b, NULL, 0);
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

static void gpio_open(const char *chip)
{
	struct gpio_v2_line_request req = { 0 };
	int fd, i;

	fd = open(chip, O_RDWR);
	if (fd < 0)
		die(chip);
	for (i = 0; i < G_NUM; i++)
		req.offsets[i] = gpio_nr[i];
	req.num_lines = G_NUM;
	req.config.flags = GPIO_V2_LINE_FLAG_OUTPUT;
	strcpy(req.consumer, "camgrab");
	if (ioctl(fd, GPIO_V2_GET_LINE_IOCTL, &req) < 0)
		die("gpio request");
	line_fd = req.fd;
	close(fd);
}

static void gpio_set(int idx, int val)
{
	struct gpio_v2_line_values v = {
		.bits = val ? 1ull << idx : 0,
		.mask = 1ull << idx,
	};

	if (ioctl(line_fd, GPIO_V2_LINE_SET_VALUES_IOCTL, &v) < 0)
		die("gpio set");
}

static int sr_write(uint8_t reg, uint8_t val)
{
	uint8_t b[2] = { reg, val };
	struct i2c_msg m = { .addr = 0x20, .flags = 0, .len = 2, .buf = b };
	struct i2c_rdwr_ioctl_data d = { &m, 1 };

	return ioctl(i2c_fd, I2C_RDWR, &d) < 0 ? -errno : 0;
}

static int s5k_write(uint16_t reg, uint16_t val)
{
	uint8_t b[4] = { reg >> 8, reg, val >> 8, val };
	struct i2c_msg m = { .addr = 0x56, .flags = 0, .len = 4, .buf = b };
	struct i2c_rdwr_ioctl_data d = { &m, 1 };

	return ioctl(i2c_fd, I2C_RDWR, &d) < 0 ? -errno : 0;
}

static int s5k_read(uint16_t reg, uint16_t *val)
{
	uint8_t w[2] = { reg >> 8, reg }, r[2] = { 0 };
	struct i2c_msg m[2] = {
		{ .addr = 0x56, .flags = 0, .len = 2, .buf = w },
		{ .addr = 0x56, .flags = I2C_M_RD, .len = 2, .buf = r },
	};
	struct i2c_rdwr_ioctl_data d = { m, 2 };
	int ret = ioctl(i2c_fd, I2C_RDWR, &d) < 0 ? -errno : 0;

	*val = r[0] << 8 | r[1];
	return ret;
}

static int sr_read(uint8_t reg, uint8_t *val)
{
	struct i2c_msg m[2] = {
		{ .addr = 0x20, .flags = 0, .len = 1, .buf = &reg },
		{ .addr = 0x20, .flags = I2C_M_RD, .len = 1, .buf = val },
	};
	struct i2c_rdwr_ioctl_data d = { m, 2 };

	return ioctl(i2c_fd, I2C_RDWR, &d) < 0 ? -errno : 0;
}

/*
 * HAL table: {u16 reg, u16 val}. Front: ff=NN sleeps NN * 10 ms. Rear:
 * ffff=NNNN sleeps NNNN ms. The marker with itself as value is written.
 */
static void sensor_table(const char *fn)
{
	uint16_t e[2];
	int n = 0, err = 0;
	FILE *f = fopen(fn, "rb");

	if (!f)
		die(fn);
	while (fread(e, 4, 1, f) == 1) {
		if (!rear && e[0] == 0xff && e[1] != 0xff) {
			msleep(e[1] * 10);
			continue;
		}
		if (rear && e[0] == 0xffff && e[1] != 0xffff) {
			msleep(e[1]);
			continue;
		}
		if (rear ? s5k_write(e[0], e[1]) : sr_write(e[0], e[1]))
			err++;
		n++;
	}
	fclose(f);
	printf("%s: %d writes, %d I2C errors\n", fn, n, err);
	if (err)
		exit(1);
}

static void mm_power(void)
{
	int i;

	rmw(pmu, 0x1c, 1u << 25, 0);			/* PD_MM_TOP FORCE_SHUTDOWN */
	for (i = 0; i < 100 && (R(pmu, 0xbc) >> 28); i++)
		msleep(1);
	printf("MM domain: PWR_STATUS0 %08x\n", R(pmu, 0xbc));
	rmw(aon, 0x0, 0, 1u << 25);			/* MM_EB */
	msleep(1);
}

static void mm_clocks(void)
{
	rmw(mmahb, AHB_EB, 0, 1u << 6);			/* MM_CKG_EB */
	rmw(mmahb, GEN_CKG, 0, (1u << 7) | (1u << 8));	/* MM_AXI, MM_MTX_AXI */
	R(mmclk, 0x20) = 3;				/* MM AHB 153.6 MHz */
	rmw(mmahb, AHB_EB, 0, (1u << 0) | (1u << 4));	/* DCAM, CSI */
	rmw(mmahb, GEN_CKG, 0, 0xf);			/* CPHY cfg, CSI, sensor, DCAM AXI */
	rmw(mmclk, 0x2c, 0x3, 2);			/* DCAM 256 MHz, never 3 */
	rmw(mmclk, 0x24, 0x703, 0x101);			/* MCLK 48 MHz / 2 */
	printf("MM: AHB_EB %08x GEN_CKG %08x AHB_CFG %08x DCAM_CLK %08x MCLK %08x\n",
	       R(mmahb, AHB_EB), R(mmahb, GEN_CKG), R(mmclk, 0x20),
	       R(mmclk, 0x2c), R(mmclk, 0x24));
}

static void rails(int on)
{
	if (on) {
		adi_rmw(LDO_PD, PD_CAMMOT, 0);
		msleep(1);
		adi_rmw(V_CAMIO, 0x7f00, 0x68 << 8);	/* 1.8 V */
		adi_rmw(LDO_PD, PD_CAMIO, 0);
		msleep(1);
		adi_rmw(V_CAMA, 0xff, 0xa0);		/* 2.8 V */
		adi_rmw(LDO_PD, PD_CAMA, 0);
		msleep(1);
	} else {
		adi_rmw(LDO_PD, 0, PD_CAMA | PD_CAMIO | PD_CAMMOT);
	}
}

static void front_on(void)
{
	uint8_t id = 0;

	gpio_set(G_STBY_R, 0);
	gpio_set(G_RST_R, 0);
	gpio_set(G_STBY_F, 0);
	gpio_set(G_RST_F, 0);
	rails(1);
	if (!getenv("NO_DVDD_PULSE")) {
		gpio_set(G_DVDD, 1);	/* the vendor pulses it */
		msleep(2);
		gpio_set(G_DVDD, 0);
	}
	msleep(3);
	gpio_set(G_STBY_F, 1);
	msleep(2);
	/* MCLK is running since mm_enable() */
	msleep(30);
	gpio_set(G_RST_F, 1);
	msleep(10);
	if (sr_write(0x03, 0x00) || sr_read(0x04, &id) || id != 0xb4) {
		fprintf(stderr, "camgrab: front sensor id %02x, expect b4\n", id);
		exit(1);
	}
	printf("front SR200PC20M id %02x\n", id);
}

static void rear_on(void)
{
	uint16_t id = 0, rev = 0;

	gpio_set(G_STBY_F, 0);
	gpio_set(G_RST_F, 0);
	gpio_set(G_STBY_R, 0);
	gpio_set(G_RST_R, 0);
	msleep(1);
	rails(1);
	/* MCLK is running since mm_enable() */
	msleep(1);
	gpio_set(G_DVDD, 1);		/* external 1.2 V LDO */
	msleep(1);
	gpio_set(G_STBY_R, 1);
	msleep(1);
	gpio_set(G_RST_R, 1);
	msleep(10);
	if (s5k_write(0xfcfc, 0xd000) || s5k_write(0x002c, 0x7000) ||
	    s5k_write(0x002e, 0x01a4) || s5k_read(0x0f12, &id) ||
	    s5k_write(0x002e, 0x01a6) || s5k_read(0x0f12, &rev) ||
	    id != 0x4ec0) {
		fprintf(stderr, "camgrab: rear sensor id %04x, expect 4ec0\n", id);
		exit(1);
	}
	printf("rear S5K4ECGX id %04x rev %04x\n", id, rev);
}

static void rear_off(void)
{
	gpio_set(G_RST_R, 0);
	msleep(1);
	gpio_set(G_STBY_R, 0);
	gpio_set(G_DVDD, 0);
	rmw(mmahb, GEN_CKG, 1u << 2, 0);	/* MCLK gate */
	rails(0);
}

static void front_off(void)
{
	sr_write(0x03, 0x00);
	sr_write(0x01, 0x31);		/* sleep */
	gpio_set(G_RST_F, 0);
	msleep(1);
	gpio_set(G_STBY_F, 0);
	rmw(mmahb, GEN_CKG, 1u << 2, 0);	/* MCLK gate */
	rails(0);
}

/* Synopsys D-PHY test port */
static void dphy_write(uint8_t code, uint8_t data)
{
	R(csi, CSI_TST1) = 0;			/* testen 0 */
	rmw(csi, CSI_TST0, 0, 2);		/* testclk 1 */
	usleep1();
	R(csi, CSI_TST1) = code;
	R(csi, CSI_TST1) = code | (1u << 16);	/* testen 1 */
	usleep1();
	rmw(csi, CSI_TST0, 2, 0);		/* testclk 0: latch code */
	usleep1();
	R(csi, CSI_TST1) = code;		/* testen 0 */
	usleep1();
	R(csi, CSI_TST1) = data;
	usleep1();
	rmw(csi, CSI_TST0, 0, 2);		/* testclk 1: latch data */
	usleep1();
}

static void csi_init(void)
{
	uint32_t v;

	rmw(mmahb, AHB_RST, 0, 1u << 5);
	usleep1();
	rmw(mmahb, AHB_RST, 1u << 5, 0);
	v = R(csi, CSI_VERSION);
	printf("CSI version %08x\n", v);
	if (v != 0x3130332a) {
		fprintf(stderr, "camgrab: unexpected CSI version\n");
		exit(1);
	}
	if (rear) {
		rmw(aon, 0x24, 1u << 10, 0);		/* CSI0 (PHY A) power-down off */
		rmw(mmahb, CSI2_CTRL, 0x3f, 0x0c);	/* PHY A */
	} else {
		rmw(mmahb, CSI2_CTRL, 0x3f, 0x20);	/* PHY C into the host */
	}

	R(csi, CSI_SHUTDOWNZ) = 0;
	R(csi, CSI_DPHY_RSTZ) = 0;
	R(csi, CSI_TST0) = 1;				/* testclr */
	usleep1();
	R(csi, CSI_TST0) = 0;
	usleep1();
	dphy_write(0x34, 0x14);
	/* hsfreqrange and settle: rear 344 Mbps, front 192 Mbps */
	dphy_write(0x44, rear ? 0x2a : 0x44);
	dphy_write(0x75, rear ? 0x91 : 0x89);
	dphy_write(0x54, 0x14);
	dphy_write(0x64, 0x14);
	dphy_write(0x74, 0x14);
	/* cfg_done */
	R(csi, CSI_TST1) = 0;
	rmw(csi, CSI_TST0, 0, 2);
	usleep1();
	R(csi, CSI_TST1) = 1u << 16;
	rmw(csi, CSI_TST0, 2, 0);
	usleep1();
	R(csi, CSI_TST1) = 0;

	R(csi, CSI_N_LANES) = rear ? 1 : 0;		/* lanes - 1 */
	R(csi, CSI_SHUTDOWNZ) = 1;
	R(csi, CSI_DPHY_RSTZ) = 0;
	usleep1();
	R(csi, CSI_DPHY_RSTZ) = 1;
	R(csi, CSI_RESETN) = 0;
	usleep1();
	R(csi, CSI_RESETN) = 1;
	R(csi, CSI_MASK1) = ~0u;
	R(csi, CSI_MASK2) = ~0u;
	msleep(1);
	printf("CSI: CTRL %08x PHY_STATE %08x (lane0 stop bit4, clk stop bit10) ERR1 %08x ERR2 %08x\n",
	       R(mmahb, CSI2_CTRL), R(csi, CSI_PHY_STATE), R(csi, CSI_ERR1),
	       R(csi, CSI_ERR2));
}

static void dcam_setup(void)
{
	int i;

	for (i = 0; i < 1000 && (R(dcam, AHBM_STS) & 1); i++)
		usleep1();
	rmw(mmahb, AHB_RST, 0, 0x3);			/* DCAM, CCIR */
	usleep1();
	rmw(mmahb, AHB_RST, 0x3, 0);
	R(dcam, INT_CLR) = 0x1fffff;
	R(dcam, INT_MASK) = 0;				/* polled */

	rmw(dcam, DCAM_CFG, 0, 1u << 9);		/* CSI-2 input */
	R(dcam, CAP_CTRL) = (2u << 7) | (1u << 6);	/* UYVY, multi-frame, YUV, 8 bit */
	R(dcam, CAP_FRM_CTRL) = 2 | (1u << 22);		/* skip 2, clear count */
	R(dcam, CAP_START) = 0;
	R(dcam, CAP_END) = (W - 1) | ((H - 1) << 16);
	R(dcam, CAP_DECI) = 0;

	rmw(dcam, DCAM_CFG, 3u << 11, 0);		/* path1 from CAP */
	R(dcam, PATH1_SRC) = W | (H << 16);
	R(dcam, PATH1_TRIM_ST) = 0;
	R(dcam, PATH1_TRIM_SZ) = W | (H << 16);
	R(dcam, PATH1_DST) = W | (H << 16);
	R(dcam, PATH1_CFG) = 1u << 20;			/* YUV422 2-plane, bypass */
	rmw(dcam, ENDIAN_SEL, 0, (1u << 6) | (1u << 8) | (1u << 18) | (1u << 19));
	R(dcam, FRM_ADDR1) = BUF;
	R(dcam, FRM_ADDR2) = BUF + W * H;

	rmw(dcam, AHBM_STS, 0, 1u << 8);
	rmw(dcam, DCAM_CONTROL, 0, (1u << 10) | (1u << 14));
	rmw(dcam, DCAM_CFG, 0, 1u << 1);		/* path1 enable */
	rmw(dcam, DCAM_CONTROL, 0, 1u << 0);		/* cap force copy */
	rmw(dcam, DCAM_CONTROL, 0, 1u << 2);		/* cap enable */
}

static void dcam_stop(void)
{
	int i;

	rmw(dcam, DCAM_CONTROL, 1u << 2, 0);
	rmw(dcam, AHBM_STS, 0, 1u << 4);
	rmw(dcam, DCAM_CFG, 1u << 1, 0);
	for (i = 0; i < 1000 && (R(dcam, AHBM_STS) & (1u << 17)); i++)
		usleep1();
	rmw(mmahb, AHB_RST, 0, 1u << 8);
	usleep1();
	rmw(mmahb, AHB_RST, 1u << 8, 0);
}

int main(int argc, char **argv)
{
	uint32_t raw, last = 0, n;
	int i, done = 0, want;
	FILE *out;

	if (argc != 9 && argc != 10) {
		fprintf(stderr, "usage: camgrab front|rear /dev/i2c-N /dev/gpiochipM init.bin mode.bin out.nv16 W H [frames]\n");
		return 2;
	}
	rear = !strcmp(argv[1], "rear");
	argv++;
	W = atoi(argv[6]);
	H = atoi(argv[7]);
	want = argc == 10 ? atoi(argv[8]) : 20;
	if (!W || !H || W > 2048 || (W & 3) || (H & 1) || BUF_LEN > BUF_MAX) {
		fprintf(stderr, "camgrab: bad size %ux%u\n", W, H);
		return 2;
	}
	mem_fd = open("/dev/mem", O_RDWR | O_SYNC);
	if (mem_fd < 0)
		die("/dev/mem");
	i2c_fd = open(argv[1], O_RDWR);
	if (i2c_fd < 0)
		die(argv[1]);
	gpio_open(argv[2]);

	iram = map(0, 0x1000);
	pmu = map(0x402b0000, 0x1000);
	aon = map(0x402e0000, 0x1000);
	buf = map(BUF, BUF_MAX);
	memset((void *)buf, 0xa5, BUF_LEN);

	crumb(1);
	mm_power();
	mmahb = map(0x60d00000, 0x1000);
	mmclk = map(0x60e00000, 0x1000);
	mm_clocks();
	dcam = map(0x60800000, 0x1000);
	csi = map(0x60c00000, 0x1000);

	crumb(2);
	if (rear) {
		rear_on();
		csi_init();				/* sensor still in LP-11 */
		crumb(3);
		sensor_table(argv[3]);			/* ends with preview on */
		sensor_table(argv[4]);
		crumb(4);
		dcam_setup();
	} else {
		front_on();
		sensor_table(argv[3]);
		sensor_table(argv[4]);
		sr_write(0x03, 0x00);
		sr_write(0x01, 0x31);			/* sleep: lanes to LP-11 */
		msleep(20);
		crumb(3);
		csi_init();
		crumb(4);
		dcam_setup();
		sr_write(0x03, 0x00);
		sr_write(0x01, 0x30);			/* stream on */
	}
	crumb(5);

	for (i = 0; i < 100 * (2 + want / 10) && done < want; i++) {
		msleep(10);
		raw = R(dcam, INT_RAW);
		if (raw != last && !done) {
			n = R(dcam, CAP_FRM_CTRL) >> 16;
			printf("%4d ms: INT_RAW %08x frames %u PHY_STATE %08x ERR1 %08x ERR2 %08x\n",
			       i * 10, raw, n, R(csi, CSI_PHY_STATE),
			       R(csi, CSI_ERR1), R(csi, CSI_ERR2));
			last = raw;
		}
		if (raw & (1u << 6)) {			/* PATH1_DONE */
			done++;
			R(dcam, INT_CLR) = 1u << 6;
		}
		if (raw & ((1u << 7) | (1u << 10) | (1u << 11) | (1u << 14))) {
			printf("error: INT_RAW %08x\n", raw);
			break;				/* overflow / line / frame error */
		}
	}
	printf("%d path1 frames in %d ms, sensor frame count %u, ERR1 %08x\n",
	       done, i * 10, R(dcam, CAP_FRM_CTRL) >> 16, R(csi, CSI_ERR1));
	crumb(6);
	dcam_stop();
	if (rear)
		rear_off();
	else
		front_off();
	crumb(7);

	for (i = 0, n = 0; i < BUF_LEN; i++)
		n += buf[i] == 0xa5;
	printf("%s: %u of %u bytes still 0xa5\n", done ? "PATH1_DONE" : "no frame",
	       n, BUF_LEN);
	out = fopen(argv[5], "wb");
	if (!out || fwrite((void *)buf, 1, BUF_LEN, out) != BUF_LEN || fclose(out))
		die(argv[5]);
	return done ? 0 : 1;
}
