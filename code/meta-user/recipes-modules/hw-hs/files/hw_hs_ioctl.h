// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * AMD ERNIC hardware handshake driver
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
#ifndef HH_IOCTL_
#define HH_IOCTL_

#define HH_MAGIC_NUM 'H'

#define HH_CONFIG _IOW(HH_MAGIC_NUM, HH_FN_CONFIG, struct hh_round_cfg)
#define HH_WQE_COMMIT _IOW(HH_MAGIC_NUM, HH_FN_WQE, struct hh_wqe_cfg)
#define HH_START _IO(HH_MAGIC_NUM, HH_FN_START)
#define HH_ABORT _IO(HH_MAGIC_NUM, HH_FN_ABORT)
#define HH_RESET _IO(HH_MAGIC_NUM, HH_FN_RESET)
#define HH_READ_STATUS _IOR(HH_MAGIC_NUM, HH_FN_READ_STATUS, struct hh_status_rd)
#define HH_SET_PATTERN _IOW(HH_MAGIC_NUM, HH_FN_SET_PATTERN, __u32)
#define HH_SET_WQE_IDX _IOW(HH_MAGIC_NUM, HH_FN_SET_WQE_IDX, __u32)
#define HH_QPCTX_COMMIT _IOW(HH_MAGIC_NUM, HH_FN_QPCTX_COMMIT, struct hh_qpctx_cfg)
#define HH_MR_REBASE _IOW(HH_MAGIC_NUM, HH_FN_MR_REBASE, struct hh_mr_rebase)

// transport demo
#define HH_DEMO_CFG _IOW(HH_MAGIC_NUM, HH_FN_DEMO_CFG, struct hh_demo_cfg)
#define HH_DEMO_START_RUN _IO(HH_MAGIC_NUM, HH_FN_DEMO_START)
#define HH_DEMO_STOP_RUN _IO(HH_MAGIC_NUM, HH_FN_DEMO_STOP)
#define HH_DEMO_STATUS _IOR(HH_MAGIC_NUM, HH_FN_DEMO_STATUS, struct hh_demo_status)
#define HH_TRACE_ARM _IO(HH_MAGIC_NUM, HH_FN_TRACE_ARM)
#define HH_TRACE_STATUS _IOR(HH_MAGIC_NUM, HH_FN_TRACE_STATUS, struct hh_trace_status)

#endif
