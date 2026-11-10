/* SPDX-License-Identifier: MIT
 * ARM32 SCX35 VP8 ABI: verified with the local LOS16 library, SHA256
 * 164d55cc313ec1630f56d16942f2f8558b63bda0c0fb925af0d7327a56b9e668.
 * Init/Decode/GetBufferDimensions/SetCurRecPic and callback offsets checked.
 * ABI declarations only; no vendor decoder implementation.
 */
#ifndef GTELWIFI_SPRD_VP8_ABI_H
#define GTELWIFI_SPRD_VP8_ABI_H
#include <stdint.h>
#include <stddef.h>
#ifdef __cplusplus
#define VP8_ABI_ASSERT static_assert
#else
#define VP8_ABI_ASSERT _Static_assert
#endif
struct vp8_handle {
    void *decoder, *userdata;
    int (*bind)(void *, void *, int);
    int (*unbind)(void *, void *, int);
};
struct vp8_buffer {
    uint8_t *cpu;
    uint32_t dma, size;
    int32_t frames;
    uint8_t *internal;
    int32_t internal_size;
};
struct vp8_format {
    int32_t standard, width, height, extra_size;
    void *extra;
    uint32_t extra_dma;
    int32_t yuv;
};
struct vp8_input {
    uint8_t *stream;
    uint32_t dma, length;
    int32_t last, seek, pts, display, error_count;
    uint16_t errors[30][2];
};
struct vp8_output {
    uint8_t *y, *u, *v;
    uint32_t width, height;
    int32_t transposed, pts, effective, errors;
    void *header;
    int32_t picture_type;
};
VP8_ABI_ASSERT(sizeof(struct vp8_handle) == 16, "VP8 handle ABI");
VP8_ABI_ASSERT(sizeof(struct vp8_buffer) == 24, "VP8 buffer ABI");
VP8_ABI_ASSERT(sizeof(struct vp8_format) == 28, "VP8 format ABI");
VP8_ABI_ASSERT(sizeof(struct vp8_input) == 152, "VP8 input ABI, 32-bit PTS");
VP8_ABI_ASSERT(sizeof(struct vp8_output) == 44, "VP8 output ABI");
VP8_ABI_ASSERT(offsetof(struct vp8_output, effective) == 28, "VP8 output flag");
VP8_ABI_ASSERT(offsetof(struct vp8_output, header) == 36, "VP8 output header");

#undef VP8_ABI_ASSERT
#endif
