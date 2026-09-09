/*
 * AMD FPGA ERNIC Infiniband Driver
 *
 * Copyright (C) 2024 AMD, Inc. All rights reserved.
 * Copyright 2026 Xanadu Quantum Technologies Inc.
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
#ifndef _XRNIC_H_
#define _XRNIC_H_

#include <linux/types.h>
#include <linux/io.h>

#define XRNIC_INVALID_OPC -1

#define XRNIC_BUF_RKEY_MASK		(0xFF)
#define XRNIC_MR_PDNUM(mrn)		(0x00 + (mrn) * 0x100)
#define XRNIC_MR_VA_LO(mrn)		(0x04 + (mrn) * 0x100)
#define XRNIC_MR_VA_HI(mrn)		(0x08 + (mrn) * 0x100)
#define XRNIC_MR_BUF_BASE_LO(mrn)	(0x0c + (mrn) * 0x100)
#define XRNIC_MR_BUF_BASE_HI(mrn)	(0x10 + (mrn) * 0x100)
#define XRNIC_MR_BUF_RKEY(mrn)		(0x14 + (mrn) * 0x100)
#define XRNIC_MR_WRRD_BUF_LEN(mrn)	(0x18 + (mrn) * 0x100)
#define XRNIC_MR_ACC_DESC(mrn)		(0x1c + (mrn) * 0x100)

#define XRNIC_VIRT_ADDR_0(q)	(0x04 + (q) * 0x100)
#define XRNIC_VIRT_ADDR_1(q)	(0x08 + (q) * 0x100)
#define XRNIC_BUFBASE_ADDR_0(q)	(0x0c + (q) * 0x100)
#define XRNIC_BUFBASE_ADDR_1(q)	(0x10 + (q) * 0x100)
#define XRNIC_BUF_R_KEY(q)	(0x14 + (q) * 0x100)
#define XRNIC_W_R_BUF_LEN(q)	(0x18 + (q) * 0x100)
#define XRNIC_ACCESS_DESC(q)	(0x1c + (q) * 0x100)

#define ERNIC_V4

#ifdef ERNIC_V4
#define XRNIC_GLOBAL_REG_OFFSET	0x100000
#define XRNIC_RCVQ_INT_STAT_0_31	(XRNIC_GLOBAL_REG_OFFSET + 0x190)
#define XRNIC_COMPQ_INT_STAT_0_31	(XRNIC_GLOBAL_REG_OFFSET + 0x290)
#define XRNIC_CNP_INT_STAT_0_31		(XRNIC_GLOBAL_REG_OFFSET + 0x390)
#define XRNIC_PER_QP_OFFSET	0x180000 //0x180100
#define XRNIC_DB_PA_OFFSET	0x180000 //0x180000
#define XRNIC_UDP_SPORT_SHIFT	8
#define XRNIC_NUM_QP_MASK	0xfff
#define XRNIC_CONF_QP_EN		(XRNIC_GLOBAL_REG_OFFSET + 0x44)
#define XRNIC_SQ_PICI_DB_CHECK_EN	(1 << 16)
#else
#define XRNIC_GLOBAL_REG_OFFSET	0x20000
#define XRNIC_RCVQ_INT_STAT_0_31	(XRNIC_GLOBAL_REG_OFFSET + 0x190)
#define XRNIC_COMPQ_INT_STAT_0_31	(XRNIC_GLOBAL_REG_OFFSET + 0x1b0)
#define XRNIC_CNP_INT_STAT_0_31		(XRNIC_GLOBAL_REG_OFFSET + 0x1D0)
#define XRNIC_PER_QP_OFFSET	0x20200
#define XRNIC_DB_PA_OFFSET	0x20000
#define XRNIC_UDP_SPORT_SHIFT	16
#define XRNIC_NUM_QP_MASK	0xff
#define XRNIC_NUM_QP_SHIFT	8
#define XRNIC_SQ_PICI_DB_CHECK_EN	(1 << 16)
#endif

#define XRNIC_CONF		(XRNIC_GLOBAL_REG_OFFSET + 0x0)
#define XRNIC_EN		BIT(0)
#define XRNIC_UDP_SPORT_MASK	0xffff

#define XRNIC_ADV_CONF		(XRNIC_GLOBAL_REG_OFFSET + 0x4)
#define XRNIC_ROCE_PAUSE_OFFSET	(XRNIC_GLOBAL_REG_OFFSET + 0x8)
#define XRNIC_PAUSE_CONF	(XRNIC_GLOBAL_REG_OFFSET + 0xC)
/* PFC enable & prioirty config */
#define XRNIC_PFC_GLOBAL_PRIOIRTY	8
#define XRNIC_PFC_PRIO_BIT_MASK		0xF
enum {
	XRNIC_ROCE_PFC_EN_BIT = 0,
	XRNIC_NON_ROCE_PFC_EN_BIT = 1,
	XRNIC_ROCE_PFC_PRIO_BIT = 4,
	XRNIC_NON_ROCE_PFC_PRIO_BIT = 8,
	XRNIC_DIS_PRIO_CHECK_BIT = 13,
};
#define XRNIC_MAC_ADDR_LO	(XRNIC_GLOBAL_REG_OFFSET + 0x10)
#define XRNIC_MAC_ADDR_HI	(XRNIC_GLOBAL_REG_OFFSET + 0x14)
#define XRNIC_NON_ROCE_PAUSE_OFFSET 	(XRNIC_GLOBAL_REG_OFFSET + 0x18)

