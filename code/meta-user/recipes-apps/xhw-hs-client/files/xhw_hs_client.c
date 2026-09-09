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
#define _GNU_SOURCE
#include <endian.h>
#include <getopt.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <string.h>
#include <ctype.h>
#include <stdio.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <semaphore.h>
#include <pthread.h>
# include <inttypes.h>
#include <sys/mman.h>
#include <rdma/rdma_cma.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <rdma/rdma_verbs.h>
#include <umm_export.h>

unsigned int mem_type = XMEM_PL_DDR;
static int debug = 0;
bool common_rdma_buf = false;
#define DEBUG_LOG if (debug) printf

#define HH_DEV_NAME	"/dev/xib0"
int update_sgl_data(struct ibv_qp *ibvqp, char* buf, int len);
#define XIB_ERNIC
#define XIB_DMA_MEM_ALLOC_FIX

#define XIB_MEM_ALIGN	4096
#if (XIB_MEM_ALIGN == 64)
	#define XIB_BIT_SHIFT 6
#elif (XIB_MEM_ALIGN == 4096)
	#define XIB_BIT_SHIFT 12
#endif

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
	RDMA_READ_ADV,
	RDMA_READ_COMPLETE,
	RDMA_WRITE_ADV,
	RDMA_WRITE_COMPLETE,
	DISCONNECTED,
	HW_HS_RECV_CMPLT,
	ERROR
};

struct rping_rdma_info {
	uint64_t buf;
	uint32_t rkey;
	uint32_t size;
	uint32_t iter_cnt;
	uint32_t qp_cnt;
};

/*
 * Default max buffer size for IO...
 */
#define RPING_BUFSIZE 64*1024

/* Default string for print data and
 * minimum buffer size
 */
#define _stringify( _x ) # _x
#define stringify( _x ) _stringify( _x )

#define RPING_MSG_FMT           "rdma-ping-%d: "
#define RPING_MIN_BUFSIZE       sizeof(stringify(INT_MAX)) + sizeof(RPING_MSG_FMT)

struct hh_send_fmt {
	int	tf_siz;
	int	opcode;
	int	qp_cnt;
	int     wqe_cnt;
};

/*
 * Control block struct.
 */
struct rping_cb {
	struct ibv_comp_channel *channel;
	struct ibv_cq *cq;
	struct ibv_pd *pd;
	struct ibv_qp *qp;

	struct ibv_recv_wr rq_wr;	/* recv work request record */
	struct ibv_sge recv_sgl;	/* recv single SGE */
	struct hh_send_fmt *recv_buf;/* malloc'd buffer */
	struct ibv_mr *recv_mr, *send_mr;

	struct ibv_send_wr sq_wr;	/* send work request record */
	struct ibv_sge send_sgl;
	struct rping_rdma_info *send_buf;/* single send buf */

	struct ibv_send_wr rdma_sq_wr;	/* rdma work request record */

	enum test_state state;		/* used for cond/signalling */
	sem_t sem;

	/* CM stuff */
	struct rdma_event_channel *cm_channel;
	struct rdma_cm_id *cm_id;	/* connection on client side,*/
					/* listener on service side. */
	int count;
	struct rdma_cm_event *event;
	struct ibv_wc *wc;
	volatile int cm_fail, cq_fail, cm_chnl_en, cm_established, test_done;
	volatile int poll_pkt_cnt;
	unsigned int qp_num;

	uint64_t	hh_va;
	uint64_t	hh_data_size;
	uint32_t	hh_opcode, wqe_cnt;
	struct	ibv_mr*	hh_mr;
	struct hh_send_fmt hh_info;/* malloc'd buffer */
	volatile int		rx_wqe_ready, rq_depth;
	volatile uint32_t	rx_compl, rx_posted;
	uint32_t	rq_size_in_kb;
	int send_buf_chunk_id, rdma_buf_chunk_id;
};

struct rping_test_info {
	struct rping_cb	*cb;
	pthread_t cmthread;
	pthread_t disc_thread;
	pthread_t cqthread;
	pthread_t datathread;
	int	qp_cnt;
	int	size;
	int	count;
	int	verbose;
	int	validate;
	struct sockaddr_storage sin, ssource;
	uint16_t port;			/* dst port in NBO */
	int	cq_init;
	volatile int done_cnt;
	volatile int tf_siz;
	int	dev_fd;
	uint32_t	rq_size_in_kb;
	struct rdma_event_channel *cm_channel;
} rping_test;

