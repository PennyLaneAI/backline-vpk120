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
#include <stdio.h>
#include <stdarg.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/mman.h>
#include "umm.h"
#include "bit_ops.h"

struct umm_proc_ctx *umm_ctx = NULL;

int is_xib_memory_available(unsigned int memory_type)
{
	unsigned int temp = (1U << memory_type);

	if (!umm_ctx)
		return -EFAULT;

	if (umm_ctx->platform_mem_info.supported_mem_types & temp)
		return XMM_MEM_AVAILABLE;

	return XMM_MEM_UNAVAILBLE;
}

int get_xib_mem_accessible_status(unsigned int memory_type)
{
	int ret, temp = (1U << memory_type);

	ret = is_xib_memory_available(memory_type);
	if (XMM_MEM_AVAILABLE == ret)
		if (umm_ctx->platform_mem_info.processor_access & temp)
			return XMM_PROC_HAS_ACCESS;
	return ret;
}

static int delete_chunk(struct umm_proc_ctx *ctx, struct xib_umem_chunk *chunk)
{
	unsigned int mem_type = get_mem_type(chunk->chunk_id);
	struct xib_umem_chunk *iter, *prev = NULL;

	if (!is_mem_type_valid(mem_type)) {
		pr_err("invalid memory type", -EINVAL);
		return -EINVAL;
	}

	iter = ctx->xib_umem_chunk[mem_type];
	if (!iter) {
		pr_err("list is empty", -EFAULT);
		return -EFAULT;
	}
	pthread_mutex_lock(&ctx->lock);

	while(iter) {
		if (iter->chunk_id == chunk->chunk_id) {
			if (iter == ctx->xib_umem_chunk[mem_type]) {
				ctx->xib_umem_chunk[mem_type] = iter->next;
				if (!ctx->xib_umem_chunk[mem_type])
					ctx->prev[mem_type] = NULL;
			} else
				prev->next = iter->next;
		} else {
			prev = iter;
			iter = iter->next;
			continue;
		}

		if (iter == ctx->prev[mem_type])
			ctx->prev[mem_type] = prev;
		free(iter->bmap);
		/* unmap memory*/
		munmap(iter->base_va, iter->size);
		break;
	}

	if (!iter) {
		pthread_mutex_unlock(&ctx->lock);
		pr_err("Unable to find chunk", -EINVAL);
		return -EINVAL;
	} else {
		free(iter);
		pthread_mutex_unlock(&ctx->lock);
	}

	return 0;
}

unsigned int xib_umm_get_chunk_id(void *uctx, uint64_t va)
{
	struct umm_proc_ctx *ctx = get_umem_ctx(uctx);
	struct xib_umem_chunk *chunk, *iter;
	unsigned int i;
	int ret = 0;

	pthread_mutex_lock(&ctx->lock);
	/* memory types starts from 1 */
	for (i = (XMEM_TYPE_MIN_VAL + 1); i < XMEM_MAX_MEM_TYPES;
			i++) {
		/* get chunk info first */
		chunk = ctx->xib_umem_chunk[i];

		for (iter = chunk; iter; iter = iter->next)
			if (is_vaddr_within_range(iter, va, 1)) {
				pthread_mutex_unlock(&umm_ctx->lock);
				return iter->chunk_id;
			}
	}

	pthread_mutex_unlock(&umm_ctx->lock);
	return XMEM_INVALID_CHUNK_ID;
}

int xib_umem_get_phy_addr(void *uctx, unsigned int chunk_id, uint64_t va,
			uint64_t *pa)
{
	struct umm_proc_ctx *umm_ctx = get_umem_ctx(uctx);
	struct xib_umem_chunk *chunk;
	int ret = 0;

	chunk = get_chunk_info(umm_ctx, chunk_id);
	if (!chunk) {
		pr_err("Unable to find required chunk", -EINVAL);
		return -EINVAL;
	}

	if (!((chunk->base_va <= va) && ((chunk->base_va + chunk->size) >= va))) {
		pr_err("Invalid va for the given chunk", -EINVAL);
		return -EINVAL;
	}

	*pa = (va - chunk->base_va + chunk->base_pa);
	return 0;
}

