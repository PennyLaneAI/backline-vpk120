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
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdint.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <unistd.h>
#include <errno.h>
#include <ctype.h>
#include <getopt.h>
#include <libgen.h>
#include <rdma/rdma_cma.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <inttypes.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <rdma/rdma_verbs.h>
#include <rdma/rdma_verbs.h>
#include <umm_export.h>
#include "xhw_hs_server.h"
#define _GNU_SOURCE

#define DEBUG_LOG if (debug) printf
#define XRNIC_WQE_SIZE	(64)
#define PR_INFO(v, fmt, ...) do {\
				if (v <= verbose)\
					fprintf(stdout, fmt, __VA_ARGS__);\
			} while(0)
/* Informed Errors look ugly when more than 80 characters long */
#define	error_handler(x)	do {					\
					char str[100];			\
					sprintf(str, "[0;31m%s",x);	\
					perror(str);			\
					printf("[0;m");		\
					exit(EXIT_FAILURE);		\
				} while(0);

unsigned int debug = 0, verbose = 0;
unsigned int qp_cfg_cnt = 0;
struct per_qp_hw_hsk_reg_info qp_hs_cfg;
struct rping_test hrping_test;
unsigned int mem_type = XMEM_PL_DDR;
bool reuse_rdma_buf = false;

__attribute__((destructor)) void destruct_rping(void)
{
	if (hrping_test.dev_fd > 0)
		close(hrping_test.dev_fd);
}

int alloc_bram_mem(uint32_t size, uint64_t *addr)
{
	if (size > (bram_mem_mgr.size - bram_mem_mgr.alloc_size))
		return -ENOMEM;

	*addr = bram_mem_mgr.mem_base_p + bram_mem_mgr.alloc_size;
	bram_mem_mgr.alloc_size += size;

	return 0;
}

int init_bram()
{
	bram_mem_mgr.mem_base_p = BRAM_MEM_BASE;
	bram_mem_mgr.size = (1024 * 1024);
	bram_mem_mgr.alloc_size = 0;
}

#if 0
__attribute__((destructor)) void destruct_rping(void)
{
	int i;
	struct ctx_list *list = hrping_test.head, *temp;

	/* Delete allocated contexts */
	while(list) {
		if (list->ctx.hh_mem_alloc)
			free(list->ctx.hw_hs_va);
		free(list->ctx.rdma_buf);
		temp = list;
		list = list->next;
		free(temp);
	}
}
#endif

void rdmatest_poll_cq(struct app_context* rdma_ctx, int id)
{
	struct ibv_wc wc;
	void* cq_ctx;
	struct ibv_cq* cq_event;
	int ret;

	while (1) {
		ret = ibv_poll_cq(rdma_ctx->cq, 1, &wc);
		if (ret <= 0)
			continue;
		if (wc.status != IBV_WC_SUCCESS) {
			printf("ernic wc status = %d(%s), id = %d:%d\n",
					wc.status,
					ibv_wc_status_str(wc.status),
					wc.wr_id, id);
			return;
		}

		if (wc.wr_id == id)
			break;
	}
}

int init_server_cm(struct rping_test* hrping)
{
	int err;
	struct sockaddr_in sin4_addr;
	struct rdma_cm_event *event;

#if 0
	if (hrping->param.sin.ss_family == AF_INET)
		((struct sockaddr_in *) &hrping->param.sin)->sin_port = hrping->param.port;
	else
		((struct sockaddr_in6 *) &hrping->param.sin)->sin6_port = hrping->param.port;
	if (rdma_bind_addr(hrping->cm_id, (struct sockaddr *) &hrping->param.sin))
		error_handler("RDMA BIND Failed:");
#else
	sin4_addr.sin_family = AF_INET;
	sin4_addr.sin_port = htons(7176);
	sin4_addr.sin_addr.s_addr = INADDR_ANY;
	err = rdma_bind_addr(hrping->cm_id, (struct sockaddr *)&sin4_addr);

	if (err) {
		error_handler("RDMA BIND Failed:");
		return err;
	}
#endif

	err = rdma_listen(hrping->cm_id, hrping->param.qp_cnt);
	if (err) {
		error_handler("RDMA Listen Failed:");
		return err;
	}

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

	rdma_ctx->comp_chan = NULL;

	/* TODO remove 32 CQ depth */
	rdma_ctx->cq = ibv_create_cq(cm_id->verbs, 32, NULL,
				     rdma_ctx->comp_chan, 0);
	if (!rdma_ctx->cq) {
		error_handler("create_cq Failed:");
		return -EFAULT;
	}

	rdma_ctx->rq_depth = HH_CQ_RQ_DEPTH;
	err = ibv_req_notify_cq(rdma_ctx->cq, 0);
	if (err) {
		error_handler("req_notify_cq Failed:");
		return -EFAULT;
	}

	return 0;
}