#define HH_MAX_Q_DEPTH 16
enum hh_opcode {
	HH_RDMA_WRITE_OP,
	HH_RDMA_READ_OP,
	HH_RDMA_SEND_OP,
};

#define HH_MAX_QUEUE_DEPTH	64
#define HH_RX_REPOST_WQE_CNT	16
#define HH_SQ_DEPTH		32
#define HH_CQ_RQ_DEPTH		HH_SQ_DEPTH

int poll_cq(struct rping_cb *cb, int id)
{
        struct ibv_wc wc;
        int ret;
	struct ibv_cq* cq_event;
	void* cq_ctx;

        while(1) {
		if ((ret = ibv_poll_cq(cb->cq, 1, &wc)) != 1)
			continue;

		if (wc.status != IBV_WC_SUCCESS) {
			printf("ernic wc status = %#x (%s), id = %lx:%d\n",
				wc.status,
				ibv_wc_status_str(wc.status),
				wc.wr_id, id);
			return -1;
		}

		if (wc.wr_id == id)
			break;
        }
	return 0;
}

int rping_test_client(struct rping_cb *cb);
static int rping_cma_event_handler(struct rdma_cm_id *cma_id,
				    struct rdma_cm_event *event)
{
	int ret = 0;
	struct rping_cb *cb = cma_id->context;

	DEBUG_LOG("cma_event type %s cma_id %p (%s)\n",
		  rdma_event_str(event->event), cma_id,
		  (cma_id == cb->cm_id) ? "parent" : "child");

	switch (event->event) {
	case RDMA_CM_EVENT_ADDR_RESOLVED:
		cb->state = ADDR_RESOLVED;
		ret = rdma_resolve_route(cma_id, 2000);
		if (ret) {
			cb->state = ERROR;
			perror("rdma_resolve_route");
			sem_post(&cb->sem);
		}
		break;

	case RDMA_CM_EVENT_ROUTE_RESOLVED:
		cb->state = ROUTE_RESOLVED;
		sem_post(&cb->sem);
		break;

	case RDMA_CM_EVENT_CONNECT_REQUEST:
		cb->state = CONNECT_REQUEST;
		sem_post(&cb->sem);
		break;

	case RDMA_CM_EVENT_ESTABLISHED:
		DEBUG_LOG("Connection established for QP #%d\n",
			cma_id->qp->qp_num);
		/*
		 * Server will wake up when first RECV completes.
		 */
		cb->state = CONNECTED;
		cb->cm_chnl_en = 0;
		sem_post(&cb->sem);
		break;

	case RDMA_CM_EVENT_ADDR_ERROR:
	case RDMA_CM_EVENT_ROUTE_ERROR:
	case RDMA_CM_EVENT_CONNECT_ERROR:
	case RDMA_CM_EVENT_UNREACHABLE:
	case RDMA_CM_EVENT_REJECTED:
		DEBUG_LOG("cma event %s, error %d\n",
			rdma_event_str(event->event), event->status);
		sem_post(&cb->sem);
		ret = -1;
		break;

	case RDMA_CM_EVENT_DISCONNECTED:
		cb->state = DISCONNECTED;
		sem_post(&cb->sem);
		break;

	case RDMA_CM_EVENT_DEVICE_REMOVAL:
		printf("cma detected device removal!!!!\n");
		cb->state = ERROR;
		sem_post(&cb->sem);
		ret = -1;
		break;

	default:
		DEBUG_LOG("unhandled event: %s, ignoring\n",
			rdma_event_str(event->event));
		break;
	}
	return ret;
}


