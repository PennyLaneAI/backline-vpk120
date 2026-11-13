// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * AMD ERNIC user space memory manage module
 *
 * Copyright (C) 2024 AMD, Inc. All rights reserved.
 *
 * Author : Anjaneyulu Reddy Mule <anjaneyu@xilinx.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation and may be copied,
 * distributed, and modified under those terms.
 */
#ifndef _UMM_H
#define _UMM_H
//#include <ccan/container_of.h>
#include <stdbool.h>
#include <pthread.h>
#include <stddef.h>
#include "umm_export.h"


#define XMEM_DEV	"/dev/xib_kmm0"

#if !defined(PAGE_SIZE)
	#define PAGE_SIZE	getpagesize()
#endif

#define XMEM_DEF_RQ_DEPTH	(16)
#define	XMEM_DEF_SQ_DEPTH	(16)
#define XMEM_MIN_RQ_BUF_SIZE	(256)
#define XMEM_DEF_MEM_BLOCK_SIZE	(4096)

#define XMEM_DEF_SQ_MEM		XMEM_PS_DDR
#define XMEM_DEF_CQ_MEM		XMEM_PS_DDR
#define XMEM_DEF_RQ_MEM		XMEM_PS_DDR

#define XMEM_ENV_SQ_STR		"SQ"
#define XMEM_ENV_RQ_STR		"RQ"
#define XMEM_ENV_CQ_STR		"CQ"

#define XMEM_ENV_MAX_APP_QP_STR	"XMM_APP_MAX_QP"
#define XMEM_MAX_APP_QP_DEF	10
#define XMEM_ENV_RQ_SGE_STR	"XMM_APP_RQ_SGE"
#define XMEM_RQ_SGE_DEF		1
#define XMEM_DEF_RQ_BUF_SIZE	(XMEM_RQ_SGE_DEF * XLNX_RQ_SGE_SIZE)
#define XMEM_ENV_SQ_DEPTH_STR	"XMM_SQ_DEPTH"
#define XMEM_ENV_RQ_DEPTH_STR	"XMM_RQ_DEPTH"

#define XMEM_ENV_PL_STR		"PL"
#define XMEM_ENV_PS_STR		"PS"
#define XMEM_ENV_EDDR_STR	"EDDR"
#define XMEM_ENV_BRAM_STR	"BRAM"
#define XMEM_ENV_PCI_DDR_STR	"PCI_DDR"

#define ROUND_UP(n, d) (((n) + (d) -1) / (d))
#define BITS_TO_INTS(n) ROUND_UP(n, 8 * sizeof(int))

#define SIZE_TO_BLOCKS(ts, bs) (((ts) + (bs) - 1) / (bs))


static inline char *container_of_or_null_(void *member_ptr, size_t offset)
{
	return member_ptr ? (char *)member_ptr - offset : NULL;
}

/*
* UMM to KMM ioctl interface related information
*/
enum xmem_ioctl_opc {
	XMEM_ALLOC_OPC = 0,
	XMEM_FREE_OPC,
	XMEM_SUPPORT_OPC,
};

struct xib_umem_ioctl_info {
	uint32_t	type;
	uint64_t	size;
	uint64_t	chunk_ofs;
	union {
		uint32_t	chunk_id;
		struct {
			uint8_t	id[3];
			uint8_t mem_type:7;
			/* To avoid un-necessary signed int operation problems
				while shifting bits, leaving out the MSB bit to 0*/
			uint8_t reserved:1; // must always be set to 0
		};
	};
};

/* struct to save bitmap of memory types and processor access */
struct xib_umm_mem_info {
	/* 1 indicates memory type is supported by the platform */
	unsigned int supported_mem_types;
	/* 1 indicates processor can access */
	unsigned int processor_access;
};

#define XMEM_MGR_MAGIC_KEY	'M'
#define XMM_ALLOC_OPC	_IOWR(XMEM_MGR_MAGIC_KEY, XMEM_ALLOC_OPC, struct xib_umem_ioctl_info)
#define XMM_FREE_OPC	_IOWR(XMEM_MGR_MAGIC_KEY, XMEM_FREE_OPC, struct xib_umem_ioctl_info)
#define XMM_SUPPORT_OPC	_IOWR(XMEM_MGR_MAGIC_KEY, XMEM_SUPPORT_OPC, struct xib_umm_mem_info)

struct xib_umem_chunk {
	uint64_t	size, free_mem;
	uint64_t	blk_size;
	uint32_t	chunk_id;
	uint32_t 	mem_type;
	uint32_t	free_blks;
	volatile uint64_t	base_va;
	uint64_t	base_pa;  
	/* bit map to fill used buffers */
	unsigned int 	*bmap, max_blks;
	struct xib_umem_chunk *next;
};

struct umm_proc_ctx {
	/* user context */
	void *ctx;

	int mm_fd;
	/* max qp & max cq */
	uint32_t	max_qp, max_cq;
	uint32_t max_qp_per_app, rq_buffer_size, rq_depth, sq_depth;
	/* list of chunks */
	struct xib_umem_chunk *xib_umem_chunk[XMEM_MAX_MEM_TYPES];
	struct xib_umem_chunk *prev[XMEM_MAX_MEM_TYPES];
	struct xib_umm_create_ctx_resp resp;

	struct xib_umm_mem_info platform_mem_info;
	pthread_mutex_t	lock;
};

static __inline struct umm_proc_ctx *get_umem_ctx(void *uctx)
{
	extern struct umm_proc_ctx *umm_ctx;
#if 0
	return container_of(uctx, struct umm_proc_ctx, ctx);
#else
	return umm_ctx;
#endif
}

static __inline unsigned int get_blk_num(struct xib_umem_chunk *chunk, uint64_t va)
{
	return SIZE_TO_BLOCKS((va - chunk->base_va), chunk->blk_size);
}

static __inline bool is_vaddr_within_range(struct xib_umem_chunk *chunk, uint64_t va,
			unsigned int size)
{
	if ((va >= chunk->base_va) && ((chunk->base_va + chunk->size) >= (va + size)))
		return true;
	return false;
}

static __inline bool is_single_blk_mem(struct xib_umem_chunk *chunk)
{
	return ((chunk->blk_size == chunk->size) && (chunk->max_blks == 1));
}

static __inline bool is_chunk_in_use(struct xib_umem_chunk *chunk)
{
	return (chunk->free_blks != chunk->max_blks);
}

static __inline bool chunk_has_mem(struct xib_umem_chunk *chunk, uint32_t blk_cnt)
{
	if (blk_cnt > chunk->max_blks) {
		printf("Requested blk count [%d] is > max blks in chunk [%d]\n", blk_cnt, chunk->max_blks);
		return false;
	}

	return (!(chunk->free_blks < blk_cnt));
}
static struct xib_umem_chunk *get_chunk_info(struct umm_proc_ctx *ctx, int chunk_id);
#endif
