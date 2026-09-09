/*
 * AMD FPGA ERNIC Infiniband Driver
 *
 * Copyright (C) 2024 AMD, Inc. All rights reserved.
 *
 * Author: Syed S <syeds@xilinx.com>
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
#ifndef _XIB_H_
#define _XIB_H_

#include <linux/kernel.h>
#include <linux/interrupt.h>
#include <rdma/ib_verbs.h>
#include "rnic.h"
#include "axidma.h"

struct fn_list {
	int	(*fn_handler)(void *arg1, void *arg2, int len);
	int	fn_id;
};

struct mod_list {
	int	mod_id;
	int	no_of_fns;
	struct	fn_list *fn;
};

struct vendor_req_table {
	int	max_modules;
	struct	mod_list *mod;
};

#define XIB_MAX_BMAP_NAME	(16)
struct xib_bmap {
	unsigned long *bitmap;
	u32 max_count;
	char name[XIB_MAX_BMAP_NAME];
};

#if defined (CONFIG_ARCH_ZYNQMP) || defined (CONFIG_ARCH_ZYNQ) \
	|| (defined CONFIG_ARCH_VERSAL)
#define ARCH_HAS_PS
#endif

struct xilinx_ib_dev_attr {
	u32		max_pd;
	u32		max_cq_wqes;
	u32		max_qp_wqes;
	u32		max_qp;
	u32		max_mr;
	u32		max_send_sge;

};

struct xilinx_ib_dev {
	struct ib_device		ib_dev;
	struct platform_device		*pdev;
	struct net_device		*netdev;
	u8				active_speed;
	u8				active_width;
	u16				mtu;
	struct xilinx_ib_dev_attr	dev_attr;
	struct xrnic_local		*xl;
	struct xib_qp			*gsi_qp;
	spinlock_t			lock;
	struct xib_bmap			pd_map;
	struct xib_bmap			qp_map;
	struct xib_bmap			mr_map;
	struct xib_qp			**qp_list;
	struct kobject			*pfc_kobj;
	struct axidma_q                 *dmaq;
};

struct xib_event_data {
	struct xilinx_ib_dev		*xib;
	u32				event;
	u32				qpn;
};

enum xib_mr_type {
	XIB_MR_DMA,
	XIB_MR_USER
};
struct xib_mr {
	struct ib_mr		ib_mr;
	struct ib_umem		*umem;
	u64			size;
	u32			mr_idx;
	u32			pd;
	int			type;
	u8			rkey;
};

struct xib_pd {
	struct ib_pd		ib_pd;
	u32			pdn;
};

struct xib_ah {
	struct ib_ah ib_ah;
	struct rdma_ah_init_attr attr;
};

struct xib_ucontext {
	struct ib_ucontext	ib_uc;
	int			pfn;
	unsigned int		index;
};


#define UPPER_32_BITS(a) ((a) >> 32U)

#define XRNIC_SQ_WQE_SIZE 64
#define XRNIC_SEND_SGL_SIZE 4096
#if 0
#define XRNIC_SQ_DEPTH 128
#define USER_RQ_DEPTH 16
#define USER_SQ_DEPTH 64
#define USER_CQ_SIZE  2048
#endif
#define CQE_SIZE	4

static inline struct xilinx_ib_dev *get_xilinx_dev(struct ib_device *ibdev)
{
	return container_of(ibdev, struct xilinx_ib_dev, ib_dev);
}

static inline struct xib_ucontext *get_xib_ucontext(struct ib_ucontext *ibuc)
{
	return container_of(ibuc, struct xib_ucontext, ib_uc);
}

static inline struct xib_ah *get_xib_ah(struct ib_ah *ibah)
{
	return container_of(ibah, struct xib_ah, ib_ah);
}

static inline struct xib_pd *get_xib_pd(struct ib_pd *ibpd)
{
	return container_of(ibpd, struct xib_pd, ib_pd);
}

static inline struct xib_mr *get_xib_mr(struct ib_mr *ibmr)
{
	return container_of(ibmr, struct xib_mr, ib_mr);
}

irqreturn_t xib_irq(int irq, void *ptr);

int xib_bmap_alloc(struct xib_bmap *bmap, u32 max_count, char *name);
int xib_bmap_alloc_id(struct xib_bmap *bmap, u32 *id_num);
void xib_bmap_release_id(struct xib_bmap *bmap, u32 id_num);
void *xib_alloc_coherent(char *from, void *xib,
		size_t len, u64 *dma_handle, gfp_t flags);
void *xib_zalloc_coherent(char *from, struct xilinx_ib_dev *xib,
		size_t len, u64 *dma_handle, gfp_t flag);
void xib_free_coherent(void *xib,
		u64 size, void *cpu_addr, u64 dma_handle);
int xib_bram_init(void);
int xib_ext_ddr_init(void);
char *xib_get_sq_mem(void);
char *xib_get_rq_mem(void);
bool xib_pl_present(void);
int xib_register_pl_allocator(struct device *dev);
void xib_fatal_handler(unsigned long data);
void xib_comp_handler(unsigned long data);
void xib_cnp_handler(unsigned long data);
void xib_gsi_comp_handler(unsigned long data); 
int axidma_init(struct xilinx_ib_dev *xl, struct device_node *ernic_np);
int axidma_exit(struct xilinx_ib_dev *xl);
#ifdef CONFIG_MICROBLAZE
int axitimer_init(struct device_node *ernic_np);
void axitimer_exit(void);
#endif
#endif /* _XIB_H_ */
