/*
 * Copyright (C) 2024 AMD, Inc. All rights reserved.
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
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <rdma/rdma_cma.h>
#include <pthread.h>
#include <pthread.h>
#include <unistd.h>
#include <errno.h>
#include <ctype.h>
#include <getopt.h>
#include <libgen.h>
#include <rdma/rdma_verbs.h>
#include <umm_export.h>

#define _GNU_SOURCE

unsigned int debug = 0;
#define DEBUG_LOG if (debug) printf
unsigned int mem_type = XMEM_PL_DDR;

#define RPING_QP_CONNECTION_CNT	255
#define	RDMA_TEST_QP_CNT	1
#define	RDMA_LOOP_CNT		1
#define RDMA_MIN_BUF_SIZE	64

#define IMPL_HW_SHK	1
#define NO_POST_RECV
#define HW_HSK_SEND_OP_CODE 2
#define HH_MAX_QUEUE_DEPTH	16
#define HH_CQ_RQ_DEPTH		HH_MAX_QUEUE_DEPTH
#define HH_RX_REPOST_WQE_CNT	256

#define PAYLOAD_SIZE (4096)
#define ERNIC_SGE_SIZE (256)

/* Informed Errors look ugly when more than 80 characters long */
#define	error_handler(x)	do {					\
					char str[100];			\
					sprintf(str, "[0;31m%s",x);	\
					perror(str);			\
					printf("[0;m");		\
					exit(EXIT_FAILURE);		\
				} while(0);
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
	uint8_t	 server;
	uint8_t  cm_test;
	uint8_t	 operation;
	uint8_t	 help;
        char*	 server_addr;
	uint32_t qp_count;
	uint32_t iter_count;
	uint32_t random;
	uint32_t file;
	char* va_err;
	char* rkey_err;
};

struct rping_rdma_info {
	uint64_t buf;
	uint32_t rkey;
	uint32_t size;
	uint32_t iter_cnt;
	uint32_t qp_cnt;
};

enum rping_cm_states {
	CONNECT_REQ,
	CONNECT_REQ_PROC,
	ESTABLISHED,
	READY_FOR_DATA_TRANSFER,
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
	uint8_t rping_recv_buf[PAYLOAD_SIZE];/* malloc'd buffer */

	struct ibv_send_wr sq_wr;	/* send work request record */
	struct ibv_sge send_sgl;
	struct rping_rdma_info *rping_send_buf;/* single send buf */
	struct ibv_mr *rping_send_mr;

	struct ibv_send_wr rdma_sq_wr;	/* rdma work request record */
	struct ibv_sge rdma_sgl;	/* rdma single SGE */
	uint64_t rdma_buf;			/* used as rdma sink */
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
	struct ibv_wc *wc;
	volatile int rx_wqe_ready, rq_depth;
	unsigned int send_buf_chunk_id, rdma_buf_chunk_id;
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
	int qp_cnt, test_qp_cnt, wqe_cnt, q_depth;
};

struct rping_test hrping_test;

#if 0
__attribute__((destructor)) void destruct_rping(void)
{
	int i;
	struct ctx_list *list = hrping_test.head, *temp;

	return;
	}
}
#endif

void rdmatest_poll_cq(struct app_context* rdma_ctx, unsigned id)
{
	struct ibv_wc wc;
	void* cq_ctx;
	struct ibv_cq* cq_event;

	while (1) {
#if 0
		if (ibv_get_cq_event(rdma_ctx->comp_chan,&cq_event, &cq_ctx))
			return ;
		if (ibv_req_notify_cq(rdma_ctx->cq, 0))
			return ;
#endif
		if (ibv_poll_cq(rdma_ctx->cq, 1, &wc) != 1)
			return ;
		if (wc.status != IBV_WC_SUCCESS)
		{
			printf("ernic wc status = %d(%s), id = %d:%d\n",
					wc.status,
					ibv_wc_status_str(wc.status),
					wc.wr_id, id);
			return ;
		} else {
			if (wc.wc_flags & IBV_WC_WITH_INV)
				printf("invalidated rkey : 0x%x\n", wc.invalidated_rkey);
			if (wc.wc_flags & IBV_WC_WITH_IMM)
				printf("immediate data : 0x%x\n", wc.imm_data);
		}

		if (wc.wr_id == id)
			break;
		memset(&wc, 0, sizeof(struct ibv_wc));
	}
}

