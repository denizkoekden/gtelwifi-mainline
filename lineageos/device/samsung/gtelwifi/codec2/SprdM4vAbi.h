/* SPDX-License-Identifier: MIT
 * ARM32 ABI declarations for the local LOS16 MPEG4/H263 decoder, SHA256
 * e1c06335547cf424ee6387d24498938245e7123762d516fa9795959a2ef5c6d0.
 * Init, Decode, output_one_frame, callbacks and MemInit offsets verified
 * in that binary. The older public header has incompatible 32-bit PTS.
 */
#ifndef GTELWIFI_SPRD_M4V_ABI_H
#define GTELWIFI_SPRD_M4V_ABI_H
#include <stdint.h>
#include <stddef.h>
struct m4v_handle {
    void *decoder, *userdata;
    int (*bind)(void *, void *, int);
    int (*unbind)(void *, void *, int);
    int (*extra)(void *, unsigned);
    int32_t error;
};
struct m4v_buffer {
    uint8_t *cpu;
    uint32_t dma, size;
    int32_t frames;
    uint8_t *internal;
    int32_t internal_size;
};
struct m4v_format {
    int32_t standard, width, height, extra_size;
    void *extra;
    uint32_t extra_dma;
    int32_t yuv;
};
struct m4v_input {
    uint8_t *stream;
    uint32_t dma, length;
    int32_t last, seek;
    int64_t pts;
    int32_t display, error_count;
    uint16_t errors[30][2];
};
struct m4v_output {
    uint8_t *y, *u, *v;
    uint32_t width, height;
    int32_t transposed;
    int64_t pts;
    int32_t effective, errors;
    void *header;
    int32_t picture_type;
};
static_assert(sizeof(struct m4v_handle) == 24, "M4V handle ABI");
static_assert(offsetof(struct m4v_handle, extra) == 16, "M4V memory callback");
static_assert(sizeof(struct m4v_buffer) == 24, "M4V buffer ABI");
static_assert(sizeof(struct m4v_format) == 28, "M4V format ABI");
static_assert(sizeof(struct m4v_input) == 160, "M4V input ABI");
static_assert(offsetof(struct m4v_input, display) == 32, "M4V display flag");
static_assert(sizeof(struct m4v_output) == 48, "M4V output ABI");
static_assert(offsetof(struct m4v_output, pts) == 24, "M4V 64-bit PTS");
static_assert(offsetof(struct m4v_output, header) == 40, "M4V output header");
#endif