int rping_setup_wr(struct app_context *cb)
{
	int ret, chunk_id, data_size = sizeof(struct rping_rdma_info);
	volatile uint64_t vaddr;
	void *va;
	char *send_buf, recv_buf;
	bool proc_access = true;

	/* alocate send buf */
	chunk_id = xib_umem_alloc_chunk(cb->cm_id->verbs,
			XMEM_PL_DDR, data_size, data_size,
			proc_access);

	if (chunk_id < 0) {
		printf("Failed to alloc chunk %d\n", __LINE__);
		return -EFAULT;
	}

	cb->send_buf_chunk_id = chunk_id;

	vaddr = xib_umem_alloc_mem(cb->cm_id->verbs, chunk_id,
			data_size);
	if (XMEM_IS_INVALID_VADDR(vaddr)) {
		printf("Failed to alloc memory from chunk\n");
		/* release the chunk */
		ret = xib_umem_free_chunk(cb->cm_id->verbs, chunk_id);
		cb->send_buf_chunk_id = -EINVAL;
		return -EFAULT;
	}

	/* RQ SGEs are not needed with UMM SW arch */
	cb->rping_send_buf = vaddr;
        cb->recv_sgl.length = sizeof (struct rping_rdma_info);

	/* L-Key must have the buffer chunk ID */
        cb->recv_sgl.lkey = cb->send_buf_chunk_id;
        cb->rq_wr.sg_list = &cb->recv_sgl;
        cb->rq_wr.num_sge = 1;

        cb->send_sgl.addr = (uint64_t) vaddr;
        cb->send_sgl.length = sizeof (struct hh_send_fmt);

        cb->send_sgl.lkey = cb->send_buf_chunk_id;

        cb->sq_wr.opcode = IBV_WR_SEND;
        cb->sq_wr.send_flags = IBV_SEND_SIGNALED;
        cb->sq_wr.sg_list = &cb->send_sgl;
        cb->sq_wr.num_sge = 1;

	return 0;
}

static int rping_setup_buffers(struct app_context *cb)
{
	int ret;
	uint32_t size = (4 * 1024);

	rping_setup_wr(cb);
	return 0;
}

int hh_post_rx_wqes(struct app_context *ctx, int size, int cnt)
{
	int i, ret = 0, r = ctx->rx_posted;
	uint32_t free_wqe;

	free_wqe = ctx->rq_depth - ctx->rx_posted + ctx->rx_compl;
	if (free_wqe < cnt) {
		printf("No enuf wqes to post all the requested. Posting only %d\n", free_wqe);
		cnt = free_wqe;
	}
	for (i = 0; i < cnt; i++) {
		ret = rdma_post_recv(ctx->cm_id, (void *)(uintptr_t)(r + i),
				ctx->hw_hs_va, size, ctx->hw_hsk_mr);
		if (ret < 0) {
			printf("HH post rx failed\n");
			break;
		}
	}
	ctx->rx_posted += i;
	ctx->rx_wqe_ready = 1;
	return i;
}