int init_server_cm(struct rping_test* hrping)
{
	int err;
	struct sockaddr_in sin4_addr;
	struct rdma_cm_event *event;

	sin4_addr.sin_family = AF_INET;
	sin4_addr.sin_port = htons(7179);
	sin4_addr.sin_addr.s_addr = INADDR_ANY;

	if (rdma_bind_addr(hrping->cm_id, (struct sockaddr*)&sin4_addr))
		error_handler("RDMA BIND Failed:");

	if (rdma_listen(hrping->cm_id, RPING_QP_CONNECTION_CNT))
		error_handler("RDMA Listen Failed:");

	return 0;
}

int app_req_regions(struct app_context* rdma_ctx)
{
	int err;
	struct rdma_cm_id* cm_id;

	cm_id = rdma_ctx->cm_id;
	cm_id->pd = ibv_alloc_pd(cm_id->verbs);
	if (!cm_id->pd)
		error_handler("alloc_pd Failed:");

	rdma_ctx->comp_chan = ibv_create_comp_channel(cm_id->verbs);
	if (!rdma_ctx->comp_chan)
		error_handler("create_comp_channel Failed:");

	rdma_ctx->cq = ibv_create_cq(cm_id->verbs, hrping_test.q_depth, NULL,
				     rdma_ctx->comp_chan, 0);
	if (!rdma_ctx->cq)
		error_handler("create_cq Failed:");

	rdma_ctx->rq_depth = hrping_test.q_depth;
	err = ibv_req_notify_cq(rdma_ctx->cq, 0);
	if (err)
		error_handler("req_notify_cq Failed:");

	return 0;
}

int rping_setup_wr(struct app_context *cb)
{
        cb->recv_sgl.addr = (uint64_t) (unsigned long) cb->rping_recv_buf;
        cb->recv_sgl.length = sizeof cb->rping_recv_buf;
        cb->rq_wr.sg_list = &cb->recv_sgl;
        cb->rq_wr.num_sge = 1;

        cb->send_sgl.addr = (uint64_t) (unsigned long) cb->rping_send_buf;
        cb->send_sgl.length = sizeof (*cb->rping_send_buf);

        cb->send_sgl.lkey = cb->send_buf_chunk_id;

        cb->sq_wr.opcode = IBV_WR_SEND;
        cb->sq_wr.send_flags = IBV_SEND_SIGNALED;
        cb->sq_wr.sg_list = &cb->send_sgl;
        cb->sq_wr.num_sge = 1;

        /* as a fix, updated lkey with physical address. ERNIC V1.0
        doesn't use LKEY */
        cb->rdma_sgl.addr = cb->rdma_buf;
        cb->rdma_sgl.lkey = cb->rping_rdma_mr->lkey;
        cb->rdma_sq_wr.send_flags = IBV_SEND_SIGNALED;
        cb->rdma_sq_wr.sg_list = &cb->rdma_sgl;
        cb->rdma_sq_wr.num_sge = 1;
}

