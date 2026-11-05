#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gen_scale_coef.h"
int main(int argc, char **argv)
{
	int w = atoi(argv[1]), h = atoi(argv[2]), yuv420 = atoi(argv[3]);
	static uint32_t h_c[48], v_c[132], vc_c[132];
	static uint8_t tmp[21 << 10];
	uint8_t yt = 0, uvt = 0;
	if (!Dcam_GenScaleCoeff(w, h, w, h, h_c, v_c, vc_c, yuv420, &yt, &uvt, tmp, sizeof(tmp))) { fprintf(stderr, "fail\n"); return 1; }
	printf("y_tap %u uv_tap %u\n", yt, uvt);
	printf("H:"); for (int i = 0; i < 48; i++) printf(" %08x", h_c[i]); printf("\n");
	printf("V:"); for (int i = 0; i < 132; i++) printf(" %08x", v_c[i]); printf("\n");
	printf("VC:"); for (int i = 0; i < 132; i++) printf(" %08x", vc_c[i]); printf("\n");
	return 0;
}
