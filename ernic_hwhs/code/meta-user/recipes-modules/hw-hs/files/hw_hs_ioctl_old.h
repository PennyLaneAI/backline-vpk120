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
#ifndef HH_IOCTL_
#define HH_IOCTL_

#define HH_MAGIC_NUM	'H'

#define HH_WRITE_CMN_CFG	_IOW(HH_MAGIC_NUM, CMN_CFG_OPC, struct hw_hsk_ctrl_cfg)
#define HH_WRITE_QP_CFG		_IOW(HH_MAGIC_NUM, QP_CFG_OPC, struct per_qp_hw_hsk_reg_info)
#define HH_EN_OP		_IO(HH_MAGIC_NUM, EN_HW_HS_OPC)
#define HH_RESET_OP		_IO(HH_MAGIC_NUM, RST_HW_HS_OPS)
#define PERF_READ_OP		_IOWR(HH_MAGIC_NUM, PERF_READ_OPC, struct perf_read_str)
#define TEST_DONE_READ_OP	_IOR(HH_MAGIC_NUM, TEST_DONE_READ, unsigned int)

#endif
