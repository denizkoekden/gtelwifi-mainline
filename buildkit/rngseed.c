/*
 * rngseed <hex>: mix the given bytes into the kernel entropy pool and
 * credit them (RNDADDENTROPY), so the CRNG becomes ready.
 *
 * The SC7730 has no hardware RNG and only a 32 kHz clocksource, which the
 * kernel's jitter entropy rejects as too coarse; without help the CRNG
 * takes minutes and dropbear blocks. tools/tabssh feeds 64 bytes from the
 * Mac's CSPRNG through the ACM shell before it connects.
 */
#include <fcntl.h>
#include <linux/random.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <unistd.h>

int main(int argc, char **argv)
{
	struct {
		struct rand_pool_info info;
		unsigned char buf[512];
	} s;
	const char *h = argc > 1 ? argv[1] : "";
	int n = 0, fd;

	while (h[0] && h[1] && n < (int)sizeof(s.buf)) {
		if (sscanf(h, "%2hhx", &s.buf[n]) != 1)
			break;
		n++;
		h += 2;
	}
	if (n < 32) {
		fprintf(stderr, "rngseed: need at least 32 hex bytes\n");
		return 2;
	}
	s.info.entropy_count = n * 8;
	s.info.buf_size = n;

	fd = open("/dev/random", O_RDWR);
	if (fd < 0 || ioctl(fd, RNDADDENTROPY, &s.info) < 0) {
		perror("rngseed");
		return 1;
	}
	close(fd);
	return 0;
}
