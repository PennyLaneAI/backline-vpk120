/*
 * Copyright (C) 2024 AMD, Inc. All rights reserved.
 *
 * Author : Anjaneyulu Reddy Mule <anjaneyu@xilinx.com>
 *
 * This software is available to you under a choice of one of two
 * licenses.  You may choose to be licensed under the terms of the GNU
 * General Public License (GPL) Version 2, available from the file
 * COPYING in the main directory of this source tree, or the
 * OpenIB.org BSD license below:
 *
 *     Redistribution and use in source and binary forms, with or
 *     without modification, are permitted provided that the following
 *     conditions are met:
 *
 *      - Redistributions of source code must retain the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer.
 *
 *      - Redistributions in binary form must reproduce the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer in the documentation and/or other materials
 *        provided with the distribution.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */
#ifndef XPERF_H
#define XPERF_H

#define RPING_QP_CONNECTION_CNT	255
#define	RDMA_TEST_QP_CNT	1
#define	RDMA_LOOP_CNT		1
#define RPING_SEND_SIZE	(4096)
#define RDMA_MIN_BUF_SIZE	64

#define MAX_WQE_DEPTH	16
#define IMPL_HW_SHK	1
#define NO_POST_RECV
#define HW_HSK_SEND_OP_CODE 2
#define HH_MAX_QUEUE_DEPTH	16
#define HH_CQ_RQ_DEPTH	(8 * 1024 + 32)
#define HH_RX_REPOST_WQE_CNT	256
#define BRAM_MEM_BASE	0xB0000000
#define HH_DEV_NAME	"/dev/xib0"


struct per_qp_hw_hsk_reg_info {
	unsigned int qp_num;
        unsigned int data_pattern;
        unsigned int rkey;
        unsigned int va_lsb_val;
        unsigned int va_msb_val;
        uint64_t data_addr;
        uint64_t sq_addr;
	unsigned short data_size_kb;
	unsigned short wqe_opcode;
};

enum hw_hsk_mod_fn {
        CMN_CFG_OPC,
        QP_CFG_OPC,
        EN_HW_HS_OPC,
        RST_HW_HS_OPC,
        PERF_READ_OPC,
        TEST_DONE_READ_OPC,
        MAX_HW_HSK_FNS_OPC,
};

struct hw_hsk_ctrl_cfg {
        unsigned char hw_hs_en;
        unsigned char burst_mode;
        unsigned char db_burst_cnt;
        unsigned int qp_cnt;
        unsigned char data_pattern_en;
    /*unsigned char wqe_opcode;*/
        unsigned short queue_depth;
    /*unsigned short data_size_kb;*/
        unsigned int qp_wqe_cnt;
};

struct mem_mgr {
	uint64_t	mem_base_p;
	uint32_t	size;
	uint32_t	alloc_size;
	void		*va;
} bram_mem_mgr;

enum test_state {
	IDLE = 1,
	CONNECT_REQUEST,
	ADDR_RESOLVED,
	ROUTE_RESOLVED,
	CONNECTED,
	RDMA_READ_ADV, //6
	RDMA_READ_COMPLETE,
	RDMA_WRITE_ADV,
	RDMA_WRITE_COMPLETE,//9
	DISCONNECTED,
	HW_HS_RECV_CMPLT,
	ERROR
};

struct rping_rdma_info {
	__be64 buf;
	__be32 rkey;
	__be32 size;
	__be32 iter_cnt;
	__be32 qp_cnt;
};

struct hh_send_fmt {
	int	tf_siz;
	int	opcode;
	int	qp_cnt;
	int	wqe_cnt;
};

enum operation {
	ERNIC_INCOMING=1, /* incoming test only */
	ERNIC_OUTGOING,   /* outgoing test only */
	ERNIC_IO,         /* Both incoming and outgoing */
};

struct hh_send_dfmt {
        int     tf_siz;
        int     opcode;
        int     qp_cnt;
	int	wqe_cnt;
};


struct rdma_param {
	uint8_t	 help, data_pattern_en, sq_mem_type, rdma_buf_mem_type;
	uint8_t	block_reset, block_test, wqe_opcode, burst_mode;
	uint32_t data_pattern, data_size_kb, qp_cnt, queue_depth, db_burst_cnt;
	uint32_t qp_wqe_cnt, cpu_clk, port;
	struct sockaddr_storage sin, ssource;
};