int hw_hs_qp_cfg(struct app_context *cb, struct rdma_param* param)
{
	int ret;
	static int chunk_id = -EINVAL;
	static uint64_t vaddr, data_size, pa = 0;
	uint64_t sq_ba_pa;
	bool proc_access = false;

	qp_hs_cfg.data_pattern = 0xFEADDEAF;
	qp_hs_cfg.qp_num = cb->qp_num;
	qp_hs_cfg.rkey = cb->remote_rkey;
	qp_hs_cfg.va_lsb_val = cb->remote_addr;
	qp_hs_cfg.va_msb_val = (cb->remote_addr >> 32);
        qp_hs_cfg.wqe_opcode = hrping_test.param.wqe_opcode;

	data_size = (hrping_test.param.data_size_kb) * 1024;
        qp_hs_cfg.data_size_kb = hrping_test.param.data_size_kb;

	if (!(reuse_rdma_buf && chunk_id > 0)) {
		/* alocate RDMA buffer */
		chunk_id = xib_umem_alloc_chunk(cb->cm_id->verbs,
				param->rdma_buf_mem_type, data_size,
				data_size, proc_access);

		if (chunk_id < 0) {
			printf("Failed to alloc chunk %d\n", __LINE__);
			cb->rdma_buf_chunk_id = -EINVAL;
			return -EFAULT;
		}
		cb->rdma_buf_chunk_id = chunk_id;

		vaddr = xib_umem_alloc_mem(cb->cm_id->verbs, chunk_id,
				data_size);
		if (XMEM_IS_INVALID_VADDR(vaddr)) {
			printf("Failed to alloc memory from chunk\n");
			ret = xib_umem_free_chunk(cb->cm_id->verbs, chunk_id);
			cb->rdma_buf_chunk_id = -EINVAL;
			return -EFAULT;
		}

		cb->rdma_buf_addr = vaddr;

		/* Get Physical address to write to HW HS per QP config */
		ret = xib_umem_get_phy_addr(cb, chunk_id, vaddr,
						&pa);
		if (ret < 0) {
			printf("Failed to get RDMA buf phys addr\n");
			return ret;
		}
	} else
		cb->rdma_buf_chunk_id = -EINVAL;

	qp_hs_cfg.data_addr = pa;

	/* Allocate memory for SQs */
	data_size = param->queue_depth * XRNIC_WQE_SIZE;
	chunk_id = xib_umem_alloc_chunk(cb->cm_id->verbs,
				param->sq_mem_type, data_size,
				data_size, proc_access);
	if (chunk_id < 0) {
		printf("Failed to alloc memory for RDMA buffers %d\n",
				__LINE__);
		cb->sq_mem_chunk_id = -EINVAL;
		goto sq_chunk_alloc_fail;
	}

	cb->sq_mem_chunk_id = chunk_id;
	vaddr = xib_umem_alloc_mem(cb->cm_id->verbs, chunk_id,
				data_size);
	if (XMEM_IS_INVALID_VADDR(vaddr)) {
		printf("Failed to alloc memory from chunk\n");
		cb->sq_mem_chunk_id = -EINVAL;
		goto sq_mem_alloc_fail;
	}

	cb->sq_addr = vaddr;
	ret = xib_umem_get_phy_addr(cb->cm_id->verbs, chunk_id, vaddr, &sq_ba_pa);
	if (ret < 0) {
		printf("Failed to get SQ mem phys addr\n");
		ret = xib_umem_free_mem(cb->cm_id->verbs,
				chunk_id, vaddr, data_size);
		goto sq_mem_alloc_fail;
	}

	qp_hs_cfg.sq_addr = sq_ba_pa;

	ret = ioctl(hrping_test.dev_fd, HH_WRITE_QP_CFG, &qp_hs_cfg);
	if (ret)
		printf("ioctl failed at %d\n", __LINE__);
        return ret;

sq_mem_alloc_fail:
	ret = xib_umem_free_chunk(cb->cm_id->verbs, chunk_id);
	cb->sq_mem_chunk_id = -EINVAL;
sq_chunk_alloc_fail:
	if (cb->rdma_buf_chunk_id > 0) {
		ret = xib_umem_free_mem(cb->cm_id->verbs, cb->rdma_buf_chunk_id,
				cb->rdma_buf_addr,
				(hrping_test.param.data_size_kb) * 1024);
		ret = xib_umem_free_chunk(cb->cm_id->verbs, cb->rdma_buf_chunk_id);
		cb->rdma_buf_chunk_id = -EINVAL;
	}
	return -ENOMEM;
}

void set_wr_id(uint64_t *buf, int id)
{
	*buf = id;
}

int test_server_rping(struct app_context *ctx, struct rdma_param* param)
{
	struct rdma_cm_id* cm_id;
	struct ibv_recv_wr *bad_recv_wr;
	struct ibv_send_wr *bad_send_wr;
	int err, i = 0, cq_depth = 1;
	static int qp_cnt = 0;
	struct hh_send_fmt *hh_ctx_data;
	unsigned int  size;
	int r = rand();
	struct rping_rdma_info *rx_info;

	cm_id = ctx->cm_id;
	/* implement rping server data path */

	r = rand();

	DEBUG_LOG("QP context exhange started\n");
	/* get SEND from the remote node */
	set_wr_id(&ctx->rq_wr, r);

	ctx->rq_wr.next = NULL;
	err = ibv_post_recv(ctx->qp, &ctx->rq_wr, &bad_recv_wr);
	if (err < 0) {
		printf("Failed to post the RQE for incoming SEND at %d\n",
		       __LINE__);
		/* free qp */
		return -1;
	}

	/* post send with opcode and wqe cnt */
	hh_ctx_data = (struct hh_send_fmt *)ctx->rping_send_buf;
	hh_ctx_data->tf_siz = param->data_size_kb;
	hh_ctx_data->opcode = param->wqe_opcode;
	hh_ctx_data->wqe_cnt = param->qp_wqe_cnt;

	set_wr_id(&ctx->sq_wr, r + 1);
	ctx->sq_wr.next = NULL;
	err = ibv_post_send(ctx->qp, &ctx->sq_wr, &bad_send_wr);
        if (err < 0) {
              printf("Failed to post the RQE for incoming SEND at %d\n",
                     __LINE__);
                /* free qp */
              return err;
        }

        rdmatest_poll_cq(ctx, r);
	DEBUG_LOG("Rx context data from client\n");

	rx_info = (struct rping_rdma_info *)ctx->rq_wr.sg_list[0].addr;
	ctx->remote_rkey = ntohl(rx_info->rkey);
	ctx->remote_addr = be64toh(rx_info->buf);
	ctx->remote_len  = ntohl(rx_info->size);

	DEBUG_LOG("Received rkey %#x addr 0x%x%x from peer\n",
			ctx->remote_rkey, ctx->remote_addr >> 32, ctx->remote_addr);

	err = hw_hs_qp_cfg(ctx, param);
	DEBUG_LOG("QP# :%d reg config done\n", qp_cnt++);
	return err;
}