static void rping_setup_wr(struct rping_cb *cb)
{
	int ret, chunk_id;
	uint64_t va, pa, data_size = sizeof(*cb->send_buf);
	bool proc_access = true;
	int r = rand();
	struct ibv_recv_wr *bad_recv_wr;

	/* Allocate send buf */
	chunk_id = xib_umem_alloc_chunk(cb->cm_id->verbs,
                        XMEM_PL_DDR, data_size, data_size,
                        proc_access);
        if (chunk_id < 0) {
		printf("Failed to alloc chunk %d\n", __LINE__);
		cb->send_buf_chunk_id = -EINVAL;
		return -EFAULT;
        }

	cb->send_buf_chunk_id = chunk_id;

	va = xib_umem_alloc_mem(cb->cm_id->verbs, chunk_id,
				data_size);
        if (XMEM_IS_INVALID_VADDR(va)) {
		printf("Failed to alloc memory from chunk\n");
		ret = xib_umem_free_chunk(cb->cm_id->verbs, chunk_id);
		cb->send_buf_chunk_id = -EINVAL;
		return -EFAULT;
	}

	cb->send_buf = (struct rping_rdma_info *)va;

	cb->recv_sgl.length = sizeof (*cb->recv_buf);
	cb->rq_wr.sg_list = &cb->recv_sgl;
	cb->rq_wr.num_sge = 1;

	cb->send_sgl.addr = (uint64_t) (unsigned long) cb->send_buf;
	cb->send_sgl.length = sizeof (*cb->send_buf);
	cb->send_sgl.lkey = chunk_id;

	cb->sq_wr.opcode = IBV_WR_SEND;
	cb->sq_wr.send_flags = IBV_SEND_SIGNALED;
	cb->sq_wr.sg_list = &cb->send_sgl;
	cb->sq_wr.num_sge = 1;

	/* 1. Post a recv */
	set_wr_id(&cb->rq_wr, r);
	cb->rq_wr.next = NULL;

	ret = ibv_post_recv(cb->cm_id->qp, &cb->rq_wr, &bad_recv_wr);
	if (ret < 0) {
		printf("Failed to post recv %s:%d\n", __func__, __LINE__);
		return ret;
	}
}

static int rping_setup_buffers(struct rping_cb *cb)
{
	int ret;

	rping_setup_wr(cb);
	DEBUG_LOG("allocated & registered buffers...\n");
	return 0;
}

static int rping_create_qp(struct rping_cb *cb)
{
	struct ibv_qp_init_attr init_attr;
	int ret = cb->rq_size_in_kb * 1024;

	memset(&init_attr, 0, sizeof(init_attr));
	init_attr.cap.max_send_wr = HH_SQ_DEPTH;
	init_attr.cap.max_recv_wr = HH_CQ_RQ_DEPTH;
	init_attr.cap.max_recv_sge = xib_get_rq_sge_cnt(ret);
	init_attr.cap.max_send_sge = 1;
	init_attr.qp_type = IBV_QPT_RC;
	init_attr.send_cq = cb->cq;
	init_attr.recv_cq = cb->cq;

	ret = rdma_create_qp(cb->cm_id, cb->pd, &init_attr);
	if (!ret)
		cb->qp = cb->cm_id->qp;

	return ret;
}

static void rping_free_qp(struct rping_cb *cb)
{
	ibv_destroy_qp(cb->qp);
	ibv_destroy_cq(cb->cq);
	ibv_dealloc_pd(cb->pd);
}

static int rping_setup_qp(struct rping_cb *cb, struct rdma_cm_id *cm_id)
{
	int ret;

	cb->pd = ibv_alloc_pd(cm_id->verbs);
	if (!cb->pd) {
		printf("ibv_alloc_pd failed\n");
		return errno;
	}
	DEBUG_LOG("created pd %p\n", cb->pd);

	cb->channel = NULL;

	cb->cq = ibv_create_cq(cm_id->verbs, HH_CQ_RQ_DEPTH, NULL,
				cb->channel, 0);
	if (!cb->cq) {
		printf( "ibv_create_cq failed\n");
		ret = errno;
		goto err1;
	}

	cb->rq_depth = HH_CQ_RQ_DEPTH;
	DEBUG_LOG("created cq %p\n", cb->cq);

	ret = rping_create_qp(cb);
	if (ret) {
		perror("rdma_create_qp");
		goto err3;
	}
	DEBUG_LOG("created qp %p\n", cb->qp);
	return 0;

err3:
	ibv_destroy_cq(cb->cq);
err1:
	ibv_dealloc_pd(cb->pd);
	return ret;
}