int xib_umem_free_chunk(void *uctx, int chunk_id)
{
	struct umm_proc_ctx *umm_ctx = get_umem_ctx(uctx);
	struct xib_umem_ioctl_info req;
	int ret = 0;
	struct xib_umem_chunk *chunk;

	chunk = get_chunk_info(umm_ctx, chunk_id);
	if (!chunk) {
		pr_err("Unable to find required chunk", -EINVAL);
		return -EINVAL;
	}

	if (is_chunk_in_use(chunk)) {
		pr_err("Chunk is being in use. Can't be freed", -EFAULT);
		return -EFAULT;
	}

	/* check if chunk is being in use */
	req.chunk_id = chunk_id;
	req.type = chunk->mem_type;

	ret = ioctl(umm_ctx->mm_fd, XMM_FREE_OPC, &req);
	if (ret) {
		pr_err("Failed to request ioctl. Erro code is", errno);
		return ret;
	}
	ret = delete_chunk(umm_ctx, chunk);
	return ret;
}

int update_umem_table(struct umm_proc_ctx *ctx, struct xib_umem_ioctl_info *req,
			uint64_t blk_size)
{
	struct xib_umem_chunk *chunk;
	uint32_t cnt, int_cnt;
	volatile void *va;

	chunk = calloc(1, sizeof (*chunk));
	if (!chunk) {
		pr_err("Failed to alloc memory", -ENOMEM);
		return -ENOMEM;
	}

	chunk->next = NULL;
	chunk->size = req->size;
	chunk->free_mem = req->size;
	chunk->base_pa = req->chunk_ofs;
	chunk->chunk_id = req->chunk_id;
	chunk->blk_size = blk_size;
	chunk->mem_type = req->type;

	/* Create a bit map for the blocks */
	chunk->max_blks = SIZE_TO_BLOCKS(chunk->size, blk_size);

	chunk->bmap = calloc(1, BITS_TO_INTS(chunk->max_blks) * sizeof(int));
	if (!chunk->bmap) {
		pr_err("Failed to allocate memory for bit map", -ENOMEM);
		free(chunk);
		return -ENOMEM;
	}

	chunk->free_blks = chunk->max_blks;

	/* TODO: can modify in better way.
	 * Making virtual address same as physical address,
	 * if want to use the memory type 64bit DDR in microblaze arch.
	 */
	if (get_xib_mem_accessible_status(req->type) != XMM_PROC_HAS_ACCESS) {
		chunk->base_va = chunk->base_pa;
	} else {
		va = (volatile uintptr_t)mmap(NULL, chunk->size, PROT_READ |
				PROT_WRITE | PROT_EXEC, MAP_SHARED,
				ctx->mm_fd, chunk->base_pa);

		if (va == MAP_FAILED) {
			pr_err("Memory mapping failed", errno);
			free(chunk->bmap);
			free(chunk);
			return -EFAULT;
		}
		chunk->base_va = va;
	}
	pthread_mutex_lock(&ctx->lock);

	if (!ctx->xib_umem_chunk[req->type])
		ctx->xib_umem_chunk[req->type] = chunk;
	else
		ctx->prev[req->type]->next = chunk;
	ctx->prev[req->type] = chunk;

	pthread_mutex_unlock(&ctx->lock);
	return chunk->chunk_id;
}

static struct xib_umem_chunk *get_chunk_info(struct umm_proc_ctx *ctx, int chunk_id)
{
	struct xib_umem_chunk *chunk, *iter;
	uint32_t mem_type = get_mem_type(chunk_id);

	pthread_mutex_lock(&ctx->lock);
	chunk = ctx->xib_umem_chunk[mem_type];

	for (iter = chunk; iter; iter = iter->next)
		if (iter->chunk_id == chunk_id)
			break;
	pthread_mutex_unlock(&ctx->lock);
	return iter;
}