struct ctx_list *update_ctx_entry(struct rping_test *hrping, struct rdma_cm_id *id,
				  enum rdma_cm_event_type event)
{
	struct ctx_list *temp;

	pthread_mutex_lock(&hrping->cm_list_lock);

	temp = hrping->head;
	while (temp) {
		if (temp->client_cm_id == id) {
			break;
		}
		temp = temp->next;
	}

	if (temp) {
		if (event == RDMA_CM_EVENT_ESTABLISHED)
			temp->state = ESTABLISHED;
		else
			temp->state = INVALID_STATE;
	} else
		printf("No existing connetion found\n");
	pthread_mutex_unlock(&hrping->cm_list_lock);
	return temp;
}

int set_ctx_to_rx_connect_req(struct rping_test *hrping, struct rdma_cm_id *id,
			      enum rdma_cm_event_type event)
{
	struct ctx_list *temp;
	static uint32_t qp_cnt = 0;

	DEBUG_LOG("CM event is %d:%d\n", event, RDMA_CM_EVENT_CONNECT_REQUEST);
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
		temp->ctx.qp_num = qp_cnt;

		pthread_mutex_lock(&hrping->cm_list_lock);
		if (!hrping->head)
			hrping->head = temp;
		else
			hrping->prev->next = temp;

		hrping->prev = temp;
		temp->state = CONNECT_REQ;
		qp_cnt++;
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

	if (init_server_cm(hrping))
		pthread_exit((void *)-EFAULT);
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
	} while(hrping->connections < hrping->rx_test_qp_cnt);
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
		while (temp && (i < 3)) {
			if (temp->state == CONNECT_REQ) {
				/* start processing the test */
				temp->ctx.cm_id = temp->client_cm_id;
				if (app_req_regions(&temp->ctx))
					error_handler("request regions Failed:");
                                memset(&init_attr, 0, sizeof(init_attr));
		                init_attr.cap.max_send_sge = 1;
#if 0
		                init_attr.cap.max_send_wr = 1000;
		                init_attr.cap.max_recv_wr = HH_CQ_RQ_DEPTH;
#else
				init_attr.cap.max_recv_wr = 16;
		                init_attr.cap.max_send_wr = 32;
#endif
		                init_attr.cap.max_recv_sge = 2;

		                init_attr.send_cq = temp->ctx.cq;
		                init_attr.recv_cq = temp->ctx.cq;
		                init_attr.qp_type = IBV_QPT_RC;

		                conn_param.initiator_depth      = 16;
		                conn_param.responder_resources  = 16;
		                conn_param.retry_count          = 10;
				ret = rdma_create_qp(temp->client_cm_id, temp->client_cm_id->pd,
						     &init_attr);

				if (ret)
					error_handler("create_qp Failed:");

				temp->ctx.qp = temp->client_cm_id->qp;
				rdma_accept(temp->client_cm_id, &conn_param);
				temp->state = CONNECT_REQ_PROC;
			} else if (temp->state == ESTABLISHED) {
				rping_setup_buffers(&temp->ctx);
				temp->state = READY_FOR_DATA_TRANSFER;
				DEBUG_LOG("QP is READY for data transfers\n");
				hrping_test.connections++;
				DEBUG_LOG("[%d] Connections of required [%d] are setup\n",
					hrping->connections, hrping->rx_test_qp_cnt);
			}
			temp = temp->next;
			i++;
		}
		i = 0;
		pthread_mutex_unlock(&hrping->cm_list_lock);
		if (hrping->connections >= hrping->rx_test_qp_cnt)
			break;
		sched_yield();
	}

	return;
}

