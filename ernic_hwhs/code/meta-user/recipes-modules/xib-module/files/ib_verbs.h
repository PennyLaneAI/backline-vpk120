/*
 * AMD FPGA Xilinx ERNIC Infiniband Driver
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
#ifndef _XIB_IB_VERBS_H_
#define _XIB_IB_VERBS_H_

//#define DEBUG_IPV6

#define SQ_BASE_ALIGN_SZ	32
#define SQ_BASE_ALIGN(addr)	ALIGN(addr, SQ_BASE_ALIGN_SZ)
#define SQ_BASE_ALIGNED(addr)	IS_ALIGNED((unsigned long)(addr), \
				SQ_BASE_ALIGN_SZ)

#define RQ_BASE_ALIGN_SZ	256
#define RQ_BASE_ALIGN(addr)	ALIGN(addr, RQ_BASE_ALIGN_SZ)
#define RQ_BASE_ALIGNED(addr)	IS_ALIGNED((unsigned long)(addr), \
				RQ_BASE_ALIGN_SZ)

#define XIB_MAX_RQE_SGE		8
#define XIB_MAX_SQE_SGE		8

enum xib_cq_type {
	XIB_CQ_TYPE_GSI,
	XIB_CQ_TYPE_KERNEL,
	XIB_CQ_TYPE_USER,
};

enum xib_qp_type {
	XIB_QP_TYPE_GSI,
	XIB_QP_TYPE_KERNEL,
	XIB_QP_TYPE_USER,
};

enum xib_qp_state {
	XIB_QP_STATE_RESET,
	XIB_QP_STATE_INIT,
	XIB_QP_STATE_RTR,
	XIB_QP_STATE_RTS,
	XIB_QP_STATE_SQD,
	XIB_QP_STATE_ERR,
	XIB_QP_STATE_SQE
};

struct xib_rqe {
	u64 wr_id;
	u32		num_sge;
	u32		ip_version;
#ifdef DEBUG_IPV6
	u32		reserved;
#endif
	struct ib_sge sg_list[XIB_MAX_RQE_SGE];
};

struct xib_rq {
	void			*rq_ba_v;
	u64			rq_ba_p;
	u32			rq_wrptr_db_local;
	u32			rq_ci_db_local;
	u32			prod;
	u32			cons;
	u32			gsi_cons;
	u32			max_wr;

	/* store rqe from stack */
	struct xib_rqe		*rqe_list;
};

struct xib_pl_buf {
	void *va;
	u64 pa;
	u64 sgl_addr;
	size_t len;
};

struct xib_sqd {
        uint64_t        wr_id;
        struct xib_sqd *next;
};

struct xib_sq {
	u32			sq_cmpl_db_local;
	u32			send_cq_db_local;
	struct {
		u64		wr_id;
		bool		signaled;
	} *wr_id_array;
        struct xib_sqd *sqd_wr_list;
        uint64_t        sqd_length;
	u64			*sgl_pa;
	struct xib_pl_buf	*pl_buf_list;
	u32			max_wr;
};

struct xib_cq {
	struct ib_cq		ib_cq;
	struct ib_umem		*umem;
	void			*buf_v;
	u64			buf_p;
	enum xib_cq_type	cq_type;
	struct xib_qp		*qp;
	spinlock_t		cq_lock;
};

struct qp_hw_hsk_cfg {
	u64			data_ba_p;
	void			*data_ba_va;
	u64			sq_ba_p;
	void			*sq_ba_va;
};

#define SEND_INVALIDATE         0x1
#define SEND_IMMEDIATE          0x2
#define WRITE_IMMEDIATE         0x3

struct xib_imm_inv {
	u32	data;
	u8	type;
	bool	isvalid;
};

struct xib_qp {
	struct ib_qp		ib_qp;
	u32			sq_depth;
	u64			sq_ba_p;
	void			*sq_ba_v;

	bool			send_sgl_busy;
	bool			is_ipv6;
#ifdef DEBUG_IPV6
	bool			res1;
#endif
	u64			send_sgl_p;
	void			*send_sgl_v;

	enum xib_qp_state	state;

	spinlock_t		sq_lock;
	spinlock_t		rq_lock;

	void			*ua_v;
	dma_addr_t		ua_p;
	u32			ua_size;

	/* RQ */
	struct xib_rq		rq;

	/* SQ */
	struct xib_sq		sq;

	/* CQ */
	struct xib_cq		*sq_cq;
	struct xib_cq		*rq_cq;

	u32			hw_qpn;
	enum xib_qp_type	qp_type;

	/* QP1 */
	u32			send_psn;
	bool			io_qp;
	struct ib_ud_header	qp1_hdr;
	struct tasklet_struct	comp_task;
	struct tasklet_struct	cnp_task;
	struct qp_hw_hsk_cfg	hw_hs_cfg;
	struct tasklet_struct	fatal_hdlr_task;
	struct completion	sq_drained;
	u32			sq_polled_count;
	u32			post_send_count;
	u32			rq_buf_size;
	struct xib_imm_inv	*imm_inv_data;
};

