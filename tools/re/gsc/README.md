# Dcam_GenScaleCoeff host harness

Runs the vendor's DCAM scaler coefficient generator
(`drivers/media/sprd_dcam/common/gen_scale_coef.c`, GPL-2.0, in the vendor
kernel at `/src/vendor`) for given sizes and prints the H, V-luma and
V-chroma tables plus the taps. The generator stores pointers in `uint32_t`,
so it only runs as 32-bit code: build it for armhf and run it on the tablet.

    docker/run.sh bash -c 'd=/src/vendor/drivers/media/sprd_dcam/common; cd /work/tools/re/gsc && \
      arm-linux-gnueabihf-gcc -static -O1 -w -I. -I$d -o /work/out/gsc-arm main.c $d/gen_scale_coef.c $d/sin_cos.c'
    tools/nurassh 'cat > /tmp/gsc && chmod +x /tmp/gsc' < out/gsc-arm
    tools/nurassh '/tmp/gsc 640 480 1; rm /tmp/gsc'     # in_w in_h yuv420 (output = input)

`main.c` scales 1:1; for other ratios change the output size there. The
1:1 YUV420 tables in `sc7730-dcam.c` (patch 0089) come from this.