unsigned int reserve_mem_blocks(struct xib_umem_chunk *chunk, unsigned int blk_cnt)
{
	unsigned int bit_num;

	bit_num = find_zero_bits(chunk->bmap, blk_cnt, chunk->max_blks);
	if (bit_num < chunk->max_blks) {
		set_bit_map(chunk->bmap, bit_num, blk_cnt);
		chunk->free_mem -= (blk_cnt * chunk->blk_size);
		chunk->free_blks -= blk_cnt;
	} else
		pr_err("Required number of continuous free blocks are not available", -EFAULT);

	return bit_num;
}

volatile uint64_t xib_umem_alloc_mem(void *uctx, int chunk_id, uint64_t size)
{
	struct umm_proc_ctx *umm_ctx = get_umem_ctx(uctx);
	unsigned int blk_cnt, blk_num;
	struct xib_umem_chunk *chunk;

	if (!size) {
		pr_err("Size can't be 0", -EINVAL);
		return -EINVAL;
	}

	chunk = get_chunk_info(umm_ctx, chunk_id);
	if (!chunk) {
		pr_err("Unable to find required chunk", -EINVAL);
		return XMEM_INVALID_VIRT_ADD;
	}

	pthread_mutex_lock(&umm_ctx->lock);
	blk_cnt = SIZE_TO_BLOCKS(size, chunk->blk_size);

	if (!chunk_has_mem(chunk, blk_cnt)) {
		pr_err("Memory not available to allocate from chunk", -ENOMEM);
		goto err;
	}

	/* reserve the memory blocks */
	blk_num = reserve_mem_blocks(chunk, blk_cnt);
	if (blk_num >= chunk->max_blks) {
		pr_err("Memory not available to allocate from chunk", -ENOMEM);
		goto err;
	}
	pthread_mutex_unlock(&umm_ctx->lock);
	/* get corresponding va */
	return chunk->base_va + blk_num * chunk->blk_size;
err:
	pthread_mutex_unlock(&umm_ctx->lock);
	return XMEM_INVALID_VIRT_ADD;
}

int xib_umem_free_mem(void *uctx, unsigned int chunk_id, uint64_t  uva,
			uint64_t size)
{
	struct umm_proc_ctx *umm_ctx = get_umem_ctx(uctx);
	uint32_t blk_cnt, blk_num;
	struct xib_umem_chunk *chunk;

	chunk = get_chunk_info(umm_ctx, chunk_id);
	if (!chunk) {
		pr_err("Unable to find required chunk", -EINVAL);
		return -EINVAL;
	}

	if (!is_vaddr_within_range(chunk, uva, size)) {
		pr_err("Given VA & size are not in the chunk's range", -EINVAL);
		return -EINVAL;
	}

	pthread_mutex_lock(&umm_ctx->lock);

	blk_cnt = SIZE_TO_BLOCKS(size, chunk->blk_size);
	blk_num = get_blk_num(chunk, uva);

	clear_bit_map(chunk->bmap, blk_num, blk_cnt);
	chunk->free_mem += (blk_cnt * chunk->blk_size);
	chunk->free_blks += blk_cnt;

	pthread_mutex_unlock(&umm_ctx->lock);
	return 0;
}

static int xib_get_supported_mem_info(struct umm_proc_ctx *umm_ctx)
{
	int ret = 0;

	/* open device */
	ret = ioctl(umm_ctx->mm_fd, XMM_SUPPORT_OPC,
			&umm_ctx->platform_mem_info);
	if (ret) {
		pr_err("Failed to request ioctl\n", -errno);
		memset(&umm_ctx->platform_mem_info, 0,
			sizeof (umm_ctx->platform_mem_info));
	}

	return ret;
}


