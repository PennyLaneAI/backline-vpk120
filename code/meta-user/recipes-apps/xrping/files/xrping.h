/*
 * Copyright (c) 2005 Ammasso, Inc. All rights reserved.
 * Copyright (c) 2006 Open Grid Computing, Inc. All rights reserved.
 * Copyright (c) 2020 Xilinx, Inc. All rights reserved.
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
#ifndef _XRPING_H
#define _XRPING_H

#define XIB_ERNIC
#define XIB_DMA_MEM_ALLOC_FIX

#define XIB_MEM_ALIGN	4096
#if (XIB_MEM_ALIGN == 64)
	#define XIB_BIT_SHIFT 6
#elif (XIB_MEM_ALIGN == 4096)
	#define XIB_BIT_SHIFT 12
#endif

#define PAYLOAD_SIZE (4096)
#define ERNIC_SGE_SIZE (256)

/*
 * rping "ping/pong" loop:
 * 	client sends source rkey/addr/len
 *	server receives source rkey/add/len
 *	server rdma reads "ping" data from source
 * 	server sends "go ahead" on rdma read completion
 *	client sends sink rkey/addr/len
 * 	server receives sink rkey/addr/len
 * 	server rdma writes "pong" data to sink
 * 	server sends "go ahead" on rdma write completion
 * 	<repeat loop>
 */

/*
 * These states are used to signal events between the completion handler
 * and the main client or server thread.
 *
 * Once CONNECTED, they cycle through RDMA_READ_ADV, RDMA_WRITE_ADV, 
 * and RDMA_WRITE_COMPLETE for each ping.
 */
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

/*
 * Default max buffer size for IO...
 */
#define RPING_BUFSIZE 64*1024
#define RPING_SQ_DEPTH 16

/* Default string for print data and
 * minimum buffer size
 */
#define _stringify( _x ) # _x
#define stringify( _x ) _stringify( _x )

#define RPING_MSG_FMT           "rdma-ping-%d: "
#define RPING_MIN_BUFSIZE       sizeof(stringify(INT_MAX)) + sizeof(RPING_MSG_FMT)

/*
 * Control block struct.
 */
struct rping_cb {
	pthread_t cqthread;
	pthread_t persistent_server_thread;
	struct ibv_comp_channel *channel;
	struct ibv_cq *cq;
	struct ibv_pd *pd;
	struct ibv_qp *qp;

	struct ibv_recv_wr rq_wr;	/* recv work request record */
	struct ibv_sge recv_sgl;	/* recv single SGE */
	uint64_t recv_buf;/* malloc'd buffer */
	struct ibv_mr *recv_mr;		/* MR associated with this buffer */

	struct ibv_send_wr sq_wr;	/* send work request record */
	struct ibv_sge send_sgl;
	uint64_t send_buf;/* single send buf */
	struct ibv_mr *send_mr;

	struct ibv_send_wr rdma_sq_wr;	/* rdma work request record */
	struct ibv_sge rdma_sgl;	/* rdma single SGE */
	uint64_t rdma_buf;			/* used as rdma sink */
	struct ibv_mr *rdma_mr;

	uint32_t remote_rkey;		/* remote guys RKEY */
	uint64_t remote_addr;		/* remote guys TO */
	uint32_t remote_len;		/* remote guys LEN */

	char *start_buf;		/* rdma read src */
	struct ibv_mr *start_mr;

	enum test_state state;		/* used for cond/signalling */
	sem_t sem;
	sem_t sem_data;
        uint32_t iter_cnt;              /* Iteration count */


	/* CM stuff */
	struct rdma_event_channel *cm_channel;
	struct rdma_cm_id *cm_id;	/* connection on client side,*/
					/* listener on service side. */
#ifdef XIB_ERNIC
	char *rdma_buf_ofs;
	pthread_mutex_t	cm_finish_lock;
	int count;
	struct rdma_cm_event *event;
	volatile int cm_fail, cq_fail, cm_chnl_en, cm_established, test_done;
	volatile int poll_pkt_cnt;
#endif
	unsigned int qp_num;
	int rdma_buf_chunk_id, ctx_buf_chunk_id;
};

struct rping_test_info {
	struct rping_cb	*cb;
        struct ctx_list *head, *prev;
	pthread_t cmthread;
	pthread_t cqthread;
	pthread_t datathread;
        volatile uint32_t  connections, disconnects;
        struct rdma_cm_id* cm_id;
        struct rdma_event_channel *cm_channel;
	struct ibv_comp_channel *channel;
	struct ibv_pd *pd;
        pthread_mutex_t cm_list_lock;
        volatile int rx_test_qp_cnt;
        int qp_cnt, test_qp_cnt, wqe_cnt;;
	int	use_format;
	int	server;
	int	size;
	int	count;
	int	verbose;
	int	validate;
	unsigned int	cpu_clk;
	unsigned int q_depth;
	struct sockaddr_storage sin, ssource;
	__be16 port;			/* dst port in NBO */
	int	cq_init;
	volatile int done_cnt;
	unsigned int use_umm;
	int	test_mul_mr_reg;
	volatile unsigned int low_proc_qp_num, low_cm_est_qp_num, cm_en_cnt;
} rping_test;

enum rping_cm_states {
        CONNECT_REQ,
        CONNECT_REQ_PROC,
        ESTABLISHED,
        READY_FOR_DATA_TRANSFER,
        INVALID_STATE
};

struct ctx_list {
        struct rping_cb ctx;
        struct rdma_cm_id* client_cm_id;
        volatile enum rping_cm_states state;
        struct ctx_list *next;
};

#endif