#define XRNIC_IPV6_ADD_1	(XRNIC_GLOBAL_REG_OFFSET + 0x20)
#define XRNIC_IPV6_ADD_2	(XRNIC_GLOBAL_REG_OFFSET + 0x24)
#define XRNIC_IPV6_ADD_3	(XRNIC_GLOBAL_REG_OFFSET + 0x28)
#define XRNIC_IPV6_ADD_4	(XRNIC_GLOBAL_REG_OFFSET + 0x2c)

#define XRNIC_ERR_BUF_BASE_LSB	(XRNIC_GLOBAL_REG_OFFSET + 0x60)
#define XRNIC_ERR_BUF_BASE_MSB	(XRNIC_GLOBAL_REG_OFFSET + 0x64)
#define XRNIC_ERR_BUF_SZ	(XRNIC_GLOBAL_REG_OFFSET + 0x68)
#define XRNIC_ERR_BUF_WR_PTR	(XRNIC_GLOBAL_REG_OFFSET + 0x6c)

#define XRNIC_IPV4_ADDR		(XRNIC_GLOBAL_REG_OFFSET + 0x70)
#define XRNIC_MR_ACC_DESC_RD_WR 0x2

/* Deprecated
#define XRNIC_OUTG_PKT_ERRQ_BASE	0x20078
#define XRNIC_OUTG_PKT_ERRQ_SZ		0x20080
#define XRNIC_OUTG_PKT_ERRQ_WPTR	0x20084
*/
#define XRNIC_INCG_PKT_ERRQ_BASE_LSB	(XRNIC_GLOBAL_REG_OFFSET + 0x88)
#define XRNIC_INCG_PKT_ERRQ_BASE_MSB	(XRNIC_GLOBAL_REG_OFFSET + 0x8C)
#define XRNIC_INCG_PKT_ERRQ_SZ		(XRNIC_GLOBAL_REG_OFFSET + 0x90)
#define XRNIC_INCG_PKT_ERRQ_WPTR	(XRNIC_GLOBAL_REG_OFFSET + 0x94)

#define XRNIC_DATA_BUF_BASE_LSB	(XRNIC_GLOBAL_REG_OFFSET + 0xA0)
#define XRNIC_DATA_BUF_BASE_MSB	(XRNIC_GLOBAL_REG_OFFSET + 0xA4)
#define XRNIC_DATA_BUF_SZ	(XRNIC_GLOBAL_REG_OFFSET + 0xA8)
/* only valid for nvmf use case */
#define XRNIC_CNCT_IO_CONF	(XRNIC_GLOBAL_REG_OFFSET + 0xAC)
#define XRNIC_RSP_ERR_BUF_BA_LSB	(XRNIC_GLOBAL_REG_OFFSET + 0xB0)
#define XRNIC_RSP_ERR_BUF_BA_MSB	(XRNIC_GLOBAL_REG_OFFSET + 0xB4)
#define XRNIC_RSP_ERR_BUF_DEPTH	(XRNIC_GLOBAL_REG_OFFSET + 0xB8)

