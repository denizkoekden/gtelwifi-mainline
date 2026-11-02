// SPDX-License-Identifier: GPL-2.0
/*
 * camprobe: camera milestone M1 on the gtelwifi from user space. Powers
 * the rear S5K4ECGX and the front SR200PC20M one after the other with the
 * vendor sequences (sensor_power_gtel3g.c), reads their chip IDs over I2C0
 * and powers everything down again.
 *
 * PMIC LDOs through debugfs sprd-adi (read-modify-write, the registers
 * are shared with the USB LDO and GPS), GPIOs through the GPIO character
 * device (held while running), MM power domain and sensor clock through
 * /dev/mem. Usage: camprobe /dev/i2c-N /dev/gpiochipM
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

#define ADI_DIR		"/sys/kernel/debug/sprd-adi/"
#define LDO_PD		0x814		/* 1 = off */
#define PD_CAMA		(1u << 4)
#define PD_CAMIO	(1u << 6)
#define PD_CAMMOT	(1u << 7)
#define V_CAMIO		0x81c		/* [14:8], 1.15 V + n * 6.25 mV */
#define V_CAMA		0x830		/* [7:0], 1.2 V + n * 10 mV */
#define V_CAMMOT	0x834		/* [7:0] */

enum { G_RST_R, G_STBY_R, G_RST_F, G_STBY_F, G_DVDD, G_NUM };
static const unsigned int gpio_nr[G_NUM] = { 186, 188, 238, 187, 193 };
static int line_fd = -1;
static int i2c_fd = -1;

static void msleep(unsigned int ms)
{
	struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };

	nanosleep(&ts, NULL);
}

static void die(const char *what)
{
	fprintf(stderr, "camprobe: %s: %s\n", what, strerror(errno));
	exit(1);
}

