// SPDX-License-Identifier: Apache-2.0
/*
 * TWRP installer for LineageOS on the Galaxy Tab E 9.6 (SM-T560, gtelwifi).
 *
 * The TWRP builds for this tablet (omni_gtel3g) have no working shell
 * ("library libc.so not found"), so the usual update-binary script cannot
 * run. This is a static program instead. Called by TWRP as
 *   update-binary <api> <pipe fd> <zip>
 * it writes the images stored (uncompressed) in the zip:
 *
 *   boot.img     -> KERNEL, read back and compared
 *   system.img   -> SYSTEM, Android sparse image (raw, fill, don't care)
 *   wipe         -> if present: userdata gets zeros over its start, which
 *                   Android formats on the next boot ("formattable" in the
 *                   fstab); HIDDEN (/metadata) gets the empty ext4 of
 *                   metadata.img (zeros when the zip has none): first stage
 *                   init does not format it
 *   metadata.img -> optional, sparse empty ext4 for HIDDEN (mkmetadata.py)
 *
 * TWRP keeps /data (and maybe /metadata) mounted while it installs, and the
 * zip may even sit on /data. A file system that is still mounted writes its
 * superblock back when it is unmounted later, so Android found a damaged
 * ext4 instead of a cleared partition and did not format it (testers,
 * 06.11.2026: the first boot only worked after "Format Data"). Before
 * clearing, every mount of the partition is therefore unmounted, or
 * remounted read-only when it is busy (an ro file system writes nothing
 * back); when neither works the partition is left alone and the user is told
 * to use "Format Data".
 *
 * Partitions are found by their PARTNAME in /sys/block/mmcblk0, and their
 * sizes are checked against the tablet's PIT before anything is written.
 * Nothing else (bootloader, PARAM, EFS, RECOVERY, CSC) is ever touched.
 *
 * Build: arm-linux-gnueabihf-gcc -static -O2 -Wall -o update-binary update-binary.c
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <poll.h>
#include <linux/input.h>

static FILE *ui;

static void say(const char *fmt, ...)
{
	char buf[512];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	/* TWRP logs ui_print itself, stderr only without it */
	if (ui) {
		fprintf(ui, "ui_print %s\nui_print\n", buf);
		fflush(ui);
	} else {
		fprintf(stderr, "%s\n", buf);
	}
}

static void progress(double frac)
{
	if (ui) {
		fprintf(ui, "set_progress %.3f\n", frac);
		fflush(ui);
	}
}

static void die(const char *fmt, ...)
{
	char buf[512];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	say("ERROR: %s", buf);
	exit(1);
}

/* --- zip (stored entries only) --- */

struct entry {
	char name[64];
	uint32_t method, size, local;
	off_t data;
};

static struct entry entries[16];
static int nentries;

static uint16_t le16(const uint8_t *p) { return p[0] | p[1] << 8; }
static uint32_t le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

static void pread_all(int fd, void *buf, size_t n, off_t off)
{
	uint8_t *p = buf;

	while (n) {
		ssize_t r = pread(fd, p, n, off);

		if (r <= 0)
			die("read %zu bytes at %lld: %s", n, (long long)off, r ? strerror(errno) : "EOF");
		p += r;
		n -= r;
		off += r;
	}
}

static void read_zip(int fd)
{
	uint8_t eocd[22 + 65535], h[46];
	off_t size = lseek(fd, 0, SEEK_END), cd, at;
	size_t tail = size < (off_t)sizeof(eocd) ? (size_t)size : sizeof(eocd);
	uint16_t count;
	ssize_t i;

	pread_all(fd, eocd, tail, size - tail);
	for (i = tail - 22; i >= 0; i--)
		if (le32(eocd + i) == 0x06054b50)
			break;
	if (i < 0)
		die("not a zip file");
	count = le16(eocd + i + 10);
	cd = le32(eocd + i + 16);

	for (at = cd; count-- && nentries < 16;) {
		struct entry *e = &entries[nentries];
		uint16_t nlen, xlen, clen, lnlen, lxlen;
		uint8_t lh[30];

		pread_all(fd, h, sizeof(h), at);
		if (le32(h) != 0x02014b50)
			die("bad central directory");
		nlen = le16(h + 28);
		xlen = le16(h + 30);
		clen = le16(h + 32);
		e->method = le16(h + 10);
		e->size = le32(h + 24);
		e->local = le32(h + 42);
		memset(e->name, 0, sizeof(e->name));
		pread_all(fd, e->name, nlen < sizeof(e->name) - 1 ? nlen : sizeof(e->name) - 1, at + 46);
		at += 46 + nlen + xlen + clen;

		pread_all(fd, lh, sizeof(lh), e->local);
		if (le32(lh) != 0x04034b50)
			die("bad local header for %s", e->name);
		lnlen = le16(lh + 26);
		lxlen = le16(lh + 28);
		e->data = (off_t)e->local + 30 + lnlen + lxlen;
		nentries++;
	}
}

