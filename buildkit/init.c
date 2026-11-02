/*
 * gtelwifi bring-up status screen (/bin/fbstatus, started by /init).
 * Test 1 ran this as PID 1; it still works that way.
 *
 * Draws straight into the Spreadtrum bootloader framebuffer via /dev/mem
 * (address from "lcd_base=" on the kernel command line), so it works even
 * if no kernel display driver probed. Shows:
 *   - kernel version, model, RAM, uptime (uptime ticking == timer IRQs ok)
 *   - state of the USB device controller (/sys/class/udc)
 *   - kernel log lines worth a photo (timer, USB, errors), then the tail
 * Refreshes once per second forever.
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/klog.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>

#include "font8x16.h"

#define W	800
#define H	1280
#define FB_SIZE	(W * H * 4)
#define FALLBACK_LCD_BASE 0x9ea44000UL

static volatile uint32_t *fb;
static uint32_t *back;	/* draw off-screen, then copy: no flicker */

#define WHITE	0x00ffffffu
#define GREY	0x00aaaaaau
#define BLACK	0x00000000u

static void px(int x, int y, uint32_t c)
{
	if (x >= 0 && x < W && y >= 0 && y < H)
		back[y * W + x] = c;
}

static void rect(int x0, int y0, int w, int h, uint32_t c)
{
	for (int y = y0; y < y0 + h; y++)
		for (int x = x0; x < x0 + w; x++)
			px(x, y, c);
}

/* Map the few UTF-8 characters we use to CP437 */
static unsigned char cp437(const unsigned char **s)
{
	unsigned char c = **s;

	if (c == 0xc3) {
		unsigned char d = (*s)[1];

		(*s)++;
		switch (d) {
		case 0xa4: return 0x84;	/* ä */
		case 0xb6: return 0x94;	/* ö */
		case 0xbc: return 0x81;	/* ü */
		case 0x9f: return 0xe1;	/* ß */
		case 0x84: return 0x8e;	/* Ä */
		case 0x96: return 0x99;	/* Ö */
		case 0x9c: return 0x9a;	/* Ü */
		default:   return '?';
		}
	}
	return c;
}

/* Draw text, return number of glyphs drawn. Clips at screen edge. */
static int text(int x, int y, int scale, uint32_t c, const char *str)
{
	const unsigned char *s = (const unsigned char *)str;
	int n = 0;

	for (; *s && *s != '\n'; s++, n++) {
		unsigned char ch = cp437(&s);
		const unsigned char *g = &font8x16[ch * 16];

		if (x + 8 * scale > W)
			break;
		for (int r = 0; r < 16; r++)
			for (int b = 0; b < 8; b++)
				if (g[r] & (0x80 >> b))
					rect(x + b * scale, y + r * scale,
					     scale, scale, c);
		x += 8 * scale;
	}
	return n;
}

static int read_file(const char *path, char *buf, size_t len)
{
	int fd = open(path, O_RDONLY);
	ssize_t n;

	if (fd < 0)
		return -1;
	n = read(fd, buf, len - 1);
	close(fd);
	if (n < 0)
		n = 0;
	buf[n] = 0;
	return (int)n;
}

static unsigned long find_lcd_base(const char *cmdline)
{
	const char *p = strstr(cmdline, "lcd_base=");

	if (!p)
		return FALLBACK_LCD_BASE;
	return strtoul(p + 9, NULL, 16);
}

static void meminfo(char *out, size_t len)
{
	char buf[4096];
	char *p;

	snprintf(out, len, "?");
	if (read_file("/proc/meminfo", buf, sizeof(buf)) <= 0)
		return;
	p = strstr(buf, "MemTotal:");
	if (p)
		snprintf(out, len, "%lu MB", strtoul(p + 9, NULL, 10) / 1024);
}

static char klog[256 * 1024];

static void draw_klog(int y0)
{
	int n = klogctl(3 /* READ_ALL */, klog, sizeof(klog) - 1);
	int rows = (H - y0) / 16, count = 0;
	char *lines[512];

	if (n <= 0)
		return;
	klog[n] = 0;
	/* split into lines, dropping the "<6>" / "<12>" priority prefix */
	for (char *p = klog; *p && count < 512; ) {
		char *q = p;

		if (*q == '<') {
			while (*++q >= '0' && *q <= '9')
				;
			q = *q == '>' ? q + 1 : p;
		}
		lines[count++] = q;
		p = strchr(p, '\n');
		if (!p)
			break;
		*p++ = 0;
	}

	/* lines worth a photo first, they would scroll out of the tail */
	static const char *const keys[] = {
		"Booting Linux", "sc8830-timer", "sprd-usb", "dwc2", "udc", "mmc",
		"rror", "fail", "WARNING", "] init: ",
	};
	int y = y0, shown = 0;

	for (int i = 0; i < count && shown < rows / 2; i++) {
		for (size_t k = 0; k < sizeof(keys) / sizeof(keys[0]); k++) {
			if (strstr(lines[i], keys[k])) {
				text(0, y, 1, WHITE, lines[i]);
				y += 16;
				shown++;
				break;
			}
		}
	}
	if (shown)
		y += 8;

	rows = (H - y) / 16;
	int first = count > rows ? count - rows : 0;
	for (int i = first; i < count; i++, y += 16)
		text(0, y, 1, GREY, lines[i]);
}