static int rping_setup_buffers(struct app_context *cb)
{
	int ret;
	unsigned int data_size = sizeof(*cb->rping_send_buf), chunk_id;
	uint64_t va;
	bool proc_access = true;

	chunk_id = xib_umem_alloc_chunk(cb,
			XMEM_PL_DDR, data_size, data_size,
			proc_access);

	if (chunk_id < 0) {
		printf("Failed to alloc chunk %d\n", __LINE__);
		return -ENOMEM;
        }

	cb->send_buf_chunk_id = chunk_id;

        va = xib_umem_alloc_mem(cb, chunk_id, data_size);
        if (XMEM_IS_INVALID_VADDR(va)) {
                printf("Failed to alloc memory from chunk\n");
		ret |= xib_umem_free_chunk(cb, cb->send_buf_chunk_id);
		cb->send_buf_chunk_id = -EINVAL;
                return -ENOMEM;
        }
	cb->rping_send_buf = va;

	data_size = (4 * 1024);
	chunk_id = xib_umem_alloc_chunk(cb,
			mem_type, data_size, data_size,
			proc_access);

	if (chunk_id < 0) {
		printf("Failed to alloc chunk %d\n", __LINE__);
		goto err1;
        }

	cb->rdma_buf_chunk_id = chunk_id;
        va = xib_umem_alloc_mem(cb, chunk_id, data_size);
        if (XMEM_IS_INVALID_VADDR(va)) {
                printf("Failed to alloc memory from chunk\n");
		ret |= xib_umem_free_chunk(cb, cb->rdma_buf_chunk_id);
		cb->rdma_buf_chunk_id = XMEM_INVALID_CHUNK_ID;
		goto err1;
        }
	cb->rdma_buf = va;

	cb->rping_rdma_mr = ibv_reg_mr_ex(cb->cm_id->pd, cb->rdma_buf, data_size,
				 IBV_ACCESS_LOCAL_WRITE |
				 IBV_ACCESS_REMOTE_READ |
				 IBV_ACCESS_REMOTE_WRITE |
				IBV_ACCESS_MW_BIND);
	if (!cb->rping_rdma_mr) {
		fprintf(stderr, "rdma_buf reg_mr failed\n");
		ret = errno;
		goto err2;
	}

	rping_setup_wr(cb);
	return 0;

err2:
	ret = xib_umem_free_mem(cb, cb->rdma_buf_chunk_id,
			cb->rdma_buf, data_size);
	ret |= xib_umem_free_chunk(cb, cb->rdma_buf_chunk_id);
	cb->rdma_buf_chunk_id = XMEM_INVALID_CHUNK_ID;
err1:
	ret = xib_umem_free_mem(cb, cb->send_buf_chunk_id,
			cb->rping_send_buf, sizeof(*cb->rping_send_buf));
	ret |= xib_umem_free_chunk(cb, cb->send_buf_chunk_id);
	cb->send_buf_chunk_id = XMEM_INVALID_CHUNK_ID;
	return -EFAULT;
}

void set_wr_id(uint64_t *buf, int id)
{
	/* for both send & recv wr, first 64B are wr id */
	*buf = id;
}

int test_server_rping(struct app_context *ctx)
{
	struct rdma_cm_id* cm_id;
	struct ibv_recv_wr *bad_recv_wr;
	struct ibv_send_wr *bad_send_wr;
	int err, r, i = 0, cq_depth = 1;
	static int qp_cnt = 0;
	struct hh_send_dfmt hh_ctx_data;

	cm_id = ctx->cm_id;
	/* implement rping server data path */
	/* else run the normal rping functionality */
	ctx->iter_cnt = 1;
	for (i = 0; i < ctx->iter_cnt; i++) {
		r = rand();

		set_wr_id(&ctx->rq_wr, r);
		ctx->rq_wr.next = NULL;

		/* 1. Wait for incoming SEND */
		err = ibv_post_recv(ctx->cm_id->qp, &ctx->rq_wr, &bad_recv_wr);
	        if (err < 0) {
	                printf("Failed to post the RQE for incoming SEND at %d\n",
	                       __LINE__);
	                /* free qp */
	                return -1;
	        }
	        /* poll for CQ */
	        rdmatest_poll_cq(ctx, r);

		/* 4. Wait for Incoming SEND */
		set_wr_id(&ctx->rq_wr, r + 3);
		err = ibv_post_recv(ctx->cm_id->qp, &ctx->rq_wr, &bad_recv_wr);
		if (err < 0) {
			printf("Failed to post the RQE for incoming SEND at %d\n",
			       __LINE__);
			/* free qp */
			return -1;
		}
		rdmatest_poll_cq(ctx, r + 3);

		ctx->rping_send_buf->buf = ctx->rdma_buf;
		ctx->rping_send_buf->rkey = ctx->rping_rdma_mr->rkey;
		ctx->rping_send_buf->size = PAYLOAD_SIZE;

		set_wr_id(&ctx->sq_wr, r + 2);
		ctx->sq_wr.next = NULL;
		printf("Rdma buf address is 0x%llx: rkey is %#x\n",
				ctx->rdma_buf, ctx->rping_rdma_mr->rkey);
		err = ibv_post_send(ctx->cm_id->qp, &ctx->sq_wr, &bad_send_wr);
		if (err < 0) {
			printf("Failed to post the RQE for incoming SEND at %d\n",
			       __LINE__);
			/* free qp */
			return -1;
		}
		rdmatest_poll_cq(ctx, r + 2);
	}
	return 0;
}

