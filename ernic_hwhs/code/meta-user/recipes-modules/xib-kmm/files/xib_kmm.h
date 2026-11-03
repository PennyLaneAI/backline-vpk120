/*
 * AMD FPGA ERNIC kernel memory manager driver
 *
 * Copyright (C) 2024 AMD, Inc. All rights reserved.
 *
 * Author: Ravali P <ravalip@xilinx.com>
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#ifndef _XIB_KMM_H
#define _XIB_KMM_H

#include "xib_kmm_ioctl.h"
#include "xib_kmm_export.h"

#define DEV_MINOR_BASE	0
#define DEVS_CNT	1
#define DEV_NAME	"xib_kmm"

struct xib_kmem_chunk {
	u64     start_addr;
	u64     size;
	u32     chunk_id;
	struct list_head   chunk_list;
};

struct xib_kmem_info {
	xib_mem_type    type;
	u64             base_addr;
	struct page	**pages;
	u64 		base_va;
	u64             size;
	u64		available_size;
	uint32_t	max_chunk;
	struct list_head chunk_list;
	struct list_head mmr_list;
	spinlock_t	chunk_lock;
	struct device		*dev;
	struct percpu_ref	*percpu_ref;
	struct completion	comp;
	struct dev_pagemap	pg_map;

};
struct xlnx_kmm_config {
	int dummy;
};

struct xib_kmm_ctx {
	struct xib_kmm_mem_support_info platform_mem_info;
};

struct xib_kmem_info *find_node(struct xib_kmem_info *mmr, xib_mem_type type);
int add_node(xib_mem_type type, u64 base_addr, u64 size, struct resource *,
		struct device *, bool);
int xib_close(struct inode *inode, struct file *fp);
static int xib_open(struct inode *inode, struct file *fp);
static ssize_t xib_read (struct file *fp, char *buf, size_t len, loff_t *ofs);
static ssize_t xib_write(struct file *fp, const char *buf, size_t len, loff_t *ofs);
static int xib_mmap(struct file *fp, struct vm_area_struct *vma);
#endif
