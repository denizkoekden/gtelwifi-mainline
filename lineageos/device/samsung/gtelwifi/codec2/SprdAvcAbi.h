/* SPDX-License-Identifier: MIT
 * ARM32 ABI declarations for the dynamically loaded SCX35 AVC library.
 * Field offsets checked against the local LOS16 library and the public
 * SPRDAVCDecoder wrapper. No decoder implementation is included.
 */
#ifndef GTELWIFI_SPRD_AVC_ABI_H
#define GTELWIFI_SPRD_AVC_ABI_H
#include <stdint.h>
#include <stddef.h>
struct avc_handle {
    void *decoder;
    void *userdata;
    int (*bind)(void *, void *);
    int (*unbind)(void *, void *);
    int (*extra)(void *, unsigned int);
    /* LOS16 extension verified in MbinfoMemAllocWrapper and init_picture. */
    int (*mbinfo)(void *, unsigned int, uint32_t *);
};
struct avc_buffer {
    uint8_t *cpu;
    uint32_t dma, size;
    int32_t frames;
    uint8_t *internal;
    int32_t internal_size;
};
struct avc_format {
    int32_t standard, width, height, extra_size;
    void *extra;
    uint32_t extra_dma;
    int32_t yuv;
};
struct avc_input {
    uint8_t *stream;
    uint32_t dma, length;
    int32_t last, seek_idr;
    int64_t pts;
    int32_t display, error_count;
    uint16_t errors[30][2];
};
struct avc_output {
    uint8_t *y, *u, *v;
    uint32_t width, height;
    int32_t transposed;
    int64_t pts;
    int32_t effective, errors;
    void *header;
    int32_t new_buffer, picture_id;
    uint8_t flags[2];
};
struct avc_info {
    uint32_t profile, width, height, range, matrix, par_width, par_height;
    uint32_t cropping, crop_left, crop_width, crop_top, crop_height;
    uint32_t references, b_frames;
};
static_assert(sizeof(void *) == 4, "The vendor ABI is ARM32 only");
static_assert(sizeof(struct avc_handle) == 24, "handle ABI");
static_assert(offsetof(struct avc_handle, mbinfo) == 20, "LOS16 mbinfo callback ABI");
static_assert(sizeof(struct avc_buffer) == 24, "buffer ABI");
static_assert(sizeof(struct avc_format) == 28, "format ABI");
static_assert(sizeof(struct avc_input) == 160, "input ABI");
static_assert(sizeof(struct avc_output) == 56, "output ABI");
static_assert(offsetof(struct avc_input, pts) == 24, "LOS16 input timestamp ABI");
static_assert(offsetof(struct avc_input, display) == 32, "LOS16 display flag ABI");
static_assert(offsetof(struct avc_output, effective) == 32, "LOS16 output flag ABI");
static_assert(offsetof(struct avc_output, header) == 40, "LOS16 output header ABI");
static_assert(sizeof(struct avc_info) == 56, "info ABI");
#endif