/* Global status registers */
#define XRNIC_INCG_SND_RDRSP_PKT_CNT	(XRNIC_GLOBAL_REG_OFFSET + 0x100)
#define XRNIC_INCG_ACK_MAD_PKT_CNT	(XRNIC_GLOBAL_REG_OFFSET + 0x104)
#define XRNIC_OUTG_SND_RDWR_PKT_CNT	(XRNIC_GLOBAL_REG_OFFSET + 0x108)
#define XRNIC_OUTG_ACK_MAD_PKT_CNT	(XRNIC_GLOBAL_REG_OFFSET + 0x10c)
#define XRNIC_LST_INCG_PKT		(XRNIC_GLOBAL_REG_OFFSET + 0x110)
#define XRNIC_LST_OUTG_PKT		(XRNIC_GLOBAL_REG_OFFSET + 0x114)
#define XRNIC_INCG_INV_DUP_PKT_CNT	(XRNIC_GLOBAL_REG_OFFSET + 0x118)
#define XRNIC_INCG_NAK_PKT_STAT		(XRNIC_GLOBAL_REG_OFFSET + 0x11c)
#define XRNIC_OUTG_RNR_PKT_STAT		(XRNIC_GLOBAL_REG_OFFSET + 0x120)
#define XRNIC_WQE_PROC_STAT		(XRNIC_GLOBAL_REG_OFFSET + 0x124)
#define XRNIC_QP_MGR_STAT		(XRNIC_GLOBAL_REG_OFFSET + 0x12c)
#define XRNIC_INCG_ALL_DRP_PKT_CNT	(XRNIC_GLOBAL_REG_OFFSET + 0x130)
#define XRNIC_INCG_NAK_PKT_CNT		(XRNIC_GLOBAL_REG_OFFSET + 0x134)
#define XRNIC_OUTG_NAK_PKT_CNT		(XRNIC_GLOBAL_REG_OFFSET + 0x138)
#define XRNIC_RESP_HDLR_STAT		(XRNIC_GLOBAL_REG_OFFSET + 0x13c)
#define XRNIC_RETRY_CNT_STAT		(XRNIC_GLOBAL_REG_OFFSET + 0x140)

#define XRNIC_INT_EN			(XRNIC_GLOBAL_REG_OFFSET + 0x180)
#define XRNIC_INT_STAT			(XRNIC_GLOBAL_REG_OFFSET + 0x184)
  #define XRNIC_INT_PKT_VALID_ERR	BIT(0)
  #define XRNIC_INT_INC_MAD_PKT		BIT(1)
  #define XRNIC_INT_BYPASS_PKT		BIT(2)
  #define XRNIC_INT_RNR_NACK		BIT(3)
  #define XRNIC_INT_WQE_COMP		BIT(4)
  #define XRNIC_INT_ILLEG_OP_SQ		BIT(5)
  #define XRNIC_INT_RQ_PKT_RCVD		BIT(6)
  #define XRNIC_INT_FATAL_ERR_RCVD	BIT(7)
  #define XRNIC_INT_CNP			BIT(8)


/* Per QP registers */
#define XRNIC_QP_CONF(q)		(XRNIC_PER_QP_OFFSET + 0x00 + (q) * 0x100)
  #define QP_ENABLE			BIT(0)
  #define QP_RQ_IRQ_EN			BIT(2)
  #define QP_CQ_IRQ_EN			BIT(3)
  #define QP_HW_HSK_DIS			BIT(4)
  #define QP_CQE_EN			BIT(5)
  #define QP_UNDER_RECOVERY		BIT(6)
  #define QP_CONF_IPV6			BIT(7)
  #define QP_PMTU_SHIFT			8
  #define QP_PMTU_MASK			0x7
  #define QP_PMTU_256			0x0
  #define QP_PMTU_512			0x1
  #define QP_PMTU_1024			0x2
  #define QP_PMTU_2048			0x3
  #define QP_PMTU_4096			0x4
  #define QP_RQ_BUF_SZ_SHIFT		16
  #define QP_RQ_BUF_SZ_MASK		0xffff