static struct entry *find(const char *name, int need)
{
	int i;

	for (i = 0; i < nentries; i++)
		if (!strcmp(entries[i].name, name)) {
			if (entries[i].method != 0)
				die("%s is compressed; the zip must store the images", name);
			return &entries[i];
		}
	if (need)
		die("%s missing in the zip", name);
	return NULL;
}

/* --- partitions --- */

static void partition(const char *partname, char *dev, size_t devlen, uint64_t *bytes)
{
	DIR *d = opendir("/sys/block/mmcblk0");
	struct dirent *de;
	char path[300], line[256];

	if (!d)
		die("no /sys/block/mmcblk0");
	while ((de = readdir(d))) {
		FILE *f;
		int match = 0;

		if (strncmp(de->d_name, "mmcblk0p", 8))
			continue;
		snprintf(path, sizeof(path), "/sys/block/mmcblk0/%s/uevent", de->d_name);
		f = fopen(path, "r");
		if (!f)
			continue;
		while (fgets(line, sizeof(line), f)) {
			line[strcspn(line, "\n")] = 0;
			if (!strncmp(line, "PARTNAME=", 9) && !strcmp(line + 9, partname))
				match = 1;
		}
		fclose(f);
		if (!match)
			continue;
		snprintf(path, sizeof(path), "/sys/block/mmcblk0/%s/size", de->d_name);
		f = fopen(path, "r");
		if (!f || fscanf(f, "%llu", (unsigned long long *)bytes) != 1)
			die("no size for %s", partname);
		fclose(f);
		*bytes *= 512;
		snprintf(dev, devlen, "/dev/block/%.32s", de->d_name);
		closedir(d);
		return;
	}
	closedir(d);
	die("partition %s not found", partname);
}

static int open_part(const char *partname, uint64_t want, uint64_t *bytes)
{
	char dev[64];
	int fd;

	partition(partname, dev, sizeof(dev), bytes);
	if (want && *bytes != want)
		die("%s is %llu bytes, expected %llu: not an SM-T560 layout", partname,
		    (unsigned long long)*bytes, (unsigned long long)want);
	fd = open(dev, O_RDWR | O_SYNC);
	if (fd < 0)
		die("open %s (%s): %s", dev, partname, strerror(errno));
	return fd;
}

static void pwrite_all(int fd, const void *buf, size_t n, off_t off)
{
	const uint8_t *p = buf;

	while (n) {
		ssize_t r = pwrite(fd, p, n, off);

		if (r <= 0)
			die("write at %lld: %s", (long long)off, strerror(errno));
		p += r;
		n -= r;
		off += r;
	}
}

#define BUF (4 << 20)
static uint8_t buf[BUF], buf2[BUF];

static void write_boot(int zip, struct entry *e)
{
	uint64_t psize, done;
	int fd = open_part("KERNEL", 16ULL << 20, &psize);

	if (e->size > psize)
		die("boot.img (%u bytes) larger than KERNEL", e->size);
	say("Writing boot.img to KERNEL (%u KiB)", e->size >> 10);
	for (done = 0; done < e->size; done += BUF) {
		size_t n = e->size - done < BUF ? e->size - done : BUF;

		pread_all(zip, buf, n, e->data + done);
		pwrite_all(fd, buf, n, done);
	}
	fsync(fd);
	for (done = 0; done < e->size; done += BUF) {
		size_t n = e->size - done < BUF ? e->size - done : BUF;

		pread_all(zip, buf, n, e->data + done);
		pread_all(fd, buf2, n, done);
		if (memcmp(buf, buf2, n))
			die("KERNEL read-back differs at %llu", (unsigned long long)done);
	}
	close(fd);
	say("KERNEL written and verified");
}

