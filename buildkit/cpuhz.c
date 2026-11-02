/* cpuhz: time a 1-cycle-per-iteration loop (subs+bne on Cortex-A7) */
#include <stdio.h>
#include <time.h>

int main(void)
{
	struct timespec a, b;
	unsigned int n = 500000000;

	clock_gettime(CLOCK_MONOTONIC, &a);
	asm volatile("1: subs %0, %0, #1\n\tbne 1b" : "+r"(n) : : "cc");
	clock_gettime(CLOCK_MONOTONIC, &b);
	double t = (b.tv_sec - a.tv_sec) + (b.tv_nsec - a.tv_nsec) / 1e9;
	printf("%.3f s for 500M iterations -> ~%.0f MHz\n", t, 500 / t);
	return 0;
}