/* "<udc>: <state> <speed>" for the first USB device controller */
static void usb_state(char *out, size_t len)
{
	char path[320], state[64] = "", speed[64] = "";
	struct dirent *de;
	DIR *d = opendir("/sys/class/udc");

	snprintf(out, len, "USB: kein UDC");
	if (!d)
		return;
	while ((de = readdir(d))) {
		if (de->d_name[0] == '.')
			continue;
		snprintf(path, sizeof(path), "/sys/class/udc/%s/state", de->d_name);
		read_file(path, state, sizeof(state));
		snprintf(path, sizeof(path), "/sys/class/udc/%s/current_speed",
			 de->d_name);
		read_file(path, speed, sizeof(speed));
		state[strcspn(state, "\n")] = 0;
		speed[strcspn(speed, "\n")] = 0;
		snprintf(out, len, "USB %.32s: %s, %s", de->d_name, state, speed);
		break;
	}
	closedir(d);
}

int main(void)
{
	char cmdline[2048] = "", model[128] = "?", mem[32], line[256], up[64];
	struct utsname u;
	unsigned long base;
	int fd;

	mount("proc", "/proc", "proc", 0, NULL);
	mount("sysfs", "/sys", "sysfs", 0, NULL);
	mount("devtmpfs", "/dev", "devtmpfs", 0, NULL);

	read_file("/proc/cmdline", cmdline, sizeof(cmdline));
	read_file("/proc/device-tree/model", model, sizeof(model));
	base = find_lcd_base(cmdline);
	uname(&u);
	meminfo(mem, sizeof(mem));

	fd = open("/dev/mem", O_RDWR | O_SYNC);
	if (fd >= 0)
		fb = mmap(NULL, FB_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED,
			  fd, (off_t)base);
	back = malloc(FB_SIZE);
	if (fd < 0 || fb == MAP_FAILED || !back) {
		printf("init: cannot map framebuffer at %#lx: %s\n", base,
		       strerror(errno));
		for (;;)
			pause();
	}

	for (unsigned long tick = 0;; tick++) {
		int y = 16;

		memset(back, 0, FB_SIZE);
		text(16, y, 4, WHITE, "Linux läuft!");		y += 72;
		snprintf(line, sizeof(line), "%s %s", u.sysname, u.release);
		text(16, y, 2, WHITE, line);			y += 40;
		text(16, y, 2, WHITE, model);			y += 40;
		snprintf(line, sizeof(line), "RAM: %s   lcd_base: %#lx", mem, base);
		text(16, y, 2, WHITE, line);			y += 40;

		read_file("/proc/uptime", up, sizeof(up));
		up[strcspn(up, " ")] = 0;
		snprintf(line, sizeof(line), "Uptime: %s s  (Bild %lu)", up, tick);
		text(16, y, 2, WHITE, line);			y += 48;

		/* liveness bar: moves one step per second */
		rect(16 + (tick % 24) * 32, y, 24, 12, WHITE);	y += 28;

		/* x8r8g8b8 verified in Test 1b; the colour boxes made room for USB */
		usb_state(line, sizeof(line));
		text(16, y, 2, 0x0000ff00u, line);		y += 48;

		text(16, y, 1, WHITE, "Kommandozeile:");	y += 16;
		for (const char *p = cmdline; *p && *p != '\n' && y < 560; y += 16) {
			int n = text(16, y, 1, GREY, p);

			if (n <= 0)
				break;
			p += n;
		}
		y += 8;
		draw_klog(y);

		memcpy((void *)fb, back, FB_SIZE);

		struct timespec req = { 1, 0 }, rem;
		int r;

		while ((r = clock_nanosleep(CLOCK_MONOTONIC, 0, &req, &rem)) == EINTR)
			req = rem;
		if (r)
			fprintf(stderr, "init: clock_nanosleep: %s\n", strerror(r));
	}
}