struct xib_qp_modify_params {
	u32			flags;
#define XIB_MODIFY_QP_SQ_PSN		(1 << 0)
#define XIB_MODIFY_QP_PKEY		(1 << 1)
#define XIB_MODIFY_QP_DEST_QP		(1 << 2)
#define XIB_MODIFY_QP_STATE		(1 << 3)
#define XIB_MODIFY_QP_AV		(1 << 4)
#define XIB_MODIFY_QP_DEST_MAC		(1 << 5)
#define XIB_MODIFY_QP_TIMEOUT		(1 << 6)
#define XIB_MODIFY_QP_RQ_PSN		(1 << 7)
	enum xib_qp_state	qp_state;
	u16			pkey;
	u32			dest_qp;
	u16			mtu;
	u8			traffic_class;
	u8			hop_limit;
	u8			dmac[6];
	u16			udp_src_port;

	u8			retry_cnt;
	u8			rnr_retry_cnt;
	u32			rq_psn;
	u32			sq_psn;
#ifdef DEBUG_IPV6
	u8			res1;
	union {
		__be32		ip4_daddr;
		u8		res2[16];
	};
#else
	u8			ip_version;
	union {
		__be32		ip4_daddr;
		u8		ipv6_addr[16];
	};
#endif
};

static inline struct xib_qp *get_xib_qp(struct ib_qp *ibqp)
{
	return container_of(ibqp, struct xib_qp, ib_qp);
}

static inline struct xib_cq *get_xib_cq(struct ib_cq *ibcq)
{
	return container_of(ibcq, struct xib_cq, ib_cq);
}

static inline void xib_rq_prod_inc(struct xib_rq *rq)
{
	rq->prod = (rq->prod + 1) % rq->max_wr;
}

static inline void xib_rq_cons_inc(struct xib_rq *rq)
{
	rq->cons = (rq->cons + 1) % rq->max_wr;
}

static inline void xib_inc_sw_gsi_cons(struct xib_rq *rq)
{
	rq->gsi_cons = (rq->gsi_cons + 1) % rq->max_wr;
}

int xib_get_rq_recd(struct xib_rq *rq, u32 rq_wr_current);
int xib_poll_kernel_cq(struct ib_cq *ibcq, int num_entries, struct ib_wc *wc);

int xib_gsi_create_qp(struct ib_qp *kqp,
                      struct ib_pd *ib_pd,
                      struct ib_qp_init_attr *init_attr);

int xib_gsi_poll_cq(struct ib_cq *ibcq, int num_entries, struct ib_wc *wc);
struct ib_mr *xib_reg_user_mr(struct ib_pd *pd, u64 start, u64 length,
				   u64 virt_addr, int access_flags,
				   struct ib_udata *udata);

struct ib_mr *xib_reg_user_mr_ex(struct ib_pd *pd, u64 start, u64 length,
				   u64 virt_addr, int access_flags,
				   struct ib_udata *udata);
int xib_create_user_qp(struct ib_qp *kqp,
                       struct ib_pd *ib_pd,
                       struct ib_qp_init_attr *init_attr,
                       struct ib_udata *udata);

int xib_create_kernel_qp(struct ib_qp *kqp,
                         struct ib_pd *ib_pd,
                         struct ib_qp_init_attr *init_attr);
int xib_build_qp1_send_v2(struct ib_qp *ib_qp,
			const struct ib_send_wr *wr,
			int payload_sz,
			bool *is_udp,
			u8 *ip_version);
int xib_kernel_qp_post_recv(struct ib_qp *ibqp, const struct ib_recv_wr *wr,
				const struct ib_recv_wr **bad_wr);
void xrnic_send_wr(struct xib_qp *qp, struct xilinx_ib_dev *xib);
int xrnic_qp_set(struct xib_qp *qp);
int xrnic_qp_modify(struct xib_qp *qp, struct xib_qp_modify_params *params);
void xrnic_set_dest_ip(struct xib_qp *qp);
int xib_dealloc_qp_buffers(struct ib_device *ibdev, struct xib_qp *qp);
int xib_dealloc_user_qp_buffers(struct ib_device *ibdev, struct xib_qp *qp);
int xib_dealloc_gsi_qp_buffers(struct ib_device *ibdev, struct xib_qp *qp);
int xrnic_qp_disable(struct xib_qp *qp);
int xib_get_payload_size(struct ib_sge *sg_list, int num_sge);
int xib_rst_rq(struct xib_qp *qp);
int xib_rst_cq_sq(struct xib_qp *qp, int nvmf_rhost);
void xib_drain_sq(struct ib_qp *ibqp);
void xib_drain_rq(struct ib_qp *ibqp);
#endif /* _XIB_IB_VERBS_H_ */
