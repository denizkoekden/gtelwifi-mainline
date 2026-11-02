// SPDX-License-Identifier: GPL-2.0
/*
 * membw: rough memory bandwidth of the gtelwifi (memcpy, memset, read
 * sum) on buffers much larger than the 512 KiB L2, single thread.
 *
 *   membw [MiB]
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static double now(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

int main(int argc, char **argv)
{
	size_t mib = argc > 1 ? atoi(argv[1]) : 32, n = mib << 20, i;
	uint32_t *a = malloc(n), *b = malloc(n), sum = 0;
	double t;
	int r, rounds = 8;

	if (!a || !b)
		return 1;
	memset(a, 1, n);
	memset(b, 2, n);

	t = now();
	for (r = 0; r < rounds; r++)
		memcpy(b, a, n);
	t = now() - t;
	printf("memcpy %6.0f MB/s (read + write %6.0f MB/s)\n",
	       rounds * n / t / 1e6, 2 * rounds * n / t / 1e6);

	t = now();
	for (r = 0; r < rounds; r++)
		memset(b, r, n);
	t = now() - t;
	printf("memset %6.0f MB/s\n", rounds * n / t / 1e6);

	t = now();
	for (r = 0; r < rounds; r++)
		for (i = 0; i < n / 4; i += 8)
			sum += a[i] + a[i + 1] + a[i + 2] + a[i + 3] +
			       a[i + 4] + a[i + 5] + a[i + 6] + a[i + 7];
	t = now() - t;
	printf("read   %6.0f MB/s (%u)\n", rounds * n / t / 1e6, sum & 1);
	return 0;
}