struct ctx_list *update_ctx_entry(struct rping_test *hrping, struct rdma_cm_id *id,
				  enum rdma_cm_event_type event)
{
	struct ctx_list *temp;

	pthread_mutex_lock(&hrping->cm_list_lock);

	temp = hrping->head;
	while (temp) {
		if (temp->client_cm_id == id)
			break;
		temp = temp->next;
	}

	if (temp) {
		if (event == RDMA_CM_EVENT_ESTABLISHED) {
			temp->state = ESTABLISHED;
		} else {
			temp->state = INVALID_STATE;
		}
	} else
		printf("No existing connetion found\n");
	pthread_mutex_unlock(&hrping->cm_list_lock);
	return temp;
}

int set_ctx_to_rx_connect_req(struct rping_test *hrping, struct rdma_cm_id *id,
			      enum rdma_cm_event_type event)
{
	struct ctx_list *temp;

	if (event == RDMA_CM_EVENT_CONNECT_REQUEST) {
		/* create app region and wait for CONNECT establishment */
		/* create a cm channel and ID to wait */
		temp = malloc(sizeof(struct ctx_list));
		if (!temp) {
			printf("Failed to allocate memory for CM ctx\n");
			return -ENOMEM;
		}
		memset(temp, 0, sizeof (temp));
		temp->next = NULL;
		temp->client_cm_id = id;
		temp->ctx.hh_mem_alloc = 0;
		temp->ctx.rping_send_buf = (long)(-EINVAL);
		temp->ctx.rdma_buf = 0;

		pthread_mutex_lock(&hrping->cm_list_lock);
		if (!hrping->head)
			hrping->head = temp;
		else
			hrping->prev->next = temp;

		hrping->prev = temp;
		temp->state = CONNECT_REQ;
		pthread_mutex_unlock(&hrping->cm_list_lock);

	} else if (event == RDMA_CM_EVENT_ESTABLISHED) {
		temp = update_ctx_entry(hrping, id, event);
		if (!temp) {
			printf("ERROR: Connect request must be Rx before RTU\n");
			return -EINVAL;
		}
	} else if ((event == RDMA_CM_EVENT_ADDR_RESOLVED) ||
		   (event == RDMA_CM_EVENT_ROUTE_RESOLVED)) {
		/* no error */
	} else if (event == RDMA_CM_EVENT_DISCONNECTED) {
		hrping->disconnects++;
		printf("DISCONNECT\n");
		if (hrping->disconnects >= hrping->rx_test_qp_cnt)
			exit(0);
	} else {
		printf("Event %s:%d is not being handled\n",
			rdma_event_str(event), event);
		/* remove entry */
		return -1;
	}
	return 0;
}

void *cm_thread(void *args)
{
	struct rping_test *hrping = args;
	int ret;
	struct rdma_cm_event *event;

	/* create event channel and ID */
	hrping->cm_channel = rdma_create_event_channel();
	if (!hrping->cm_channel) {
		printf("Failed to create event channel \n");
		pthread_exit((void *)(intptr_t)-1);
	}

	ret = rdma_create_id(hrping->cm_channel, &hrping->cm_id,
			     NULL, RDMA_PS_TCP);

	if (ret) {
		printf("Failed to create CM ID\n");
		pthread_exit((void *)(intptr_t)-1);
	}

	init_server_cm(hrping);
	do {
		if (rdma_get_cm_event(hrping->cm_channel, &event)) {
			error_handler("GET_CM_EVENT Failed:");
			continue;
		}
#if 0
		if (event->event != RDMA_CM_EVENT_CONNECT_REQUEST) {
			printf("Invalid CM: Expected:%s, Received:%s",
                               rdma_event_str(RDMA_CM_EVENT_CONNECT_REQUEST),
                               rdma_event_str(event->event));
			continue;
		}
#endif
		ret = set_ctx_to_rx_connect_req(hrping, event->id, event->event);
		rdma_ack_cm_event(event);
	} while(hrping->disconnects < hrping->rx_test_qp_cnt);
}

