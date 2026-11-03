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
#ifndef _XIB_KMM_IOCTL_H
#define _XIB_KMM_IOCTL_H
enum xmem_ioctl_opc {
	XMEM_ALLOC_OPC = 0,
	XMEM_FREE_OPC,
	XMEM_SUPPORT_OPC,
	XMEM_MAX_KMM_FNS,
};

struct xib_kmm_mem_support_info {
	/* 1 indicates memory type is supported */
	unsigned int supported_mem_types;
	/* 1 indicates processor has access */
	unsigned int processor_access;
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

#define XMEM_MGR_MAGIC_KEY	'M'
#define XMM_ALLOC_OPC	_IOWR(XMEM_MGR_MAGIC_KEY, XMEM_ALLOC_OPC,\
				struct xib_umem_ioctl_info)
#define XMM_FREE_OPC	_IOWR(XMEM_MGR_MAGIC_KEY, XMEM_FREE_OPC,\
				struct xib_umem_ioctl_info)
#define XMM_SUPPORT_OPC _IOWR(XMEM_MGR_MAGIC_KEY, XMEM_SUPPORT_OPC,\
				struct xib_kmm_mem_support_info)

static long xib_ioctl(struct file *fp,unsigned int cmd, unsigned long arg);
int alloc_mem(struct xib_umem_ioctl_info *req);
int free_mem(struct xib_umem_ioctl_info *req);
#endif