#define XRNIC_QP_ADV_CONF(q)		(XRNIC_PER_QP_OFFSET + 0x04 + (q) * 0x100)
  #define QP_ADV_TC_MASK		0x1f
  #define QP_ADV_TTL_MASK		0xff
  #define QP_ADV_TTL_SHIFT		8
  #define QP_ADV_PKEY_MASK		0xffff
  #define QP_ADV_PKEY_SHIFT		16
#define XRNIC_RCVQ_BUF_BASE_LSB(q)	(XRNIC_PER_QP_OFFSET + 0x08 + (q) * 0x100)
#define XRNIC_RCVQ_BUF_BASE_MSB(q)	(XRNIC_PER_QP_OFFSET + 0xC0 + (q) * 0x100)
#define XRNIC_SNDQ_BUF_BASE_LSB(q)	(XRNIC_PER_QP_OFFSET + 0x10 + (q) * 0x100)
#define XRNIC_SNDQ_BUF_BASE_MSB(q)	(XRNIC_PER_QP_OFFSET + 0xC8 + (q) * 0x100)
#define XRNIC_CQ_BUF_BASE_LSB(q)	(XRNIC_PER_QP_OFFSET + 0x18 + (q) * 0x100)
#define XRNIC_CQ_BUF_BASE_MSB(q)	(XRNIC_PER_QP_OFFSET + 0xD0 + (q) * 0x100)
#define XRNIC_RCVQ_WP_DB_ADDR_LSB(q)	(XRNIC_PER_QP_OFFSET + 0x20 + (q) * 0x100)
#define XRNIC_RCVQ_WP_DB_ADDR_MSB(q)	(XRNIC_PER_QP_OFFSET + 0x24 + (q) * 0x100)
#define XRNIC_CQ_DB_ADDR_LSB(q)		(XRNIC_PER_QP_OFFSET + 0x28 + (q) * 0x100)
#define XRNIC_CQ_DB_ADDR_MSB(q)		(XRNIC_PER_QP_OFFSET + 0x2C + (q) * 0x100)
#define XRNIC_CQ_HEAD_PTR(q)		(XRNIC_PER_QP_OFFSET + 0x30 + (q) * 0x100)
  #define XRNIC_CQ_HEAD_PTR_MASK	0xffff
#define XRNIC_RQ_CONS_IDX(q)		(XRNIC_PER_QP_OFFSET + 0x34 + (q) * 0x100)
#define XRNIC_SQ_PROD_IDX(q)		(XRNIC_PER_QP_OFFSET + 0x38 + (q) * 0x100)
#define XRNIC_QUEUE_DEPTH(q)		(XRNIC_PER_QP_OFFSET + 0x3C + (q) * 0x100)
#define XRNIC_SNDQ_PSN(q)		(XRNIC_PER_QP_OFFSET + 0x40 + (q) * 0x100)
#define XRNIC_LAST_RQ_PSN(q)		(XRNIC_PER_QP_OFFSET + 0x44 + (q) * 0x100)
#define XRNIC_DEST_QP_CONF(q)		(XRNIC_PER_QP_OFFSET + 0x48 + (q) * 0x100)
#define XRNIC_TIMEOUT_CONF(q)		(XRNIC_PER_QP_OFFSET + 0x4C + (q) * 0x100)
#define XRNIC_MAC_DEST_ADDR_LO(q)	(XRNIC_PER_QP_OFFSET + 0x50 + (q) * 0x100)
#define XRNIC_MAC_DEST_ADDR_HI(q)	(XRNIC_PER_QP_OFFSET + 0x54 + (q) * 0x100)