enum rping_cm_states {
	CONNECT_REQ,
	CONNECT_REQ_PROC,
	ESTABLISHED,
	READY_FOR_DATA_TRANSFER,
	QP_CFG_DONE,
	INVALID_STATE
};

enum {
	HH_WRITE_OP = 0,
	HH_READ_OP,
	HH_SEND_OP,
};

struct app_context {
	struct rdma_cm_id* cm_id;
	struct rdma_event_channel *cm_channel;
/* rdma_rd_wr_buf is used for incoming RDMA operations w.r.t x86 (or)
 * outgoing RDMA operations w.r.t ERNIC.
 * This buffer is registered for both Remote Read and Write.
 * ERNIC will Write and Read to this buffer and will compare the result and
 * sends the result in next RDMA_SEND. This will be captured in recv_buffer. */
	struct ibv_cq* cq;
	struct ibv_comp_channel* comp_chan;
	struct rdma_param* param;
	struct ibv_recv_wr rq_wr;
	struct ibv_sge recv_sgl;	/* recv single SGE */
	struct ibv_mr *rping_recv_mr;		/* MR associated with this buffer */
	struct rping_rdma_info *rping_recv_buf;/* malloc'd buffer */

	struct ibv_send_wr sq_wr;	/* send work request record */
	struct ibv_sge send_sgl;
	struct hh_send_fmt *rping_send_buf;/* single send buf */
	struct ibv_mr *rping_send_mr;

	struct ibv_send_wr rdma_sq_wr;	/* rdma work request record */
	struct ibv_sge rdma_sgl;	/* rdma single SGE */
	char *rdma_buf;			/* used as rdma sink */
	struct ibv_mr *rping_rdma_mr;

	uint32_t remote_rkey;		/* remote guys RKEY */
	uint64_t remote_addr;		/* remote guys TO */
	uint32_t remote_len;		/* remote guys LEN */
	uint32_t iter_cnt;		/* Iteration count */
	struct ibv_mr *hw_hsk_mr;
	char *hw_hs_va;
	int hh_mem_alloc;
	volatile uint32_t	rx_compl, rx_posted;
	uint32_t hh_data_size, hh_free_rx_wqe;
	struct ibv_wc wc;
	volatile int rx_wqe_ready, rq_depth;
	int qp_num;
	struct ibv_qp *qp;
	int rdma_buf_chunk_id, send_buf_chunk_id, sq_mem_chunk_id;
	uint64_t rdma_buf_addr, send_buf_addr, sq_addr;
};

struct thread_info {    /* Used as argument to thread_start() */
	pthread_t thread_id;        /* ID returned by pthread_create() */
	int       thread_num;       /* Application-defined thread # */
};

struct ctx_list {
	struct app_context ctx;
	struct rdma_cm_id* client_cm_id;
	volatile enum rping_cm_states state;
	struct ctx_list *next;
};

struct rping_test {
	struct ctx_list *head, *prev;
	pthread_t	cm_thread, establish_thread;
	volatile uint32_t  connections, disconnects;
        struct rdma_cm_id* cm_id;
        struct rdma_event_channel *cm_channel;
	pthread_mutex_t cm_list_lock;
	int hh_opcode;
	volatile int rx_test_qp_cnt;
	int qp_cnt, hh_en, test_qp_cnt, wqe_cnt;;
	struct sockaddr_storage sin, ssource;
	struct rdma_param param;
	int dev_fd;
};

struct perf_read_str {
	uint32_t qp_num;
	uint32_t cq_cnt;
	uint64_t clk_cnt;
};

enum {
	LOG_LEVEL1 = 1,
	LOG_LEVEL2,
	LOG_LEVEL3,
};

#define HH_MAGIC_NUM	'H'

#define HH_WRITE_CMN_CFG	_IOW(HH_MAGIC_NUM, CMN_CFG_OPC, struct hw_hsk_ctrl_cfg)
#define HH_WRITE_QP_CFG		_IOW(HH_MAGIC_NUM, QP_CFG_OPC, struct per_qp_hw_hsk_reg_info)
#define HH_EN_OP		_IO(HH_MAGIC_NUM, EN_HW_HS_OPC)
#define HH_RESET_OP		_IO(HH_MAGIC_NUM, RST_HW_HS_OPC)
#define PERF_READ_OP		_IOWR(HH_MAGIC_NUM, PERF_READ_OPC, struct perf_read_str)
#define TEST_DONE_READ_OP	_IOR(HH_MAGIC_NUM, TEST_DONE_READ_OPC, unsigned int)
#endif
