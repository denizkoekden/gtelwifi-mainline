/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
#ifndef _UAPI_LINUX_SPRD_VSP_H
#define _UAPI_LINUX_SPRD_VSP_H

#include <linux/ioctl.h>
#include <linux/types.h>

/* SCX35 userspace ABI, including the original ACQUAIRE spelling. */
#define SPRD_VSP_MAP_SIZE 0xa000
#define VSP_CONFIG_FREQ _IOW('m', 1, unsigned int)
#define VSP_GET_FREQ _IOR('m', 2, unsigned int)
#define VSP_ENABLE _IO('m', 3)
#define VSP_DISABLE _IO('m', 4)
#define VSP_ACQUAIRE _IO('m', 5)
#define VSP_RELEASE _IO('m', 6)
/* Despite _IO, COMPLETE, HW_INFO and CAPABILITY take a __u32 pointer. */
#define VSP_COMPLETE _IO('m', 7)
#define VSP_RESET _IO('m', 8)
#define VSP_HW_INFO _IO('m', 9)
#define VSP_CAPABILITY _IO('m', 10)

/* Local bring-up extension: coherent CMA buffers, owned by this open file.
 * mmap at mmap_offset; buffers live until the file and its VMAs are gone.
 * No arbitrary physical addresses can be supplied by userspace.
 */
struct sprd_vsp_buffer {
	__u32 size;
	__u32 dma_addr;
	__u32 mmap_offset;
	__u32 reserved;
};

#define VSP_ALLOC_BUFFER _IOWR('m', 32, struct sprd_vsp_buffer)

/* Import a DMA-BUF owned by the caller. Reject noncontiguous DMA mappings.
 * The import remains pinned until this file (and all of its VMAs) closes.
 */
struct sprd_vsp_import {
	__s32 fd;
	__u32 dma_addr;
	__u32 size;
	__u32 buffer_id;
};

#define VSP_IMPORT_BUFFER _IOWR('m', 33, struct sprd_vsp_import)
/* Release an import only after the decoder has dropped every DPB reference. */
#define VSP_RELEASE_IMPORT _IOW('m', 34, __u32)

/* Persistent imports on non-coherent ARM need explicit CPU/device handoff.
 * Call only while the engine is idle. cpu=1 invalidates device-written data;
 * cpu=0 transfers CPU-written data to the device. No address is user supplied.
 */
struct sprd_vsp_sync {
	__u32 buffer_id;
	__u32 cpu;
};

#define VSP_SYNC_IMPORT _IOW('m', 35, struct sprd_vsp_sync)

#endif