/*
 * Write an Android sparse image from the zip to an open partition. With
 * zero_dc the "don't care" chunks are written as zeros (an image that
 * replaces what was on the partition completely, as the empty /metadata
 * file system does); without it they are skipped (system.img).
 */
static void write_sparse_fd(int zip, struct entry *e, int fd, uint64_t psize, const char *what,
			    int zero_dc, double p0, double p1)
{
	uint8_t h[28], ch[12];
	uint64_t out = 0;
	uint32_t blk, total_blks, chunks, i;
	off_t at = e->data;

	pread_all(zip, h, sizeof(h), at);
	if (le32(h) != 0xed26ff3a || le16(h + 4) != 1)
		die("%s is not an Android sparse image", e->name);
	blk = le32(h + 12);
	total_blks = le32(h + 16);
	chunks = le32(h + 20);
	if ((uint64_t)total_blks * blk > psize)
		die("%s (%llu bytes) larger than %s", e->name, (unsigned long long)total_blks * blk, what);
	at += le16(h + 8);
	say("Writing %s to %s (%u MiB)", e->name, what, (unsigned)((uint64_t)total_blks * blk >> 20));

	for (i = 0; i < chunks; i++) {
		uint16_t type;
		uint32_t nblk, total;
		uint64_t len;

		pread_all(zip, ch, sizeof(ch), at);
		type = le16(ch);
		nblk = le32(ch + 4);
		total = le32(ch + 8);
		len = (uint64_t)nblk * blk;
		at += le16(h + 10);

		switch (type) {
		case 0xcac1: {	/* raw */
			uint64_t done;

			for (done = 0; done < len; done += BUF) {
				size_t n = len - done < BUF ? len - done : BUF;

				pread_all(zip, buf, n, at + done);
				pwrite_all(fd, buf, n, out + done);
			}
			break;
		}
		case 0xcac2: {	/* fill */
			uint8_t v[4];
			uint64_t done;
			size_t k;

			pread_all(zip, v, 4, at);
			for (k = 0; k < BUF; k += 4)
				memcpy(buf + k, v, 4);
			for (done = 0; done < len; done += BUF)
				pwrite_all(fd, buf, len - done < BUF ? len - done : BUF, out + done);
			break;
		}
		case 0xcac3:	/* don't care */
			if (zero_dc) {
				uint64_t done;

				memset(buf, 0, BUF);
				for (done = 0; done < len; done += BUF)
					pwrite_all(fd, buf, len - done < BUF ? len - done : BUF, out + done);
			}
			break;
		case 0xcac4:	/* crc32 */
			break;
		default:
			die("unknown sparse chunk type %#x", type);
		}
		if (type != 0xcac4)
			out += len;
		at += total - le16(h + 10);
		progress(p0 + (p1 - p0) * out / ((uint64_t)total_blks * blk));
	}
	fsync(fd);
	say("%s written (%u chunks)", what, chunks);
}

static void write_sparse(int zip, struct entry *e, double p0, double p1)
{
	uint64_t psize;
	int fd = open_part("SYSTEM", 3072000ULL * 512, &psize);

	write_sparse_fd(zip, e, fd, psize, "SYSTEM", 0, p0, p1);
	close(fd);
}

/*
 * Unmount every mount of the partition (compared by device number, so that
 * /dev/block/mmcblk0pN, by-name links and the vendor kernel's
 * /dev/block/platform/sprd-sdhci.3/by-name paths all match), or remount it
 * read-only when it is busy. Returns 1 when nothing can write to it anymore.
 */
static int release_part(const char *partname)
{
	char dev[64], line[1024], mp[32][256];
	struct stat pst, st;
	uint64_t size;
	int n = 0, ok = 1;
	FILE *m;

	partition(partname, dev, sizeof(dev), &size);
	if (stat(dev, &pst) < 0)
		die("stat %s: %s", dev, strerror(errno));
	m = fopen("/proc/mounts", "r");
	if (!m)
		die("/proc/mounts: %s", strerror(errno));
	while (n < 32 && fgets(line, sizeof(line), m)) {
		char src[256], dst[256];

		if (sscanf(line, "%255s %255s", src, dst) != 2)
			continue;
		if (stat(src, &st) == 0 && S_ISBLK(st.st_mode) && st.st_rdev == pst.st_rdev)
			snprintf(mp[n++], sizeof(mp[0]), "%s", dst);
	}
	fclose(m);
	/* the last mount first: binds on top of the first one */
	while (n-- > 0) {
		if (umount2(mp[n], 0) == 0) {
			say("%s: unmounted %s", partname, mp[n]);
		} else if (errno == EINVAL) {
			/* already gone with an earlier one */
		} else if (mount(NULL, mp[n], NULL, MS_REMOUNT | MS_RDONLY, NULL) == 0) {
			say("%s: %s is in use, remounted read-only", partname, mp[n]);
		} else {
			say("%s: %s is in use and cannot be made read-only: %s", partname, mp[n],
			    strerror(errno));
			ok = 0;
		}
	}
	return ok;
}