static void *data_thread(void *arg)
{
	struct rping_test_info *rping_info = arg;
	struct rping_cb *cb;
	int ret, i;

	while(1) {
		for (i = 0; i < rping_info->qp_cnt; i++) {
			cb = &rping_info->cb[i];
			if (cb->cm_established && (!cb->test_done)) {
				/* start the data transfers */
       				ret = rping_test_client(cb);
       				if (!ret)
					printf("QP #%d successfully exchanged context with server\n", i);
				cb->test_done = 1;
				/* Once the test is over, just disable the channel to
					avoid hearing for events on the channel */
				cb->cm_chnl_en = 0;
			}
		}
		pthread_yield();
	}
}

static void *cm_thread(void *arg)
{
	struct rping_test_info *rping_info = arg;
	struct rping_cb *cb;
	int ret, i;
	struct rdma_cm_event *event;

	DEBUG_LOG("cm thread started\n");
	while (1) {
		if (rdma_get_cm_event(rping_info->cm_channel, &event)) {
			printf("Failed to get CM events. Exiting the test\n");
			exit(EXIT_FAILURE);
		}

		cb = event->id->context;
		if (rping_cma_event_handler(event->id, event)) {
			printf("Failed to process CM event for QP [%d]\n", cb->qp_num);
			exit(EXIT_FAILURE);
		}

		if (rdma_ack_cm_event(event)) {
			printf("CM Ack failed\n");
			exit(EXIT_FAILURE);
		}

		if (cb->state == DISCONNECTED) {
			rping_info->done_cnt++;
			if (rping_info->done_cnt >= rping_info->qp_cnt)
				return NULL;
		}
	}
}

static void rping_format_send(struct rping_cb *cb, char *buf, struct ibv_mr *mr)
{
	struct rping_rdma_info *info = cb->send_buf;

	info->buf = htobe64((uint64_t) (unsigned long) buf);
	info->rkey = htobe32(mr->rkey);
	info->size = htobe32(rping_test.size);
	info->iter_cnt = htobe32(cb->count);
	info->qp_cnt = htobe32(rping_test.qp_cnt);
	DEBUG_LOG("RDMA addr %" PRIx64" rkey %x len %d\n",
		  be64toh(info->buf), be32toh(info->rkey), be32toh(info->size));
}

static void free_cb(struct rping_cb *cb)
{
	free(cb);
}

void set_wr_id(uint64_t *buf, int id)
{
	/* for both send & recv wr, first 64B are wr id */
	*buf = id;
}

