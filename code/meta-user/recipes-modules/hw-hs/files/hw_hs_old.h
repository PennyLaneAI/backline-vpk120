// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * AMD ERNIC hardware handshake driver
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
#ifndef _HW_HAND_SHAKE_H
#define _HW_HAND_SHAKE_H

/* the space is < 32 Bytes. Making it 256B for future purpose */
#define HS_HS_COMMON_CFG_SIZE	(256)
#define HW_HS_QP_CFG_SIZE	(64)

#define XRNIC_HW_HNDSHK_BASE	0x44A00000
#define HW_HNDSHK_QP_CFG_BASE	(0x20000)

#define HH_CLR_STAT_REG_VAL	1
#define HW_HS_CFG_REG_OFS	(0)
#define HW_HS_TEST_STAT_OFS	(HW_HS_CFG_REG_OFS + 4)
#define HW_HS_DATA_TF_SIZE_OFS	(HW_HS_TEST_STAT_OFS + 4)
#define HW_HS_Q_DEPTH_OFS	(HW_HS_DATA_TF_SIZE_OFS + 4)
#define HW_HS_WQE_CNT_OFS	(HW_HS_Q_DEPTH_OFS + 4)
//#define HW_HS_WQE_OPCODE_OFS	(HW_HS_WQE_CNT_OFS + 4)

#define HW_HS_DATA_PTRN_OFS(q)		((q) * HW_HS_QP_CFG_SIZE)
#define HW_HS_DATA_BA_OFS_LSB(q)	((q) * HW_HS_QP_CFG_SIZE + 4)
#define HW_HS_DATA_BA_OFS_MSB(q)	((q) * HW_HS_QP_CFG_SIZE + 8)
#define HW_HS_QP_RKEY(q)		((q) * HW_HS_QP_CFG_SIZE + 12)
#define HW_HS_QP_VA_LSB(q)		((q) * HW_HS_QP_CFG_SIZE + 16)
#define HW_HS_QP_VA_MSB(q)		((q) * HW_HS_QP_CFG_SIZE + 20)
#define HW_HS_QP_PERF_CNT_LSB(q)	((q) * HW_HS_QP_CFG_SIZE + 24)
#define HW_HS_QP_PERF_CNT_MSB(q)        ((q) * HW_HS_QP_CFG_SIZE + 28)
#define HW_HS_QP_PERF_BW(q)		((q) * HW_HS_QP_CFG_SIZE + 32)
#define HW_HS_QP_WQE_BA_LSB(q)		((q) * HW_HS_QP_CFG_SIZE + 36)
#define HW_HS_QP_WQE_BA_MSB(q)		((q) * HW_HS_QP_CFG_SIZE + 40)
#define HW_HS_WQE_OPCODE_OFS(q)		((q) * HW_HS_QP_CFG_SIZE + 44)

#define BURST_MODE_EN_VAL	0
#define INLINE_MODE_EN_VAL	1
#define QP_CNT_BITS_CNT		11
#define DB_BURST_CNT_BIT_CNT	4
#define MIN_DATA_SIZ_IN_KB	4
#define MAX_DATA_SIZ_IN_KB	(2 * 1024)
#define VALID_DATA_TF_SIZ_BIT_CNT 4
#define MAX_WQE_DEPTH		16
#define HW_HS_OPCODE_BIT_CNT	2

#define ADDRW_32BIT	32
#define ADDRW_64BIT	64

enum {
	HH_EN_STATE,
	HH_DIS_STATE,
};

struct per_qp_hw_hsk_reg_info {
	u32 qp_num;
        u32 data_pattern;
        u32 rkey;
        u32 va_lsb_val;
        u32 va_msb_val;
	u64 data_addr;
	u64 sq_addr;
	u16 data_size_kb;
	u16 wqe_opcode;
};

enum {
	HW_HS_EN_BIT_POS = 0,
	HW_HS_MODE_BIT_POS,
	DB_BURSTS_CNT_BIT_POS,
	QPS_EN_BIT_POS = 6,
	DATA_PTRN_EN_BIT_POS = 14,
};


struct hw_hsk_ctrl_cfg {
	u8 hw_hs_en;
        u8 burst_mode;
        u8 db_burst_cnt;
        u32 qp_cnt;
        u8 data_pattern_en;
    //u8 wqe_opcode; /*now in per_qp_hw_hsk_reg_info*/
	u16 queue_depth;
    //u16 data_size_kb; /*now in per_qp_hw_hsk_reg_info*/
	u32 qp_wqe_cnt;
};

struct hh_mem_info {
	u64 base_addr;
	u64 tot_mem;
	u64 used_mem;
};

struct qp_info {
	u64	data_ba_p;
	u64	sq_ba_p;
};

struct hh_cfg_info {
        u64 base;
        u64 size;
};

struct hw_hs_info {
	struct hh_mem_info	mem_info;
	struct hh_cfg_info	cfg;
	struct hw_hsk_ctrl_cfg	ctrl_cfg;
	u32		def_per_qp_data_siz;
	u32		perf_read_qp;
	u32		data_ba_rx;
	u32		data_size;
	u64		data_ba;
};

struct hw_hs_dev {
	struct hw_hs_info hh_info;
	u8 __iomem	*ext_clk_reg_base;
	u8 __iomem	*cmn_cfg_base;
	u8 __iomem	*qp_cfg_base;
	struct class	*cl;
	struct cdev 	*cdevice;
	struct mutex	mutx;
	struct device	*dev;
	struct qp_info	*qp_info;
	dev_t		dev_num;
	u32		addr_width;
	u32		num_qp;
};

#ifndef MAX_VENDOR_SPEC_RESP
	#define MAX_VENDOR_SPEC_RESP 32
#endif
struct vendor_read_resp {
        __u32 data_len;
        __u8 resp_data[MAX_VENDOR_SPEC_RESP];
};

struct reg_wr_info {
        unsigned int reg_ofs;
        unsigned int val;
};

struct wr_data_format {
        union {
                unsigned int data_len;
                unsigned int cnt;
        };
        union {
                struct  reg_wr_info reg_info[10];
                void    *impl_spec_data;
        };
};

struct perf_read_str {
	u32	qp_num;
	u32	cq_cnt;
	u64	clk_cnt;
};

enum mod_code {
        HW_HSK_MODULE = 0,
        MAX_NO_OF_XRNIC_MODULES,
};

enum hw_hsk_mod_fn {
	CMN_CFG_OPC,
	QP_CFG_OPC,
	EN_HW_HS_OPC,
	RST_HW_HS_OPS,
	PERF_READ_OPC,
        TEST_DONE_READ,
        MAX_HW_HSK_FNS,
};
#endif