static int need_format;

static void zero_start(const char *partname)
{
	uint64_t psize;
	int fd = open_part(partname, 0, &psize);

	memset(buf, 0, BUF);
#ifndef INSTALLER_DRY_RUN
	pwrite_all(fd, buf, BUF, 0);
	fsync(fd);
#endif
	close(fd);
	say("%s cleared, Android formats it on the first boot", partname);
}

/*
 * /metadata: first stage init does not format "formattable" partitions
 * (lineageos/scripts/flash-system.sh), and aconfigd needs /metadata before
 * anything else can format it. So HIDDEN gets a ready, empty ext4 instead of
 * zeros (metadata.img, lineageos/installer/mkmetadata.py); old zips without
 * it keep the zeroing.
 */
static void write_metadata(int zip, struct entry *e)
{
	uint64_t psize;
	int fd = open_part("HIDDEN", 0, &psize);

#ifndef INSTALLER_DRY_RUN
	write_sparse_fd(zip, e, fd, psize, "HIDDEN", 1, 0.95, 0.99);
#else
	(void)zip;
	(void)e;
#endif
	close(fd);
}

/* Read the start of a partition back, past nothing but the block cache that the
 * O_SYNC writes went through. Returns 1 when it is what the installer wrote. */
static int head_ok(const char *partname, int want_ext4)
{
	uint64_t psize;
	uint8_t head[4096];
	size_t i;
	int fd = open_part(partname, 0, &psize), ok = 1;

	pread_all(fd, head, sizeof(head), 0);
	close(fd);
	if (want_ext4)
		return le16(head + 1024 + 56) == 0xef53;
	for (i = 0; i < sizeof(head); i++)
		ok &= head[i] == 0;
	return ok;
}

static void wipe(const char *partname, struct entry *prepared, int zip)
{
	if (!release_part(partname)) {
		say("%s NOT cleared", partname);
		need_format = 1;
		return;
	}
	if (prepared)
		write_metadata(zip, prepared);
	else
		zero_start(partname);
}

/* After the final sync: did anything bring the old file system back? */
static void check_wiped(const char *partname, int want_ext4)
{
#ifdef INSTALLER_DRY_RUN
	(void)partname;
	(void)want_ext4;
#else
	if (!head_ok(partname, want_ext4)) {
		say("%s: the start of the partition changed after clearing it", partname);
		need_format = 1;
	}
	/* a mount that is writable again would write its superblock back later */
	if (!release_part(partname))
		need_format = 1;
#endif
}

/*
 * Encryption choice (install package only, /data is formatted anyway):
 * Volume up = /data with file-based encryption (default, also after 30 s),
 * Volume down = without. The choice is a marker on the fresh /metadata,
 * /metadata/gtelwifi/noencrypt, which vendor/bin/gtelwifi-crypt.sh turns into
 * fstab.gtelwifi.noencrypt before mount_all. Update packages keep the marker.
 */