int rping_test_client(struct rping_cb *cb)
{
	int i, ret = 0;
	static int chunk_id = -EINVAL;
	int r = rand(), val = 1;
	static uint64_t va, pa;
	struct hh_send_fmt *hh_data;
	struct ibv_recv_wr *bad_recv_wr;
	struct ibv_send_wr *bad_send_wr;
	bool proc_access = false;

	r = cb->rq_wr.wr_id;
	ret = poll_cq(cb, r);
	if (ret < 0) {
		printf("Poll CQ failed %s:%d\n", __func__, __LINE__);
		return ret;
	}

	hh_data = cb->rq_wr.sg_list[0].addr;
	DEBUG_LOG("Data transfer size is  %dkB \n",
			hh_data->tf_siz);

	r++;

	if ((!hh_data->tf_siz) || (hh_data->tf_siz > 8192)) {
		printf("Requested data transfer size %d kB is not supported\n",
			hh_data->tf_siz);
		return -EINVAL;
	}

	cb->hh_data_size = hh_data->tf_siz * 1024;
	/* Fill Send SGE */
	if (!(common_rdma_buf && chunk_id > 0)) {
		chunk_id = xib_umem_alloc_chunk(cb->cm_id->verbs,
	                        mem_type, cb->hh_data_size, cb->hh_data_size,
	                        proc_access);
	        if (chunk_id < 0) {
	                printf("Failed to alloc chunk %d\n", __LINE__);
			cb->rdma_buf_chunk_id = -EINVAL;
	                return -EFAULT;
	        }

		cb->rdma_buf_chunk_id = chunk_id;
		va = xib_umem_alloc_mem(cb->cm_id->verbs, chunk_id,
					cb->hh_data_size);
	        if (XMEM_IS_INVALID_VADDR(va)) {
			printf("Failed to alloc memory from chunk\n");
			ret = xib_umem_free_chunk(cb->cm_id->verbs, chunk_id);
			cb->rdma_buf_chunk_id = -EINVAL;
			return -EFAULT;
		}
	} else
		cb->rdma_buf_chunk_id = -EINVAL;
	cb->hh_va = va;

	cb->hh_mr = ibv_reg_mr_ex(cb->cm_id->pd, cb->hh_va, cb->hh_data_size,
				IBV_ACCESS_LOCAL_WRITE |
				IBV_ACCESS_REMOTE_READ |
				IBV_ACCESS_REMOTE_WRITE);
	if (!cb->hh_mr) {
		printf("reg mr failed for hh buffer\n");
		goto reg_mr_fail;
	}

	DEBUG_LOG("buf is %llx, rkey is 0x%x\n",
			cb->hh_va, cb->hh_mr->rkey);

	cb->send_buf->buf = htobe64(cb->hh_va);
	cb->send_buf->rkey = ntohl(cb->hh_mr->rkey);
	cb->wqe_cnt = hh_data->wqe_cnt;
	cb->hh_opcode = hh_data->opcode;

	set_wr_id(&cb->sq_wr, r);
	cb->sq_wr.next = NULL;

	if (hh_data->opcode == HH_RDMA_SEND_OP) {
		if (cb->hh_data_size > (cb->rq_size_in_kb * 1024)) {
			printf("Configured RQ buffer size is < HW HS test size\n");
			goto reg_mr_fail;
		}
	}

	ret = ibv_post_send(cb->qp, &cb->sq_wr, &bad_send_wr);
	if (ret < 0) {
		printf("send failed %s:%d\n", __func__, __LINE__);
		goto reg_mr_fail;
	}

	ret = poll_cq(cb, r);
	if (ret < 0) {
		printf("Poll CQ failed %s:%d\n", __func__, __LINE__);
		goto reg_mr_fail;
	}

	if (hh_data->opcode == HH_RDMA_SEND_OP) {
		struct ibv_qp_attr qp_attr;
                memset(&qp_attr, 0, sizeof(qp_attr));
                qp_attr.qp_state = IBV_QPS_RTS; // Assuming the QP is already in RTS state
                qp_attr.qp_access_flags = IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_ATOMIC;

		/* set Loop back mode HW HS */
		ret = ibv_modify_qp(cb->cm_id->qp, &qp_attr,
				IBV_ENABLE_QP_HW_ACCL);
		if (ret) {
			printf("Failed to configure QP acceleration\n");
			goto reg_mr_fail;
		}
	}

	return ret;
reg_mr_fail:
	if (cb->rdma_buf_chunk_id > 0) {
		ret |= xib_umem_free_mem(cb->cm_id->verbs, cb->rdma_buf_chunk_id,
				cb->hh_va, cb->hh_data_size);
		ret |= xib_umem_free_chunk(cb->cm_id->verbs, chunk_id);
		cb->rdma_buf_chunk_id = -EINVAL;
	}
	return -EFAULT;
}

static int rping_connect_client(struct rping_cb *cb)
{
	struct rdma_conn_param conn_param;
	int ret;

	memset(&conn_param, 0, sizeof conn_param);
	conn_param.responder_resources = 16;
	conn_param.initiator_depth = 16;
	conn_param.retry_count = 7;

	ret = rdma_connect(cb->cm_id, &conn_param);
	if (ret) {
		perror("rdma_connect");
		return ret;
	}

	sem_wait(&cb->sem);
	if (cb->state != CONNECTED) {
		printf( "wait for CONNECTED state %d\n", cb->state);
		return -1;
	}

	/* Enable data transasctions */
	cb->cm_established = 1;
	DEBUG_LOG("rmda_connect successful\n");
	return 0;
}