static int request_memory(void *ucontext, unsigned int memory_type,
			uint64_t blk_size, uint64_t total_size)
{
	struct xib_umem_ioctl_info req;
	int ret = 0;
	struct umm_proc_ctx *umm_ctx = get_umem_ctx(ucontext);

	req.type = memory_type;
	req.size = total_size;

	ret = ioctl(umm_ctx->mm_fd, XMM_ALLOC_OPC, &req);
	if (ret) {
		printf("Failed to request ioctl. ret code is %d \n", errno);
		return ret;
	}

	ret = update_umem_table(umm_ctx, &req, blk_size);
	return ret;
}


int xib_umem_alloc_chunk(void *ucontext, int memory_type,
			uint64_t block_size, uint64_t total_size,
			bool proc_access)
{
	int ret;
	uint64_t temp = total_size;
	unsigned int page_size = PAGE_SIZE;

	if ((!block_size) || (!total_size))
		return -EINVAL;

	if (!is_mem_type_valid(memory_type)) {
		printf("Invalid memory type\n");
		return -EINVAL;
	}

	ret = get_xib_mem_accessible_status(memory_type);
	if (ret == XMM_MEM_UNAVAILBLE) {
		pr_err("Requested memory type doesn't exist", -EINVAL);
		return -EINVAL;
	} else if (ret < 0) {
		pr_err("Unexpected exception", -EFAULT);
		return ret;
	}

	if (proc_access) {
		if (ret == XMM_MEM_AVAILABLE) {
			pr_err("Processor doesnt have access "
					"to the requested memory", -EINVAL);
			return -EINVAL;
		}
	}

	/* page size will always be power of 2 */
	if (total_size & (page_size - 1)) {
		total_size = total_size & (~(page_size - 1)) ;
		total_size += page_size;
		if (temp == block_size)
			block_size = total_size; //chunk without blocks
	}

#if 0
	/* check whether or not tot size is multiples of
		page size */
	if (block_size & (block_size - 1)) {
		printf("Block size %d is not a power of 2."
			" Considering default block size %d \n", block_size,
			XMEM_DEF_MEM_BLOCK_SIZE);
		block_size = XMEM_DEF_MEM_BLOCK_SIZE;
	}
#endif

	ret = request_memory(ucontext, memory_type, block_size, total_size);
	if (ret < 0)
		pr_err("Failed to allocate memory", -EFAULT);

	/* returns chunk id */
	return ret;
}

static void update_req_data(struct xib_umem_ctx_info *info)
{
	if (!info)
		return;

	if (!info->max_qp_per_app)
		info->max_qp_per_app = XMEM_MAX_APP_QP_DEF;
	if (info->rq_buffer_size < XMEM_MIN_RQ_BUF_SIZE)
		info->rq_buffer_size = XMEM_MIN_RQ_BUF_SIZE;
	if (info->rq_depth < XMEM_DEF_RQ_DEPTH)
		info->rq_depth = XMEM_DEF_RQ_DEPTH;
	if (!info->sq_depth < XMEM_DEF_SQ_DEPTH)
		info->sq_depth = XMEM_DEF_SQ_DEPTH;
}

static int get_mem_alloc_type(const char *queue)
{
	char mem_type[20], *env;
	int mem = XMEM_PL_DDR;

	if (!queue)
		return -EINVAL;

	snprintf(mem_type, sizeof(mem_type), "XMM_%s_TYPE", queue);

	env = getenv(mem_type);
	if (!env) {
		printf("Unable to find memtype %s\n", mem_type);
		return -EFAULT;
	}

	if (!strcasecmp(env, XMEM_ENV_PL_STR))
		mem = XMEM_PL_DDR;
	else if (!strcasecmp(env, XMEM_ENV_PS_STR))
		mem = XMEM_PS_DDR;
	else if (!strcasecmp(env, XMEM_ENV_EDDR_STR))
		mem = XMEM_EXT_DDR;
	else if (!strcasecmp(env, XMEM_ENV_BRAM_STR))
		mem = XMEM_BRAM;
	else if (!strcasecmp(env, XMEM_ENV_PCI_DDR_STR))
		mem = XMEM_PCI_DDR;
	else
		return -EFAULT;
	return mem;
}

