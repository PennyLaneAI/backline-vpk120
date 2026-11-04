// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * AMD ERNIC user space memory manage module
 *
 * Copyright (C) 2024 AMD, Inc. All rights reserved.
 * Copyright 2026 Xanadu Quantum Technologies Inc.
 *
 * Author : Anjaneyulu Reddy Mule <anjaneyu@xilinx.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation and may be copied,
 * distributed, and modified under those terms.
 */
#ifndef _UMM_EXPORT_H
#define _UMM_EXPORT_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>

#define UMEM_MGR_EXIST
#undef pr_err
#define pr_err(str, err) fprintf(stderr, "ERROR: at %s:%d " str " err code : %d\n", __FILE__, __LINE__, err)
#define XMEM_CHUNK_ID_MEM_TYPE_OFS	(24)
#define XMEM_CHUNK_ID_MEM_TYPE_MASK	(0x7FU) // 7bits
#define XLNX_RQ_SGE_SIZE	256

/* An invalid VA macro to indicate mem alloc failure */
#define XMEM_INVALID_VIRT_ADD	(0)

/* Memory types of ther user application and xib providers */
enum xib_mem_type {
	XMEM_TYPE_MIN_VAL = 0,
	XMEM_PL_DDR,
	XMEM_PS_DDR,
	XMEM_EXT_DDR,
	XMEM_PCI_DDR,
	XMEM_BRAM,
	XMEM_MAX_MEM_TYPES,
};

/* Theses enums indicates memory availablity and accessibility status */
enum xib_mem_availability_stat {
	XMM_MEM_UNAVAILBLE = 0,
	XMM_MEM_AVAILABLE,
	XMM_NO_PROC_ACCESS,
	XMM_PROC_HAS_ACCESS,
};

/* A macro to indicate invalid chunk ID */
#define XMEM_INVALID_CHUNK_ID	(0)

#define XMEM_IS_INVALID_VADDR(va) ((va) == XMEM_INVALID_VIRT_ADD)

/* UMM context from apps or providers*/
struct xib_umem_ctx_info {
	int	mmap_fd;
	uint32_t max_qp;
	uint32_t max_cq;
	uint32_t max_qp_per_app;
	uint32_t rq_buffer_size;
	uint32_t rq_depth, sq_depth;
};

struct xib_umm_create_ctx_resp {
	int	sq_chunk_id;
	int	cq_chunk_id;
	int	rq_chunk_id;
};

#define XIB_PS_STR	"ps"
#define XIB_PL_STR	"pl"
#define XIB_BRAM_STR	"bram"
#define XIB_EDDR_STR	"eddr"

static int xib_mem_str_to_mem_type(char *str)
{
	int mem_type;

        if (!str)
                return -EINVAL;

	if (!strcasecmp(str, XIB_PS_STR))
		mem_type = XMEM_PS_DDR;
	else if (!strcasecmp(str, XIB_PL_STR))
		mem_type = XMEM_PL_DDR;
	else if (!strcasecmp(str, XIB_BRAM_STR))
		mem_type = XMEM_BRAM;
	else if (!strcasecmp(str, XIB_EDDR_STR))
		mem_type = XMEM_EXT_DDR;
	else {
                printf("Invalid memory type: %s\n", str);
                mem_type = -EINVAL;
        }

        return mem_type;
}

static __inline int get_mem_type(int chunk_id)
{
	return ((chunk_id >> XMEM_CHUNK_ID_MEM_TYPE_OFS) &
			XMEM_CHUNK_ID_MEM_TYPE_MASK);
}

static __inline bool is_mem_type_valid(unsigned type)
{
	return ((type >= XMEM_TYPE_MIN_VAL) && (type < XMEM_MAX_MEM_TYPES));
} 

static __inline int xib_get_rq_sge_cnt(unsigned int payload_size)
{
	return (((payload_size) + XLNX_RQ_SGE_SIZE - 1) / XLNX_RQ_SGE_SIZE);
}

struct xib_umm_create_ctx_resp *xib_umem_create_context(void *context,
				struct xib_umem_ctx_info *ctx_info);

int xib_umem_free_mem(void *uctx, unsigned int chunk_id, uint64_t  uva,
				uint64_t size);

int xib_umem_alloc_chunk(void *ucontext, int memory_type,
			uint64_t block_size, uint64_t total_size,
			bool proc_access);

volatile uint64_t xib_umem_alloc_mem(void *uctx, int chunk_id,
			uint64_t size);

int xib_umem_free_chunk(void *uctx, int chunk_id);

int xib_umem_get_phy_addr(void *uctx, unsigned int chunk_id, uint64_t va,
			uint64_t* pa);
unsigned int xib_umm_get_chunk_id(void *uctx, uint64_t va);
int is_xib_memory_available(unsigned int memory_type);
int get_xib_mem_accessible_status(unsigned int memory_type);
#endif