void *est_thread(void *args)
{
	struct ctx_list *temp = NULL;
	struct rping_test *hrping = args;
	int i = 0, ret;
        struct ibv_qp_init_attr  init_attr;
        struct rdma_conn_param conn_param;

	while (1) {
		pthread_mutex_lock(&hrping->cm_list_lock);
		if (!temp)
			temp = hrping->head;
		while(temp && (i < 3)) {
			if (temp->state == CONNECT_REQ) {
				/* start processing the test */
				temp->ctx.cm_id = temp->client_cm_id;
				if (app_req_regions(&temp->ctx))
					error_handler("request regions Failed:");
                                memset(&init_attr, 0, sizeof(init_attr));
		                init_attr.cap.max_send_wr = hrping_test.q_depth;
		                init_attr.cap.max_send_sge = 1;
		                init_attr.cap.max_recv_wr = hrping_test.q_depth;
		                init_attr.cap.max_recv_sge = (PAYLOAD_SIZE / ERNIC_SGE_SIZE);

		                init_attr.send_cq = temp->ctx.cq;
		                init_attr.recv_cq = temp->ctx.cq;
		                init_attr.qp_type = IBV_QPT_RC;

		                conn_param.initiator_depth      = 16;
		                conn_param.responder_resources  = 16;
		                conn_param.retry_count          = 10;
		                conn_param.rnr_retry_count	= 7;
				ret = rdma_create_qp(temp->client_cm_id, temp->client_cm_id->pd,
						     &init_attr);

				if (ret)
					error_handler("create_qp Failed:");

				rdma_accept(temp->client_cm_id, &conn_param);
				temp->state = CONNECT_REQ_PROC;
			} else if (temp->state == ESTABLISHED) {
				rping_setup_buffers(&temp->ctx);
				temp->state = READY_FOR_DATA_TRANSFER;
				hrping_test.connections++;
			}
			temp = temp->next;
			i++;
		}
		i = 0;
		pthread_mutex_unlock(&hrping->cm_list_lock);
		if (hrping->connections >= hrping->rx_test_qp_cnt)
			pthread_exit((void *)(uintptr_t)0);
		sched_yield();
	}
}

int run_server(struct rdma_param* uparam)
{
	struct ctx_list *list;
	int err;
	int cnt = 0;

	/* Create rping CM thread */
	err = pthread_create(&hrping_test.cm_thread, NULL, cm_thread,
			     (void *)&hrping_test);
	if (err) {
		printf("Failed to create CM thread in rping\n");
		return err;
	}

	err = pthread_create(&hrping_test.establish_thread, NULL, est_thread,
			     (void *)&hrping_test);
	if (err) {
		printf("Failed to create establishment thread\n");
		return err;
	}

	while (1) {
		pthread_mutex_lock(&hrping_test.cm_list_lock);
		if (!hrping_test.head) {
			pthread_mutex_unlock(&hrping_test.cm_list_lock);
			continue;
		}

		list = hrping_test.head;
		while (list) {
			if (list->state == READY_FOR_DATA_TRANSFER) {
				err = test_server_rping(&list->ctx);
				cnt++;
				if (cnt >= hrping_test.rx_test_qp_cnt) {
					pthread_mutex_unlock(&hrping_test.cm_list_lock);
					return 0;
				}
				list->state = INVALID_STATE;
				/* delete the entry */
			}
			list = list->next;
		}
		pthread_mutex_unlock(&hrping_test.cm_list_lock);
	}
}