static int get_env_val(const char *env_name)
{
	char *env;
	uint32_t val;

	env = getenv(env_name);
	if (!env) {
		printf("Unable to find env var %s\n", env_name);
		return -EINVAL;
	}

	val = atoi(env);
	return val;
}

static int alloc_queues(struct umm_proc_ctx *ctx)
{
	#define XRNIC_SQ_WQE_SIZE (64)
	#define CQE_SIZE	(4)

	int ret = 0, chunk_id;
	unsigned int blk_size;
	struct xib_umm_create_ctx_resp* rsp = &ctx->resp;
	unsigned int per_qp_alloc_size, alloc_size;
	unsigned int page_size = getpagesize(), mem_type;
	bool proc_access = true;

	/* create SQ */
	mem_type = get_mem_alloc_type(XMEM_ENV_SQ_STR);
	if (mem_type < 0) {
		printf("SQ environment variable not set\n");
		mem_type = XMEM_DEF_SQ_MEM;
	}

#if 0
	blk_size = ctx->sq_depth * XRNIC_SQ_WQE_SIZE;
#else
	blk_size = page_size;
#endif

	per_qp_alloc_size = ctx->sq_depth * XRNIC_SQ_WQE_SIZE;
	alloc_size = per_qp_alloc_size * ctx->max_qp;

	ret = alloc_size & (page_size - 1);
	if (ret)
		alloc_size += (page_size - ret);

	/* allocate 1 block per each QP */

	if (XMM_PROC_HAS_ACCESS != get_xib_mem_accessible_status(mem_type)) {
		pr_err("SQs must be allocated from processor accessible memory",
			-EINVAL);
		return -EFAULT;
	}

	chunk_id = xib_umem_alloc_chunk(ctx->ctx, mem_type, blk_size,
					alloc_size, proc_access);
	if (chunk_id < 0) {
		pr_err("Failed to allocate SQ", -ENOMEM);
		return -ENOMEM;
	}
	rsp->sq_chunk_id = chunk_id;

	/* create CQ */
	mem_type = get_mem_alloc_type(XMEM_ENV_CQ_STR);
	if (mem_type < 0) {
		printf("CQ environment variable not set\n");
		mem_type = XMEM_DEF_SQ_MEM;
	}

#if 1
	blk_size = ctx->sq_depth * CQE_SIZE;
#else
	blk_size = getpagesize();
#endif

	per_qp_alloc_size = ctx->sq_depth * CQE_SIZE;
	alloc_size = per_qp_alloc_size * ctx->max_qp;

	ret = alloc_size & (page_size - 1);
	if (ret)
		alloc_size += (page_size - ret);

	if (XMM_PROC_HAS_ACCESS != get_xib_mem_accessible_status(mem_type)) {
		pr_err("CQs must be allocated from processor accessible memory",
			-EINVAL);
		/* free SQ chunk */
		goto cq_alloc_error;
	}

	/* allocate 1 block per each QP */
	chunk_id = xib_umem_alloc_chunk(ctx->ctx, mem_type, blk_size,
					alloc_size, proc_access);
	if (chunk_id < 0) {
		pr_err("Failed to allocate CQ", -ENOMEM);
		goto cq_alloc_error;
	}
	rsp->cq_chunk_id = chunk_id;

	/* create RQ */
	mem_type = get_mem_alloc_type(XMEM_ENV_RQ_STR);
	if (mem_type < 0) {
		printf("RQ environment variable not set\n");
		mem_type = XMEM_DEF_SQ_MEM;
	}

#if 0
	blk_size = ctx->rq_depth * ctx->rq_buffer_size;
#else
	blk_size = getpagesize();
#endif

	per_qp_alloc_size = ctx->rq_depth * ctx->rq_buffer_size;
	alloc_size = per_qp_alloc_size * ctx->max_qp;

	ret = alloc_size & (page_size - 1);
	if (ret)
		alloc_size += (page_size - ret);

	if (XMM_PROC_HAS_ACCESS != get_xib_mem_accessible_status(mem_type)) {
		pr_err("RQs must be allocated from processor accessible memory",
			-EINVAL);

		/* free CQ, SQ chunk */
		goto rq_alloc_error;
	}

	/* allocate 1 block per each QP */
	chunk_id = xib_umem_alloc_chunk(ctx->ctx, mem_type, blk_size,
					alloc_size, proc_access);
	if (chunk_id < 0) {
		pr_err("Failed to allocate RQ", -ENOMEM);
		goto rq_alloc_error;
	}
	rsp->rq_chunk_id = chunk_id;

	return 0;

cq_alloc_error:
	ret = xib_umem_free_chunk(ctx, rsp->sq_chunk_id);
rq_alloc_error:
	ret = xib_umem_free_chunk(ctx, rsp->cq_chunk_id);
	return -EFAULT;
}