int run_server(struct rping_test *test)
{
	struct ctx_list *list;
	struct rdma_param* uparam = &test->param;
	int err;
	unsigned int err_qp = 0, qp_cnt = 0;

	/* Create rping CM thread */
	err = pthread_create(&test->cm_thread, NULL, cm_thread,
			     (void *)test);
	if (err) {
		printf("Failed to create CM thread in rping\n");
		return err;
	}

	err = pthread_create(&test->establish_thread, NULL, est_thread,
			     (void *)test);
	if (err) {
		printf("Failed to create establishment thread\n");
		return err;
	}

	while (1) {
		pthread_mutex_lock(&test->cm_list_lock);
		if (!test->head) {
			pthread_mutex_unlock(&test->cm_list_lock);
			sleep(0.1);
			continue;
		}

		list = test->head;
		while (list) {
			if (list->state == READY_FOR_DATA_TRANSFER) {
				err = test_server_rping(&list->ctx, uparam);
				if (err)
					break;

				list->state = QP_CFG_DONE;
				qp_cfg_cnt++;
				if (qp_cfg_cnt >= uparam->qp_cnt) {
					pthread_mutex_unlock(&test->cm_list_lock);
					DEBUG_LOG("ALL QPs have exhanged context\n");
					return 0;
				}
				list->state = INVALID_STATE;
				/* delete the entry */
			}
			list = list->next;
		}
		pthread_mutex_unlock(&test->cm_list_lock);
		if (err)
			return -EFAULT;
	}
	return 0;
}

void print_help(void)
{
	printf("\t--debug    :debug prints\n");
	printf("\t--help     :Display help\n");
	printf("\t--verbose  :Enable verbosity\n");
	printf("\t--common-rdma-buf or -c  : Use same RDMA buffer for all QPs\n");
	printf("\t-Z <val> or --data-pattern <val> : Data pattern in Hex\n");
	printf("\t-Q <val> or --qp <val>         : QP count\n");
	printf("\t-K <val> or --tf-size <val>    : Data transfer size in KB\n");
	printf("\t-a <ip-addr> or --ip <ip-addr> : IP address input\n");
	printf("\t-D <val> or --q-depth <val>    : Queue Depth. \n"
		"\t\t\t Value must be power of 2\n\n");
#if 0
	printf("\t-I	:Source address to bind to for client.\n");
#endif
	printf("\t-M <val> or --mode <val>       : Mode of operation\n"
		"\t\t\t \"burst\" -> Burst mode, \"inline\" -> Inline Mode\n\n");
	printf("\t-N <val> or --burst-count <val>: Burst count value\n"
		"\t\t\t value must be < queue depth and < 16 i.e WQE depth\n\n");
	printf("\t-o <val> or --opcode <val>     : opcode\n"
		"\t\t\t \"wr\" -> write, \"rd\" -> Read, \"send\"->Send\n\n");
#if 0
	printf("\t-p <port>:port number\n");
#endif
	printf("\t-P <val> or --freq <val>       : Clock frequency in MHz\n"
		"\t\t\t default val is 200\n\n");
	printf("\t-W <val> or --msg-count <val>  : WQE count\n"
		"\t\t\t Value must be mulitple of Burst count\n\n");
	printf("\t-S <type> or --sqmem <type>    : HW HS SQ Memory type\n"
		"\t\t\t memory types are : pl, ps, eddr, bram\n\n");
	printf("\t-R <type> or --data-mem <type> : HW HS RDMA buffer"
		" Memory type\n"
		"\t\t\t memory types are : pl, ps, eddr, bram\n\n");
}

void init_app_defaults(struct rdma_param *p)
{
	p->port = htobe16(7174);
	p->qp_cnt = 1;

	p->wqe_opcode = 0; // write by default
	p->data_size_kb = 4; //4KB
	p->burst_mode = 1; // Burst mode enabled
	p->qp_cnt = 1;
	p->queue_depth = 16;
	p->db_burst_cnt = 15;
	p->qp_wqe_cnt = 100;
	p->data_pattern = 0xFEADEAF;
	p->cpu_clk = 200; // 200MHz clock
	p->block_test = 0;
}

static int get_addr(char *dst, struct sockaddr *addr)
{
	struct addrinfo *res;
	int ret;

	ret = getaddrinfo(dst, NULL, NULL, &res);
	if (ret) {
		printf("getaddrinfo failed (%s) - invalid hostname or IP address\n", gai_strerror(ret));
		return ret;
	}

	if (res->ai_family == AF_INET)
		memcpy(addr, res->ai_addr, sizeof(struct sockaddr_in));
	else if (res->ai_family == PF_INET6)
		memcpy(addr, res->ai_addr, sizeof(struct sockaddr_in6));
	else {
		printf("invalid ai family\n");
		ret = 1;
	}
	((struct sockaddr_storage *)addr)->ss_family = res->ai_family;
	freeaddrinfo(res);
	return ret;
}