#define XRNIC_IP_DEST_ADDR_1(q)		(XRNIC_PER_QP_OFFSET + 0x60 + (q) * 0x100)
#define XRNIC_IP_DEST_ADDR_2(q)		(XRNIC_PER_QP_OFFSET + 0x64 + (q) * 0x100)
#define XRNIC_IP_DEST_ADDR_3(q)		(XRNIC_PER_QP_OFFSET + 0x68 + (q) * 0x100)
#define XRNIC_IP_DEST_ADDR_4(q)		(XRNIC_PER_QP_OFFSET + 0x6C + (q) * 0x100)
#define XRNIC_STAT_SND_SQN(q)		(XRNIC_PER_QP_OFFSET + 0x80 + (q) * 0x100)
#define XRNIC_STAT_MSG_SQN(q)		(XRNIC_PER_QP_OFFSET + 0x84 + (q) * 0x100)
#define XRNIC_STAT_QP(q)		(XRNIC_PER_QP_OFFSET + 0x88 + (q) * 0x100)
#define XRNIC_STAT_CUR_SQ_PTR(q)	(XRNIC_PER_QP_OFFSET + 0x8C + (q) * 0x100)
#define XRNIC_STAT_RESP_PSN(q)		(XRNIC_PER_QP_OFFSET + 0x90 + (q) * 0x100)
#define XRNIC_STAT_RQ_BUF_LSB(q)	(XRNIC_PER_QP_OFFSET + 0x94 + (q) * 0x100)
#define XRNIC_STAT_RQ_BUF_MSB(q)	(XRNIC_PER_QP_OFFSET + 0xD8 + (q) * 0x100)
#define XRNIC_STAT_WQE(q)		(XRNIC_PER_QP_OFFSET + 0x98 + (q) * 0x100)
#define XRNIC_STAT_RQ_PROD_IDX(q)	(XRNIC_PER_QP_OFFSET + 0x9c + (q) * 0x100)
#define XRNIC_QP_PD_NUM(q)		(XRNIC_PER_QP_OFFSET + 0xB0 + (q) * 0x100)

#define XRNIC_OUT_ERR_STAT_BA_LSB(q)	(XRNIC_PER_QP_OFFSET + 0x78 + (q) * 0x100)
#define XRNIC_OUT_ERR_STAT_BA_MSB(q)	(XRNIC_PER_QP_OFFSET + 0x7C + (q) * 0x100)

#define XRNIC_SEND_SGL_SIZE		4096
#define XRNIC_GSI_SQ_DEPTH		128
#define XRNIC_NUM_OF_ERROR_BUF		64
#define XRNIC_SIZE_OF_ERROR_BUF		256
#define XRNIC_RESP_ERR_BUF_SIZE		64
#define XRNIC_RESP_ERR_BUF_DEPTH	256
#define XRNIC_NUM_OF_DATA_BUF		16
#define XRNIC_SIZE_OF_DATA_BUF		4096
#define XRNIC_OUT_ERRST_Q_NUM_ENTRIES	64
#define XRNIC_IN_ERRST_Q_NUM_ENTRIES	64

#define XRNIC_SW_OVER_RIDE_EN		1
#define XRNIC_SW_OVER_RIDE_BIT		(0)

#define PFC_XON_XOFF_MIN		0
#define PFC_XON_XOFF_MAX		512


enum xrnic_wc_opcod {
	XRNIC_RDMA_WRITE = 0x0,
	XRNIC_RDMA_WRITE_WITH_IMM = 0x1,
	XRNIC_SEND_ONLY = 0x2,
	XRNIC_SEND_WITH_IMM = 0x3,
	XRNIC_RDMA_READ = 0x4,
	XRNIC_RDMA_READ_RESP = 0x5,
	XRNIC_OUTGOING_ACK = 0x6,
	XRNIC_SEND_WITH_INV = 0xC,
};

#define XRNIC_INVALID_OPC -1

struct xlnx_ernic_config {
	int dummy;
};

#define XRNIC_SQ_WQE_SIZE	64
#define XRNIC_GSI_RECV_PKT_SIZE 512
#define XRNIC_GSI_RQ_DEPTH	64
#define XRNIC_IN_PKT_ERRQ_DEPTH 64

#define XRNIC_RQ_BUF_SGE_SIZE	256
#define XRNIC_DEF_RQ_DEPTH	16
#define XRNIC_DEF_SQ_DEPTH	32