struct xib_umm_create_ctx_resp *xib_umem_create_context(void *context,
					struct xib_umem_ctx_info *umm_info)
{
	struct xib_umm_create_ctx_resp resp;
	int ret, fd;
	/* rdma-core v18 has data structure incompatibility with 2020.1 kernel. This
	requires moving to rdma-core latest. Under this macro, few env
	vars as work-arounds are introduced, which should be removed when moved to
	latest rdma-core */

	if (umm_ctx)
		return &umm_ctx->resp;

	if (!umm_info) {
		pr_err("User context information should not be NULL", -EINVAL);
		return NULL;
	}

	/* open device */
	fd = open(XMEM_DEV, O_RDWR | O_SYNC);
	if (fd < 0) {
		pr_err("Failed to open device file", -EFAULT);
		return NULL;
	}

	/* allocate context */
	umm_ctx = (struct umm_proc_ctx *) calloc(1, sizeof(*umm_ctx));
	if (!umm_ctx) {
		pr_err("Failed to allocate umem context", -ENOMEM);
		close(fd);
		return NULL;
	}

	umm_ctx->mm_fd = fd;
	/* Initialize umem context */
	umm_ctx->ctx = context;

	/* Copy device capabilities */
	umm_ctx->max_qp_per_app = umm_info->max_qp_per_app;
	umm_ctx->max_qp = umm_ctx->max_qp_per_app;
	umm_ctx->rq_buffer_size = umm_info->rq_buffer_size;
	umm_ctx->rq_depth = umm_info->rq_depth;
	umm_ctx->sq_depth = umm_info->sq_depth;
	update_req_data(umm_info);

	ret = xib_get_supported_mem_info(umm_ctx);
	/* create SQ, RQ, CQ memory */
	ret = alloc_queues(umm_ctx);
	if (ret) {
		close(fd);
		free(umm_ctx);
		return NULL;
	}
	return &umm_ctx->resp;
}

void xib_umem_delete_context(void *ucontext)
{
	struct umm_proc_ctx *umm_ctx = get_umem_ctx(ucontext);
	unsigned int i, ret = 0;
	struct xib_umem_chunk *chunk;

	/* check if context is being in use */
	pthread_mutex_lock(&umm_ctx->lock);
	for (i = 1; i < XMEM_MAX_MEM_TYPES; i++) {
		if (umm_ctx->xib_umem_chunk[i]) {
			/* check if any chunks are being in use */
			chunk = umm_ctx->xib_umem_chunk[i];
			while (chunk) {
				if (is_chunk_in_use(chunk))
					goto fail;
				ret = xib_umem_free_chunk(ucontext, umm_ctx->resp.sq_chunk_id);
				ret |= xib_umem_free_chunk(ucontext, umm_ctx->resp.rq_chunk_id);
				ret |= xib_umem_free_chunk(ucontext, umm_ctx->resp.cq_chunk_id);
				if (!ret) {
					pr_err("Failed to free chunks", -EFAULT);
				}
				chunk = chunk->next;
			}
		}
	}

	pthread_mutex_unlock(&umm_ctx->lock);
	return;
fail:
	pr_err("Chunks are in use. Can't delete context", -EFAULT);
	pthread_mutex_unlock(&umm_ctx->lock);
}