static struct option long_options[] = {
	{"opcode", required_argument, NULL, 'o'},
	{"help", no_argument, &hrping_test.param.help, 1},
	{"data-pattern", required_argument, NULL, 'Z'},
	{"tf-size", required_argument, NULL, 'K'},
	{"mode", required_argument, NULL, 'M'},
	{"qp", required_argument, NULL, 'Q'},
	{"q-depth", required_argument, NULL, 'D'},
	{"burst-count", required_argument, NULL, 'N'},
	{"msg-count", required_argument, NULL, 'W'},
	{"ip", required_argument, NULL, 'a'},
	{"freq", required_argument, NULL, 'P'},
	{"data-mem", required_argument, NULL, 'R'},
	{"sqmem", required_argument, NULL, 'S'},
	{"verbose", no_argument, &verbose, 1},
	{"debug", no_argument, &debug, 1},
	{"common-rdma-buf", no_argument, NULL, 'c'},
	{0,0,0,0},
};

int parser(int argc, char *argv[], struct rdma_param *p)
{
	#define BURST_MODE_STR "burst"
	#define INLINE_MODE_STR "inline"
	#define RD_OPC_STR	"rd"
	#define WR_OPC_STR	"wr"
	#define SEND_OPC_STR	"send"
	#define WR_OPC		(0)
	#define RD_OPC		(1)
	#define SEND_OPC	(2)

	int op, i = 0;
	char *end;
	int ret = 0;

	p->rdma_buf_mem_type = XMEM_PL_DDR;
	p->sq_mem_type = XMEM_PL_DDR;
	while ((op = getopt_long(argc, argv, ":a:I:P:p:o:K:M:Q:D:N:W:TZ:S:R:c",
				long_options, NULL)) != -1) {
		switch (op) {
#if 0
		case 'h':
			p->help = 1;
			break;
#endif
		case 'c':
			reuse_rdma_buf = true;
			break;
		case 'Z':
			p->data_pattern = strtoul(optarg, &end, 16);
			p->data_pattern_en = 1;
			break;
		case 'o':
			if (!strcasecmp(RD_OPC_STR, optarg))
				p->wqe_opcode = RD_OPC;
			else if (!strcasecmp(WR_OPC_STR, optarg))
				p->wqe_opcode = WR_OPC;
			else if (!strcasecmp(SEND_OPC_STR, optarg))
				p->wqe_opcode = SEND_OPC;
			else {
				printf("Invalid opcode %s\n", optarg);
				return -EINVAL;
			}

			break;
		case 'K':
			p->data_size_kb = atoi(optarg);
			break;
		case 'M':
			if (!strcasecmp(BURST_MODE_STR, optarg))
				p->burst_mode = 1;
			else if (!strcasecmp(INLINE_MODE_STR, optarg))
				p->burst_mode = 0;
			else {
				printf("%s is not a valid mode\n", optarg);
				return -EINVAL;
			}
			break;
		case 'Q':
			p->qp_cnt = atoi(optarg);
			if (p->qp_cnt <= 0) {
				printf("Invalid QP count \n");
				return -EINVAL;
			}
			break;

		case 'D':
			p->queue_depth = atoi(optarg);
			DEBUG_LOG("Queue depth is %d\n", p->queue_depth);
			if (p->queue_depth < 0) {
				p->queue_depth = 1;
			}
			break;

		case 'N':
			p->db_burst_cnt = atoi(optarg);
			DEBUG_LOG("Burst count is %d\n", p->db_burst_cnt);
                        if (p->db_burst_cnt < 0)
				p->db_burst_cnt = 1;

			if (p->db_burst_cnt >= MAX_WQE_DEPTH)
				p->db_burst_cnt = MAX_WQE_DEPTH - 1;
			break;
		case 'W':
			p->qp_wqe_cnt = atoi(optarg);
			DEBUG_LOG("wqe count is %d\n", p->qp_wqe_cnt);
			break;
		case 'a':
			ret = get_addr(optarg, (struct sockaddr *) &p->sin);
			break;
		case 'I':
			ret = get_addr(optarg, (struct sockaddr *) &p->ssource);
			break;
		case 'P':
			p->cpu_clk = atoi(optarg);
			DEBUG_LOG("CLK is %d MHz\n", p->cpu_clk);
			break;
		case 'p':
			p->port = htobe16(atoi(optarg));
			DEBUG_LOG("port %d\n", p->port);
			break;
		case 'R':
			if ((ret = xib_mem_str_to_mem_type(optarg)) < 0)

				return ret;
			p->rdma_buf_mem_type = ret;
			ret = 0;
			break;
		case 'S':
			if ((ret = xib_mem_str_to_mem_type(optarg)) < 0)
				return ret;
			p->sq_mem_type = ret;
			ret = 0;
			break;
		case 0:
			/* flagged fields */
			break;
		default:
			printf("Rx invalid input %c\n", optopt);
			ret = EINVAL;
		}
	}
	return ret;
}