static int rping_bind_client(struct rping_cb *cb)
{
	int ret;

	if (rping_test.sin.ss_family == AF_INET)
		((struct sockaddr_in *) &rping_test.sin)->sin_port = rping_test.port;
	else
		((struct sockaddr_in6 *) &rping_test.sin)->sin6_port = rping_test.port;

	if (rping_test.ssource.ss_family)
		ret = rdma_resolve_addr(cb->cm_id, (struct sockaddr *) &rping_test.ssource,
					(struct sockaddr *) &rping_test.sin, 2000);
	else
		ret = rdma_resolve_addr(cb->cm_id, NULL, (struct sockaddr *) &rping_test.sin, 2000);

	if (ret) {
		printf("rdma_resolve_addr failed");
		return ret;
	}

	sem_wait(&cb->sem);
	if (cb->state != ROUTE_RESOLVED) {
		printf( "waiting for addr/route resolution state %d\n",
			cb->state);
		return -1;
	}

	DEBUG_LOG("rdma_resolve_addr - rdma_resolve_route successful\n");
	return 0;
}

static int rping_setup_connection(struct rping_cb *cb)
{
	struct ibv_recv_wr *bad_wr;
	int ret;
	static int i = 0;

	ret = rdma_create_id(cb->cm_channel, &cb->cm_id, cb, RDMA_PS_TCP);
	if (ret) {
		printf("ID creation failed for app qp #%d\n", i);
		rping_test.done_cnt++;
		return -EFAULT;
	}
	DEBUG_LOG("created cm_id %p\n", cb->cm_id);
	cb->cm_chnl_en = 1;

	ret = rping_bind_client(cb);
	if (ret)
		return ret;

	ret = rping_setup_qp(cb, cb->cm_id);
	if (ret)
		return ret;

	ret = rping_setup_buffers(cb);
	if (ret)
		goto err1;

	ret = rping_connect_client(cb);
	if (ret) {
		printf("connect error %d\n", ret);
		goto err1;
	}

	i++;
	return 0;
err4:
	rdma_disconnect(cb->cm_id);
err1:
	rping_free_qp(cb);

	i++;
	return ret;
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

	if (res->ai_family == PF_INET)
		memcpy(addr, res->ai_addr, sizeof(struct sockaddr_in));
	else if (res->ai_family == PF_INET6)
		memcpy(addr, res->ai_addr, sizeof(struct sockaddr_in6));
	else
		ret = -1;

	freeaddrinfo(res);
	return ret;
}

static void usage(const char *name)
{
	printf("\t--debug \t:debug prints\n");
	printf("\t-Q <count>  or --qp <count> \t: QP count\n");
	printf("\t-K <size> or --rq-size <count> \t: RQ buffer sizein KB\n"
		"\t\t\tApplication only for HW HS SEND BW test\n\n");
	printf("\t-a <ip-addr> or --ip <ip-addr> \t: IP adddress \n");
	printf("\t-R <type> or --data-mem <type> \t: Memory to be used for RDMA buffer\n"
		"\t\t\t example memory types are : pl, ps, eddr, bram\n");
	printf("\t--common-rdma-buf or -c  : Use same RDMA buffer for all QPs\n");
}

static struct option long_options[] = {
	{"help", no_argument, NULL, 'h'},
	{"rq-size", required_argument, NULL, 'K'},
	{"qp", required_argument, NULL, 'Q'},
	{"ip", required_argument, NULL, 'a'},
	{"data-mem", required_argument, NULL, 'R'},
	{"debug", no_argument, &debug, 1},
	{"common-rdma-buf", no_argument, NULL, 'c'},
	{0,0,0,0},
};

static void init_conn_defaults(struct rping_cb *cb)
{
	memset(cb, 0, sizeof(struct rping_cb));
	/* reset to defaults */
	sem_init(&cb->sem, 0, 0);
	cb->rq_size_in_kb = rping_test.rq_size_in_kb;
	cb->cm_fail = cb->cq_fail = 0;
	cb->cm_chnl_en = 0;
	cb->cm_established = 0;
	cb->rx_compl = cb->rx_posted = 0;
	cb->count = rping_test.count;
	cb->test_done = 0;
	cb->send_buf = (long)(-EINVAL);
	cb->send_buf_chunk_id = 0;
	cb->hh_va = XMEM_INVALID_CHUNK_ID;
	cb->rdma_buf_chunk_id = 0;
}