struct xrnic_local {
	struct xilinx_ib_dev		*xib;
	struct platform_device		*pdev;
	u8 __iomem			*reg_base;
	int				irq;
	u64				qp1_sq_db_p;
	u32				*qp1_sq_db_v;
	u64				qp1_rq_db_p;
	u32				*qp1_rq_db_v;
	int				qps_enabled;
	u16				udp_sport;
	dma_addr_t			db_pa;
	u32				db_size;
	u8 __iomem			*ext_hh_base;
	u8 __iomem			*hw_hsk_base;
	u8 __iomem			*qp_hh_base;
	dma_addr_t			in_pkt_err_ba;
	dma_addr_t			retry_buf_pa;
	void				*in_pkt_err_va;
	void				*retry_buf_va;
	u32				in_pkt_err_db_local;
	u32				db_chunk_id;
	u32				num_intr_banks;
	u32				cq_rq_db_size;
};

union ctx
{
	__u16 context;
	__u16 wr_id;
}__attribute__((packed));


/* Work request 64Byte size */
#define XRNIC_WR_ID_MASK	0xffff
#define XRNIC_OPCODE_MASK	0xff
struct xrnic_wr
{
	u32	wrid;
	u64	l_addr;
	u32	length;
	u8	opcode;
	u16	reserved1;
	u8	par_wqe_indi;
	u64	r_offset;
	u32	r_tag;
#define XRNIC_MAX_SDATA		16
	u8	sdata[XRNIC_MAX_SDATA];
	u32	imm_data;
	u32	start_psn_val;
	u32	aeth_header;
	u32	reserved2;
}__attribute__((packed));

#define XRNIC_CQE_WRID_MASK 0xff
#define XRNIC_CQE_WRID_SHIFT 0
#define XRNIC_CQE_OPCODE_MASK 0xff0000
#define XRNIC_CQE_OPCODE_SHIFT 16
#define XRNIC_CQE_ERR_MASK 0xff000000
#define XRNIC_CQE_ERR_SHIFT 24

struct xrnic_cqe
{
	u32 entry;
};

static inline void xrnic_iow32(u8 __iomem *base, off_t offset, u32 value)
{
	iowrite32(value, base + offset);
	wmb();
}
static inline u32 xrnic_ior32(u8 __iomem *base, off_t offset)
{
	u32 val;
	val = ioread32(base + offset);
	rmb();
	return val;
}

static inline void xrnic_iow(struct xrnic_local *xl, off_t offset, u32 value)
{
	iowrite32(value, (xl->reg_base + offset));
}

static inline u32 xrnic_ior(struct xrnic_local *xl, off_t offset)
{
	return ioread32( (xl->reg_base + offset));
}

u64 xrnic_get_sq_db_addr(struct xrnic_local *xl, int hw_qpn);
u64 xrnic_get_rq_db_addr(struct xrnic_local *xl, int hw_qpn);
struct xrnic_local *xrnic_hw_init(struct platform_device *pdev, struct xilinx_ib_dev *xib);
void xrnic_hw_deinit(struct xilinx_ib_dev *xib);
void xrnic_set_mac(struct xrnic_local *xl, u8 *mac);
int xrnic_start(struct xrnic_local *xl);
void config_raw_ip(struct xrnic_local *xl, u32 base, u32 *ip, bool is_ipv6);
int xrnic_qp_set_pd(struct xilinx_ib_dev *xib, int qpn, int pdn);
int xrnic_set_pd(struct xilinx_ib_dev *xib, int pdn);
int xrnic_reg_mr(struct xilinx_ib_dev *xib, u64 va, u64 len,
		u64 *pbl_tbl, int umem_pgs, int pdn, u32 mr_idx, u8 rkey);
int xrnic_unreg_mr(struct xilinx_ib_dev *xib, u32 data);
dma_addr_t xrnic_buf_alloc(struct xrnic_local *xl, u32 size, u32 count);
void qp_set_ipv6_destination(struct xrnic_local *xib, int qpn, u8* raw_ipv6);
int xrnic_qp_under_recovery(struct xilinx_ib_dev *xib, int hw_qpn);
#endif /* _XRNIC_H_ */