int init_xperf_app(struct rping_test *test, int argc, char *argv[])
{
	struct rdma_param *p = &test->param;
	uint32_t i;
	int ret;

	memset(p, 0, sizeof *p);
	init_app_defaults(p);
	if (ret = parser(argc, argv, p)) {
		printf("parsing failed %d\n", ret);
		return -EFAULT;
	}

	if (!p->qp_cnt) {
		printf("QP count cant be 0\n");
		return -1;
	}

	if (p->queue_depth <= 1) {
		printf("HH queue depth can't be < 2\n");
		return -1;
	}

	if (p->db_burst_cnt >= p->queue_depth) {
		printf("HH Burst cnt must be < Queue depth\n");
		return -1;
	}

#if 0
	i = sizeof(struct rping_cb) * p->qp_cnt;
	p->cb = malloc(i);
	if (!p->cb) {
		printf("Failed to allocate memory for contexts\n");
		return -ENOMEM;
	}
	memset(p->cb, 0, i);
#endif

	test->dev_fd = open(HH_DEV_NAME, O_RDWR);
	if (test->dev_fd < 0) {
		printf("Failed to open [%s] file\n", HH_DEV_NAME);
		return -EFAULT;
	}
	return 0;
}

int test_hw_handshake(void)
{
	int stat_reg = 0, ret;

	while (!(stat_reg & 1)) {
		ret = ioctl(hrping_test.dev_fd, TEST_DONE_READ_OP, &stat_reg);
        	if (ret) {
			printf("ioctl failed at %d\n", __LINE__);
			break;
		}
		sleep(1);
	}
        return ret;
}

int enable_hw_hs(void)
{
	int ret;

	ret = ioctl(hrping_test.dev_fd, HH_EN_OP);
        if (ret)
		printf("ioctl failed: %d\n", __LINE__);
	else
		DEBUG_LOG("Enabled HW HS");
        return ret;
}

int fill_hh_ctrl_cfg(struct rping_test *test_info)
{
        int ret = 0;
	struct rdma_param *p = &test_info->param;
	struct hw_hsk_ctrl_cfg hw_ctrl_cfg;

	hw_ctrl_cfg.hw_hs_en = 0;
	hw_ctrl_cfg.burst_mode = p->burst_mode;
	hw_ctrl_cfg.db_burst_cnt = p->db_burst_cnt;
	hw_ctrl_cfg.qp_cnt = p->qp_cnt;
	hw_ctrl_cfg.data_pattern_en = p->data_pattern_en;
	hw_ctrl_cfg.queue_depth = p->queue_depth;

	hw_ctrl_cfg.qp_wqe_cnt = p->qp_wqe_cnt;

	ret = hw_ctrl_cfg.qp_wqe_cnt % hw_ctrl_cfg.db_burst_cnt;
	if (ret) {
		hw_ctrl_cfg.qp_wqe_cnt -= ret;
		printf("WQE count [%d] is not a multiple of Burst count [%d]\n"
			"Adjusting the wqe count to %d\n", p->qp_wqe_cnt, hw_ctrl_cfg.db_burst_cnt,
			hw_ctrl_cfg.qp_wqe_cnt);
	}

	ret = ioctl(hrping_test.dev_fd, HH_WRITE_CMN_CFG, &hw_ctrl_cfg);
        if (ret)
		printf("ioctl failed: %d\n", __LINE__);

	DEBUG_LOG("HH control config done\n");
        return ret;
}

int reset_hw_hsk(void)
{
        int ret;

	ret = ioctl(hrping_test.dev_fd, HH_RESET_OP);
        if (ret)
		printf("ioctl failed: %d\n", __LINE__);

        return ret;
}

int hh_perf_read(struct perf_read_str *perf, int qp_num)
{
        int ret;

	perf->qp_num = qp_num;
	ret = ioctl(hrping_test.dev_fd, PERF_READ_OP, perf);
	if (ret)
		printf("ioctl failed: %d\n", __LINE__);
        return ret;
}