int main(int argc, char *argv[])
{
	struct rping_cb *cb;
	char *end;
	int op, i = 0;
	int ret = 0, data[3];
	float qp_bw, tot_bw = 0;
	int persistent_server = 0;
	unsigned long long int clk_cnt;

	rping_test.size = 64;
	rping_test.sin.ss_family = PF_INET;
	rping_test.port = htobe16(7176);
	rping_test.qp_cnt = 1;
	rping_test.cq_init = 0;
	rping_test.count = 0;
	rping_test.rq_size_in_kb = 4;

	opterr = 0;

	if (argc == 1) {
		usage(argv[0]);
		return 0;
	}

	while ((op = getopt_long(argc, argv, ":Q:a:R:hK:c", long_options, NULL)) != -1) {
		switch (op) {
		case 0:
			break;
		case 'c':
			common_rdma_buf = true;
			break;
		case 'a':
			ret = get_addr(optarg, (struct sockaddr *) &rping_test.sin);
		break;
		case 'Q':
			rping_test.qp_cnt = atoi(optarg);
			if (rping_test.qp_cnt <= 0) {
				printf("Invalid QP count \n");
				return -1;
			}
			break;
		case 'h':
			usage(argv[0]);
			return 0;
		case 'K':
			rping_test.rq_size_in_kb = atoi(optarg);
			printf("RQ size requested is %dKB\n",
						rping_test.rq_size_in_kb);
			if (!rping_test.rq_size_in_kb) {
				printf("RQ size can't be 0. Setting it 4KB\n");
				rping_test.rq_size_in_kb = 4;
			}
			break;
		case 'R':
			if ((ret = xib_mem_str_to_mem_type(optarg)) < 0)
				return ret;
			mem_type = ret;
			break;
		default:
			printf("No option found %c\n", op);
			usage("rping");
			return -EINVAL;
		}
	}

	i = sizeof(struct rping_cb) * rping_test.qp_cnt;
	rping_test.cb = malloc(i);
	if (!rping_test.cb) {
		printf("Failed to allocate memory for contexts\n");
		return -ENOMEM;
	}
	memset(rping_test.cb, 0, i);

	rping_test.cm_channel = rdma_create_event_channel();
	if (!rping_test.cm_channel) {
		printf("event channel creation failed\n");
		return -EFAULT;
	}

	/* create reqd threads */
	ret = pthread_create(&rping_test.cmthread, NULL, cm_thread,
				&rping_test);
	if (ret) {
		perror("failed to create cm thread");
		free(rping_test.cb);
		return -1;
	}


	ret = pthread_create(&rping_test.datathread, NULL, data_thread, &rping_test);
	if (ret) {
		perror("Failed to create data transfer threads\n");
		free(rping_test.cb);
		return -1;
	}

	for (i = 0; i < rping_test.qp_cnt; i++) {
		cb = &rping_test.cb[i];
		init_conn_defaults(cb);
		cb->qp_num = i;
		cb->cm_channel = rping_test.cm_channel;

		ret = rping_setup_connection(cb);
		if (ret) {
			printf("QP connection failed for application QP#%d\n", i);
			/* Disable looking for events on this channel */
			cb->cm_chnl_en = 0;
			rping_test.done_cnt++;
		}
		DEBUG_LOG("Connection established for app qp #%d\n", i);
	}

	while(rping_test.done_cnt < rping_test.qp_cnt);

	ret = 0;
	for (i = 0; i < rping_test.qp_cnt; i++) {
		cb = &rping_test.cb[i];
		/* Free memory & then chunk */
		if (cb->send_buf_chunk_id > 0) {
			if (((long)(cb->send_buf)) > 0)
				ret = xib_umem_free_mem(cb, cb->send_buf_chunk_id,
					cb->send_buf, sizeof(struct rping_rdma_info));
			ret |= xib_umem_free_chunk(cb, cb->send_buf_chunk_id);
		}
		if (cb->rdma_buf_chunk_id > 0) {
			if (cb->hh_va > 0)
				ret |= xib_umem_free_mem(cb, cb->rdma_buf_chunk_id,
					cb->hh_va, cb->hh_data_size);

			ret |= xib_umem_free_chunk(cb, cb->rdma_buf_chunk_id);
		}
	}
	return ret;
}