static int ask_encryption(void)
{
	struct pollfd pfd[16];
	int n = 0, i, left = 30000;
	char path[64];

	for (i = 0; i < 16; i++) {
		snprintf(path, sizeof(path), "/dev/input/event%d", i);
		pfd[n].fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
		if (pfd[n].fd >= 0)
			pfd[n++].events = POLLIN;
	}
	say(" ");
	say("Encrypt /data (file-based encryption)?");
	say("  Volume UP   = yes, encrypted (default)");
	say("  Volume DOWN = no, not encrypted (TWRP can read /data)");
	say("  no key within 30 s = encrypted");
	if (!n) {
		say("no input devices, keeping encryption");
		return 1;
	}
	while (left > 0) {
		int r = poll(pfd, n, 500);

		left -= 500;
		if (r <= 0)
			continue;
		for (i = 0; i < n; i++) {
			struct input_event ev;

			if (!(pfd[i].revents & POLLIN))
				continue;
			while (read(pfd[i].fd, &ev, sizeof(ev)) == sizeof(ev)) {
				if (ev.type != EV_KEY || ev.value != 1)
					continue;
				if (ev.code == KEY_VOLUMEUP || ev.code == KEY_VOLUMEDOWN) {
					int enc = ev.code == KEY_VOLUMEUP;

					for (i = 0; i < n; i++)
						close(pfd[i].fd);
					say(enc ? "-> /data encrypted" : "-> /data NOT encrypted");
					return enc;
				}
			}
		}
	}
	for (i = 0; i < n; i++)
		close(pfd[i].fd);
	say("-> no key pressed: /data encrypted");
	return 1;
}

/* write /metadata/gtelwifi/noencrypt on the freshly written HIDDEN ext4 */
static void mark_noencrypt(void)
{
	char dev[64];
	uint64_t s;
	int fd;

	partition("HIDDEN", dev, sizeof(dev), &s);
	mkdir("/tmp/gtelmeta", 0700);
	if (mount(dev, "/tmp/gtelmeta", "ext4", MS_NOATIME, "") < 0) {
		say("could not mount the new /metadata (%s): encryption stays on", strerror(errno));
		return;
	}
	mkdir("/tmp/gtelmeta/gtelwifi", 0700);
	fd = open("/tmp/gtelmeta/gtelwifi/noencrypt", O_WRONLY | O_CREAT | O_TRUNC, 0600);
	if (fd >= 0) {
		if (write(fd, "1\n", 2) != 2)
			say("writing the marker failed: %s", strerror(errno));
		fsync(fd);
		close(fd);
	} else {
		say("could not create the marker: %s", strerror(errno));
	}
	sync();
	if (umount("/tmp/gtelmeta") < 0)
		say("umount /tmp/gtelmeta: %s", strerror(errno));
	rmdir("/tmp/gtelmeta");
}

int main(int argc, char **argv)
{
	struct entry *boot, *system;
	int zip;

	if (argc >= 3)
		ui = fdopen(atoi(argv[2]), "w");
	if (argc < 4)
		die("usage: update-binary <api> <fd> <zip>");

	say("LineageOS for the Galaxy Tab E 9.6 (SM-T560)");
	zip = open(argv[3], O_RDONLY);
	if (zip < 0)
		die("open %s: %s", argv[3], strerror(errno));
	read_zip(zip);
	boot = find("boot.img", 1);
	system = find("system.img", 1);

	/* check every partition before the first write */
	{
		uint64_t s;
		char dev[64];

		partition("KERNEL", dev, sizeof(dev), &s);
		partition("SYSTEM", dev, sizeof(dev), &s);
		partition("HIDDEN", dev, sizeof(dev), &s);
		partition("userdata", dev, sizeof(dev), &s);
	}

	progress(0.02);
#ifdef INSTALLER_DRY_RUN
	say("DRY RUN: nothing is written");
	(void)boot;
	(void)system;
#else
	write_boot(zip, boot);
	progress(0.05);
	write_sparse(zip, system, 0.05, 0.95);
#endif
	if (find("wipe", 0)) {
		struct entry *meta = find("metadata.img", 0);
		int encrypt = meta ? ask_encryption() : 1;

		/* HIDDEN first, it reads metadata.img from the zip; the zip is
		 * closed before userdata is released, it may lie on it */
		wipe("HIDDEN", meta, zip);
		close(zip);
		wipe("userdata", NULL, -1);
		sync();
		if (!need_format) {
			check_wiped("userdata", 0);
			check_wiped("HIDDEN", meta != NULL);
		}
#ifndef INSTALLER_DRY_RUN
		if (!encrypt && meta && !need_format)
			mark_noencrypt();
#else
		(void)encrypt;
#endif
	} else {
		close(zip);
		say("Data kept (update package)");
	}
	sync();
	progress(1.0);
	if (need_format) {
		say("IMPORTANT: /data or /metadata could not be cleared while it is in use.");
		say("Before rebooting: Wipe -> Format Data -> type yes.");
		return 0;
	}
	say("Done. The first boot takes 2-3 minutes.");
	return 0;
}