int main(int argc, char *argv[ ])
{
	unsigned int i, j;
	int	err, err_cnt = 0;
	struct perf_read_str perf_read;
	unsigned long long int clk_cnt;
	float qp_bw, tot_bw = 0;
	struct ctx_list *temp;

	if (argc == 1) {
		print_help();
		return 0;
	}

	if (init_xperf_app(&hrping_test, argc, argv)) {
		printf("Failed to initialize app\n");
		return -EFAULT;
	}

	if (hrping_test.param.help) {
		print_help();
		return 0;
	}

	hrping_test.rx_test_qp_cnt = hrping_test.param.qp_cnt;
#if 0
	/* TODO: BRAM is for ERNIC to ERNIC testing*/
	init_bram();
#endif

	err = fill_hh_ctrl_cfg(&hrping_test);
	if (err) {
		printf("Failed to write common hw handshake config\n");
		return err;
	}

	err = run_server(&hrping_test);
	if (err)
		return err;

	err = enable_hw_hs();
	if (err) {
		printf("Failed to enable HW HS\n");
		return err;
	}

	err = test_hw_handshake();
	if (err) {
		printf("Failed to test hw handshake\n");
		return err;
	}

	for (i = 0; i < hrping_test.param.qp_cnt; i++) {
		err = hh_perf_read(&perf_read, i);
		if (err) {
			printf("Error while reading performance counters for QP#%d\n", i);
			err_cnt++;
			continue;
		}
		DEBUG_LOG("QP[%d] clk cnts are [%#lx]\n",
				i, perf_read.clk_cnt);

		clk_cnt = perf_read.clk_cnt;
		/*
		* CPU clock is MHz and to convert the output to Gbps
		* the data size must be divided by 1024. The app receives
		* data size in KBs, which must be multiplied 1ith 1024 to
		* get data size, so both of them are removed to simplify
		*/
		qp_bw = (float)((uint64_t)hrping_test.param.data_size_kb *\
					8 * hrping_test.param.cpu_clk)\
					/ ((uint64_t)clk_cnt);
		qp_bw = qp_bw * perf_read.cq_cnt;
		DEBUG_LOG("BW used by QP[%d] is %0.3f Gbps\n", i, qp_bw);
		tot_bw += qp_bw;
	}

	if (err_cnt) {
		printf("Failed to read perf counters for %d QPs\n", err_cnt);
		printf("Remaining QPs have used %0.2f Gbps\n", tot_bw);
	} else
		printf("Total BW used is: %0.2f Gbps\n", tot_bw);

	err = reset_hw_hsk();
	if (err)
		printf("Failed to reset hw handshake\n");
	/* wait for the disconnects to come */

	temp = hrping_test.head;
	if (!temp)
		return 0;
	for (i = 0; i < hrping_test.param.qp_cnt; i++) {
		if (temp->ctx.send_buf_chunk_id > 0) {
			if (temp->ctx.rping_send_buf > 0)
				err = xib_umem_free_mem(temp, temp->ctx.send_buf_chunk_id,
					(uint64_t)temp->ctx.rping_send_buf,
					sizeof(struct rping_rdma_info));
			err |= xib_umem_free_chunk(temp, temp->ctx.send_buf_chunk_id);
		}

		if (temp->ctx.sq_mem_chunk_id > 0) {
			if (temp->ctx.sq_addr > 0)
				err |= xib_umem_free_mem(temp, temp->ctx.sq_mem_chunk_id,
					(uint64_t)temp->ctx.sq_addr,
					hrping_test.param.queue_depth * XRNIC_WQE_SIZE);

			err |= xib_umem_free_chunk(temp, temp->ctx.sq_mem_chunk_id);
		}

		if (temp->ctx.rdma_buf_chunk_id > 0) {
			if (temp->ctx.rdma_buf_addr > 0)
				err |= xib_umem_free_mem(temp, temp->ctx.rdma_buf_chunk_id,
					(uint64_t)temp->ctx.rdma_buf_addr,
					(hrping_test.param.data_size_kb) * 1024);

			err |= xib_umem_free_chunk(temp, temp->ctx.rdma_buf_chunk_id);
		}
		if (err)
			printf("Failed to free memory or chunk\n");

		rdma_disconnect(temp->ctx.cm_id);
		rdma_destroy_qp(temp->ctx.cm_id);
		ibv_destroy_cq(temp->ctx.cq);
		rdma_destroy_id(temp->ctx.cm_id);
		temp = temp->next;
		if (!temp)
			break;
	}
#if 0
	rdma_destroy_event_channel(hrping_test.cm_channel);
#endif
	rdma_destroy_id(hrping_test.cm_id);
	return 0;
}