/* PMIC registers through the kernel's ADI lock */
static uint32_t adi_read(uint32_t reg)
{
	char buf[32];
	FILE *f;

	f = fopen(ADI_DIR "address", "w");
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
	FILE *f;

	f = fopen(ADI_DIR "address", "w");
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

/* SoC registers */
static void mem_rmw(uint32_t addr, uint32_t clr, uint32_t set)
{
	long pg = sysconf(_SC_PAGESIZE);
	volatile uint32_t *p;
	void *map;
	int fd;

	fd = open("/dev/mem", O_RDWR | O_SYNC);
	if (fd < 0)
		die("/dev/mem");
	map = mmap(NULL, pg, PROT_READ | PROT_WRITE, MAP_SHARED, fd,
		   addr & ~(pg - 1));
	if (map == MAP_FAILED)
		die("mmap");
	p = (volatile uint32_t *)((char *)map + (addr & (pg - 1)));
	*p = (*p & ~clr) | set;
	munmap(map, pg);
	close(fd);
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
	req.config.flags = GPIO_V2_LINE_FLAG_OUTPUT;	/* all low */
	strcpy(req.consumer, "camprobe");
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

static void mclk(int on)
{
	if (on) {
		/* clk_48m / 2, gate SENSOR_CKG_EN */
		mem_rmw(0x60e00024, 0x703, 0x101);
		mem_rmw(0x60d00008, 0, 1u << 2);
	} else {
		mem_rmw(0x60d00008, 1u << 2, 0);
	}
}

static int i2c_xfer(uint16_t addr, uint8_t *w, int wlen, uint8_t *r, int rlen)
{
	struct i2c_msg msgs[2] = {
		{ .addr = addr, .flags = 0, .len = wlen, .buf = w },
		{ .addr = addr, .flags = I2C_M_RD, .len = rlen, .buf = r },
	};
	struct i2c_rdwr_ioctl_data data = { msgs, rlen ? 2 : 1 };

	return ioctl(i2c_fd, I2C_RDWR, &data) < 0 ? -errno : 0;
}

static int s5k_write(uint16_t reg, uint16_t val)
{
	uint8_t b[4] = { reg >> 8, reg, val >> 8, val };

	return i2c_xfer(0x56, b, 4, NULL, 0);
}

static int s5k_read(uint16_t reg, uint16_t *val)
{
	uint8_t w[2] = { reg >> 8, reg }, r[2];
	int ret = i2c_xfer(0x56, w, 2, r, 2);

	*val = r[0] << 8 | r[1];
	return ret;
}

static void rails(int on)
{
	if (on) {
		adi_rmw(LDO_PD, PD_CAMMOT, 0);			/* CAMMOT */
		msleep(1);
		adi_rmw(V_CAMIO, 0x7f00, 0x68 << 8);		/* 1.8 V */
		adi_rmw(LDO_PD, PD_CAMIO, 0);
		msleep(1);
		adi_rmw(V_CAMA, 0xff, 0xa0);			/* 2.8 V */
		adi_rmw(LDO_PD, PD_CAMA, 0);
		msleep(1);
	} else {
		adi_rmw(LDO_PD, 0, PD_CAMA | PD_CAMIO | PD_CAMMOT);
	}
}

static void rear(void)
{
	uint16_t id = 0, rev = 0;
	int ret;

	gpio_set(G_STBY_R, 0);
	gpio_set(G_RST_R, 0);
	msleep(1);
	rails(1);
	mclk(1);
	msleep(1);
	gpio_set(G_DVDD, 1);		/* external 1.2 V LDO */
	msleep(1);
	gpio_set(G_STBY_R, 1);
	msleep(1);
	gpio_set(G_RST_R, 1);
	msleep(10);

	ret = s5k_write(0xfcfc, 0xd000);
	if (!ret)
		ret = s5k_write(0x002c, 0x7000);
	if (!ret)
		ret = s5k_write(0x002e, 0x01a4);
	if (!ret)
		ret = s5k_read(0x0f12, &id);
	if (!ret)
		ret = s5k_write(0x002e, 0x01a6);
	if (!ret)
		ret = s5k_read(0x0f12, &rev);
	if (ret)
		printf("rear  S5K4ECGX @0x56: I2C error %d (%s)\n", ret, strerror(-ret));
	else
		printf("rear  S5K4ECGX @0x56: id %04x rev %04x (expect 4ec0 0011)\n",
		       id, rev);

	gpio_set(G_RST_R, 0);
	msleep(1);
	gpio_set(G_STBY_R, 0);
	gpio_set(G_DVDD, 0);
	mclk(0);
}

static void front(void)
{
	uint8_t w[2] = { 0x03, 0x00 }, reg = 0x04, id = 0;
	int ret;

	gpio_set(G_STBY_F, 0);
	gpio_set(G_RST_F, 0);
	/* rails are still on from the rear sensor */
	gpio_set(G_DVDD, 1);		/* the vendor pulses it */
	msleep(2);
	gpio_set(G_DVDD, 0);
	msleep(3);
	gpio_set(G_STBY_F, 1);
	msleep(2);
	mclk(1);
	msleep(30);
	gpio_set(G_RST_F, 1);
	msleep(10);

	ret = i2c_xfer(0x20, w, 2, NULL, 0);
	if (!ret)
		ret = i2c_xfer(0x20, &reg, 1, &id, 1);
	if (ret)
		printf("front SR200PC20M @0x20: I2C error %d (%s)\n", ret, strerror(-ret));
	else
		printf("front SR200PC20M @0x20: id %02x (expect b4)\n", id);

	gpio_set(G_RST_F, 0);
	msleep(1);
	gpio_set(G_STBY_F, 0);
	mclk(0);
}

int main(int argc, char **argv)
{
	if (argc != 3) {
		fprintf(stderr, "usage: camprobe /dev/i2c-N /dev/gpiochipM\n");
		return 2;
	}
	i2c_fd = open(argv[1], O_RDWR);
	if (i2c_fd < 0)
		die(argv[1]);
	gpio_open(argv[2]);

	/* MM power domain on, then its AHB clocks */
	mem_rmw(0x402b001c, 1u << 25, 0);
	msleep(10);
	mem_rmw(0x402e0000, 0, 1u << 25);
	msleep(10);
	mem_rmw(0x60d00000, 0, 0x11);
	mem_rmw(0x60d00008, 0, 0x0b);

	printf("LDO_PD %04x, CAMIO %04x, CAMA %04x before\n",
	       adi_read(LDO_PD), adi_read(V_CAMIO), adi_read(V_CAMA));
	rear();
	front();
	rails(0);
	printf("LDO_PD %04x after\n", adi_read(LDO_PD));
	return 0;
}