int rdma_parse_param(int argc, char *argv[], struct rdma_param** uparam)
{
	int c, val, ret;
	struct rdma_param* param = *uparam;
	char* base;

	param->qp_count = RDMA_TEST_QP_CNT;
	param->iter_count = RDMA_LOOP_CNT;
	param->server_addr = (char *) malloc(100);
	param->rkey_err = (char *) malloc(20);
	param->va_err = (char *) malloc(20);
	param->server = 1;
	hrping_test.rx_test_qp_cnt = 1;

	hrping_test.q_depth = 32;
	while (1) {
		int option_index = 0;
		static struct option long_options[] = {
			{"server",	required_argument, 0,  's' },
			{"qp",		required_argument, 0,  'q' },
			{"iter",	required_argument, 0,  'i' },
			{"err_rkey",	required_argument, 0,  'k' },
			{"err_va",	required_argument, 0,  'v' },
			{"random",	no_argument, 	   0,  'r' },
			{"cmtest",	no_argument,	   0,  'c' },
			{"file",	no_argument,	   0,  'f' },
			{"help",	no_argument, 	   0,  'h' },
			{"debug",	no_argument, 	   0,  'd' },
			{"q_depth",	required_argument, 0,  'D' },
			{"mem_type",	required_argument, 0,  'm' },
			{NULL,0,NULL,0}
		};
		c = getopt_long(argc, argv, "s:q:i:k:v:rcfm:hdD:",
				long_options, &option_index);

		if (c == -1)
			break;

		switch(c) {
			case 'D':
				hrping_test.q_depth = atoi(optarg);
				printf("Q depth is %d\n", hrping_test.q_depth);
				break;
			case 'd':
				debug = 1;
				break;
			case 'h':
				param->help = 1;
				break;
			case 'f':
				param->file = 1;
				break;
			case 'c':
				param->cm_test = 1;
				break;
			case 's':
				param->server = 0;
				strcpy(param->server_addr, optarg);
				break;
			case 'q':
				param->qp_count = atoi(optarg);
				break;
			case 'i':
				param->iter_count = atoi(optarg);
				break;
			case 'k':
				strcpy(param->rkey_err, optarg);
				break;
			case 'v':
				strcpy(param->va_err, optarg);
				break;
			case 'r':
				param->random = 1;
				break;
			case 'm':
				if ((ret = xib_mem_str_to_mem_type(optarg)) < 0)
					return ret;
				mem_type = ret;
				break;
			default:
				printf("unknown option [%d:%s]\n", __LINE__,
								__func__);
		}
	}
	return errno;
}

void print_help(void)
{
	printf("\ninv_imm_test_in server application:\n"
	       "\t--help  (or) -h: Display help\n"
	       "\t--debug  (or) -d: debug info\n"
	       "\t--q_depth (or) -D: Queue depth\n"
	      );
	printf("\t-m <type> (or) mem_type <type>: RDMA buffers memory type"
			" [pl, eddr, ps , bram]\n");
}

int main(int argc, char *argv[ ])
{
	int	err;
	int	s;
	int tnum,i,c, size;
	int rand_size[20];
        int randomIndex ;
	pthread_t  thread_id;
	struct thread_info* tinfo;
	pthread_attr_t p_attr;
	struct rdma_param* uparam;
	struct ctx_list *list;
	struct app_context *cb;
	unsigned int data_size = 4096;

	uparam = (struct rdma_param*)malloc(sizeof *uparam);
	if (!uparam) {
		printf("Failed to allocate memory for parameters\n");
		return -ENOMEM;
	}

	memset(uparam,0, sizeof *uparam);
	if(rdma_parse_param(argc, argv, &uparam))
		error_handler("parsing user param Failed");

	if (uparam->help) {
		print_help();
		return -1;
	}

	err = pthread_mutex_init(&hrping_test.cm_list_lock, NULL);
	if (err != 0) {
		printf("\n mutex init has failed\n");
		return err;
	}

	run_server(uparam);

	pthread_join(hrping_test.cm_thread, NULL);
	pthread_join(hrping_test.establish_thread, NULL);

	pthread_mutex_destroy(&hrping_test.cm_list_lock);

	/* wait for the disconnects to come */
	while(hrping_test.disconnects < hrping_test.rx_test_qp_cnt);

	list = hrping_test.head;
	while (list) {
		cb = &list->ctx;
		if (cb->send_buf_chunk_id > 0) {
			if (((long)cb->rping_send_buf) > 0)
				err = xib_umem_free_mem(cb, cb->send_buf_chunk_id,
					cb->rping_send_buf, sizeof(*cb->rping_send_buf));
			err |= xib_umem_free_chunk(cb, cb->send_buf_chunk_id);
		}

		if (cb->rdma_buf_chunk_id > 0) {
			if (cb->rdma_buf > 0)
				err = xib_umem_free_mem(cb, cb->rdma_buf_chunk_id,
					cb->rdma_buf, data_size);
			err |= xib_umem_free_chunk(cb, cb->rdma_buf_chunk_id);
		}
		list = list->next;
	}
	return 0;
}
