/*
 * Copyright (c) 2005 Ammasso, Inc. All rights reserved.
 * Copyright (c) 2006 Open Grid Computing, Inc. All rights reserved.
 * Copyright (c) 2020 Xilinx, Inc. All rights reserved.
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

static int debug = 0;
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
	pthread_t cq_thread;
	struct ibv_comp_channel *channel;
	struct ibv_cq *cq;
	struct ibv_pd *pd;
	struct ibv_qp *qp;

	struct ibv_recv_wr rq_wr;	/* recv work request record */
	struct ibv_sge recv_sgl;	/* recv single SGE */
	struct hh_send_fmt recv_buf;/* malloc'd buffer */
	struct ibv_mr *recv_mr, *send_mr;

	struct ibv_send_wr sq_wr;	/* send work request record */
	struct ibv_sge send_sgl;
	struct rping_rdma_info send_buf;/* single send buf */

	struct ibv_send_wr rdma_sq_wr;	/* rdma work request record */

	enum test_state state;		/* used for cond/signalling */
	enum test_state disc_state;		/* used for cond/signalling */
	sem_t sem;

	/* CM stuff */
	struct rdma_event_channel *cm_channel;
	struct rdma_cm_id *cm_id;	/* connection on client side,*/
					/* listener on service side. */
	int count;
	struct rdma_cm_event *event;
	struct ibv_wc *wc;
	volatile int cm_fail, cq_fail, cm_chnl_en, cm_established, test_done, disc_rx, disc_check_en;
	volatile int poll_pkt_cnt;
	unsigned int qp_num;

	void	*hh_va;
	uint32_t	hh_data_size, hh_opcode, wqe_cnt;
	struct	ibv_mr*	hh_mr;
	struct hh_send_fmt hh_info;/* malloc'd buffer */
	volatile int		rx_wqe_ready, rq_depth;
	volatile uint32_t	rx_compl, rx_posted;
	int wr_id;
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
	struct rdma_event_channel *cm_channel;
} rping_test;

#define HH_MAX_Q_DEPTH 16
enum hh_opcode {
	HH_RDMA_WRITE_OP,
	HH_RDMA_READ_OP,
	HH_RDMA_SEND_OP,
};

#define HH_MAX_QUEUE_DEPTH	16
#define HH_CQ_RQ_DEPTH		(8192)
#define HH_RX_REPOST_WQE_CNT	256
#define HH_SQ_DEPTH		1000


int hh_poll_cq(struct rping_cb *rdma_ctx)
{
	void* cq_ctx;
	struct ibv_cq* cq_event;
	uint32_t ret = 0;

	while (1) {
		if ((ret = ibv_poll_cq(rdma_ctx->cq, HH_RX_REPOST_WQE_CNT, rdma_ctx->wc)) < 0)
			return -1;
		return ret;
	}
}

int hh_post_rx_wqes(struct rping_cb *ctx, int size, int cnt)
{
	int i, ret = 0, r = ctx->rx_posted;
	uint32_t free_wqe;
	struct ibv_recv_wr *bad_wr;

	free_wqe = ctx->rq_depth - ctx->rx_posted + ctx->rx_compl;
	if (free_wqe < cnt) {
		DEBUG_LOG("No enuf wqes to post all the requested. Posting only %d\n", free_wqe);
		cnt = free_wqe;
	}
	for (i = 0; i < cnt; i++) {
		ret = rdma_post_recv(ctx->cm_id, (void *)(uintptr_t)(r + i),
					ctx->hh_va, size, ctx->hh_mr);
		if (ret < 0) {
			printf("HH post rx failed\n");
			break;
		}
	}
	ctx->rx_posted += i;
	DEBUG_LOG("Total RQs posted are %d\n", ctx->rx_posted);
	return i;
}

int poll_cq(struct rping_cb *cb, int id)
{
        struct ibv_wc wc;
        int ret;
	struct ibv_cq* cq_event;
	void* cq_ctx;

        while(1) {
		if ((ret = ibv_poll_cq(cb->cq, 1, &wc)) != 1) {
			if (ret < 0)
				return ret;
			continue;
		}

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
		cb->disc_state = DISCONNECTED;
		rping_test.done_cnt++;
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


static int rping_setup_wr(struct rping_cb *cb)
{
	cb->recv_mr = ibv_reg_mr(cb->pd, &cb->recv_buf, sizeof cb->recv_buf,
				IBV_ACCESS_LOCAL_WRITE);
	if (!cb->recv_mr) {
		fprintf(stderr, "recv_buf reg_mr failed\n");
		return -EFAULT;
	}

	cb->send_mr = ibv_reg_mr(cb->pd, &cb->send_buf, sizeof cb->send_buf, 0);
	if (!cb->send_mr) {
		fprintf(stderr, "send_buf reg_mr failed\n");
		ibv_dereg_mr(cb->recv_mr);
		return -EFAULT;
	}

	cb->recv_sgl.addr = (uint64_t) (unsigned long) &cb->recv_buf;
	cb->recv_sgl.length = sizeof cb->recv_buf;
	cb->recv_sgl.lkey =  cb->recv_mr->lkey;
	cb->rq_wr.sg_list = &cb->recv_sgl;
	cb->rq_wr.num_sge = 1;

	cb->send_sgl.addr = (uint64_t) (unsigned long) &cb->send_buf;
	cb->send_sgl.length = sizeof cb->send_buf;
	cb->send_sgl.lkey = cb->send_mr->lkey;

	cb->sq_wr.opcode = IBV_WR_SEND;
	cb->sq_wr.send_flags = IBV_SEND_SIGNALED;
	cb->sq_wr.sg_list = &cb->send_sgl;
	cb->sq_wr.num_sge = 1;
	return 0;
}

static int rping_setup_buffers(struct rping_cb *cb)
{
	int ret, r = rand();

	if (ret = rping_setup_wr(cb))
		return ret;

	cb->wr_id = rand();
	ret = rdma_post_recv(cb->cm_id, (void *)(uintptr_t)cb->wr_id,
					&cb->recv_buf, sizeof(cb->recv_buf), cb->recv_mr);
	if (ret < 0) {
		printf("Failed to post recv %s:%d\n", __func__, __LINE__);
		return ret;
	}

	DEBUG_LOG("allocated & registered buffers...\n");
	return 0;
}

static int rping_create_qp(struct rping_cb *cb)
{
	struct ibv_qp_init_attr init_attr;
	int ret;

	memset(&init_attr, 0, sizeof(init_attr));
	init_attr.cap.max_send_wr = HH_SQ_DEPTH;
	init_attr.cap.max_recv_wr = HH_CQ_RQ_DEPTH;
	init_attr.cap.max_recv_sge = 1;
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
		goto err2;
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
err2:
	ibv_destroy_comp_channel(cb->channel);
err1:
	ibv_dealloc_pd(cb->pd);
	return ret;
}

static void *data_thread(void *arg)
{
	struct rping_test_info *rping_info = arg;
	struct rping_cb *cb;
	int ret, i, cnt = 0;

	while(1) {
		for (i = 0; i < rping_info->qp_cnt; i++) {
			cb = &rping_info->cb[i];
			if (cb->cm_established && (!cb->test_done)) {
				/* start the data transfers */
       				ret = rping_test_client(cb);
       				if (!ret)
					printf("Successfully exchanged context of app QP #%d\n", i);
				cb->test_done = 1;
				/* Once the test is over, just disable the channel to
					avoid hearing for events on the channel */
				cb->cm_chnl_en = 0;
				cnt++;
				if (cnt >= rping_info->qp_cnt)
					return NULL;
			}
		}
		pthread_yield();
	}
}

static void *disc_thread(void *arg)
{
	struct rping_test_info *rping_info = arg;
	struct rping_cb *cb;
	int ret, i, qp_cnctd_cnt = 0;

	DEBUG_LOG("%s started\n", __func__);
	while (1) {
		for (i = 0; i < rping_info->qp_cnt; i++) {
			cb = &rping_info->cb[i];
			if ((!cb->disc_rx) && cb->disc_check_en) {
				ret = rdma_get_cm_event(cb->cm_channel, &cb->event);
				if (ret) {
					printf("rdma_get_cm_event failed CB=%#x", (uint32_t) (uintptr_t)cb);
					cb->disc_rx = 1;
					continue;
				}

				ret = rping_cma_event_handler(cb->event->id, cb->event);
				rdma_ack_cm_event(cb->event);
				if (ret) {
					printf("Ack cm event failed for %#x\n", (uint32_t)(uintptr_t)cb);
					cb->disc_rx = 1;
				}
				if (cb->disc_state == DISCONNECTED) {
					cb->disc_rx = 1;
					qp_cnctd_cnt++;
					if (qp_cnctd_cnt >= rping_info->qp_cnt) {
						exit(0);
					}
				}
			}
		}
	}
}

static void *cm_thread(void *arg)
{
	struct rping_test_info *rping_info = arg;
	struct rping_cb *cb;
	int ret, i, qp_cnctd_cnt = 0;
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

static void *cq_thread(void *arg)
{
	struct rping_cb *cb = arg;
	struct ibv_cq *ev_cq;
	void *ev_ctx;
	int ret, i;

	while (!cb->rx_wqe_ready);
	DEBUG_LOG("cq_thread started.\n");
	while (1) {
		 if (!cb->rx_posted && !cb->rx_compl) {
			pthread_yield();
		}
		if ((!cb->cq_fail) && cb->cm_established) {
			ret = hh_poll_cq(cb);
			if (ret <= 0) {
				continue;
			}

			cb->rx_compl += ret;
			DEBUG_LOG("Total completions rx are %d\n", cb->rx_compl);
			ret = hh_post_rx_wqes(cb, cb->hh_data_size, HH_RX_REPOST_WQE_CNT);
		}
	}
}

static void rping_format_send(struct rping_cb *cb, char *buf, struct ibv_mr *mr)
{
	struct rping_rdma_info *info = &cb->send_buf;

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
	int r = cb->wr_id, val = 1;
	struct hh_send_fmt *hh_data;

	ret = poll_cq(cb, r);
	if (ret < 0) {
		printf("Poll CQ failed %s:%d\n", __func__, __LINE__);
		return ret;
	}

	hh_data = &cb->recv_buf;
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
	cb->hh_va = malloc(cb->hh_data_size);
	if (!cb->hh_va) {
		printf("Failed to alloc hw hs buffer\n");
		return -ENOMEM;
	}
	cb->hh_mr = ibv_reg_mr(cb->cm_id->pd, cb->hh_va, cb->hh_data_size,
				IBV_ACCESS_LOCAL_WRITE |
				IBV_ACCESS_REMOTE_READ |
				IBV_ACCESS_REMOTE_WRITE);
	if (!cb->hh_mr) {
		printf("reg mr failed for hh buffer\n");
		return -EFAULT;
	}

	DEBUG_LOG("buf is %#lx, rkey is 0x%x\n",
			((uint64_t)(uintptr_t)cb->hh_va), cb->hh_mr->rkey);

	cb->send_buf.buf = htobe64(((uint64_t)(uintptr_t)cb->hh_va));
	cb->send_buf.rkey = ntohl(cb->hh_mr->rkey);
	if (hh_data->opcode == HH_RDMA_SEND_OP) {
		cb->wc = malloc(HH_RX_REPOST_WQE_CNT * sizeof (struct ibv_wc));
		if (!cb->wc) {
			printf("Failed to create WC entries\n");
			return -ENOMEM;
		}
		/* create a per QP cq thread */
		ret = pthread_create(&cb->cq_thread, NULL, cq_thread,
                                        (void *)cb);
		if (ret) {
			printf("Failed to create cq thread\n");
			return ret;
		}

		ret = hh_post_rx_wqes(cb, cb->hh_data_size, cb->rq_depth);
		if (ret < 0) {
			printf("Unable to post %d Rx WQEs\n", cb->rq_depth);
			return ret;
		}
	}

	cb->wqe_cnt = hh_data->wqe_cnt;
	cb->hh_opcode = hh_data->opcode;

	ret = rdma_post_send(cb->cm_id, (void *)(uintptr_t)r, &cb->send_buf,
					sizeof(cb->send_buf),
					cb->send_mr, IBV_SEND_SIGNALED);
	if (ret < 0) {
		printf("send failed %s:%d\n", __func__, __LINE__);
		return ret;
	}

	ret = poll_cq(cb, r);
	if (ret < 0) {
		printf("Poll CQ failed %s:%d\n", __func__, __LINE__);
		return ret;
	}

	cb->rx_wqe_ready = 1;

	return ret;
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
	printf("\t-I\t:Source address to bind to for client.\n");
	printf("\t-d\t:debug prints\n");
	printf("\t-S <size> \t:ping data size\n");
	printf("\t-a <ip-addr> \t: Server IP address\n");
	printf("\t-p <port> \t:port number\n");
	printf("\t-Q <val> : QP count\n");
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

	opterr = 0;

	if (argc == 1) {
		usage(argv[0]);
		return 0;
	}

	while ((op=getopt(argc, argv, "I:p:S:dQ:a:h")) != -1) {
		switch (op) {
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
		case 'I':
			ret = get_addr(optarg, (struct sockaddr *) &rping_test.ssource);
			break;
		case 'p':
			rping_test.port = htobe16(atoi(optarg));
			DEBUG_LOG("port %d\n", rping_test.port);
			break;
		case 'S':
			rping_test.size = atoi(optarg);
			if ((rping_test.size < RPING_MIN_BUFSIZE) ||
			    (rping_test.size > (RPING_BUFSIZE - 1))) {
				printf( "Invalid size %d "
				       "(valid range is %zd to %d)\n",
				       rping_test.size, RPING_MIN_BUFSIZE, RPING_BUFSIZE);
				ret = EINVAL;
			} else
				DEBUG_LOG("size %d\n", (int) atoi(optarg));
			break;
		case 'd':
			debug++;
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
	ret = pthread_create(&rping_test.cmthread, NULL, cm_thread, &rping_test);
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
		memset(cb, 0, sizeof(struct rping_cb));
		/* reset to defaults */
		sem_init(&cb->sem, 0, 0);
		cb->qp_num = i;
		cb->cm_fail = cb->cq_fail = 0;
		cb->cm_chnl_en = 0;
		cb->cm_established = 0;
		cb->rx_compl = cb->rx_posted = 0;
		cb->count = rping_test.count;
		cb->test_done = 0;
		cb->disc_rx = 0;
		cb->disc_state = 0;
		cb->disc_check_en = 0;
		cb->cm_channel = rping_test.cm_channel;

		ret = rping_setup_connection(cb);
		if (ret) {
			printf("QP connection failed for application QP#%d\n", i);
			/* Disable looking for events on this channel */
			cb->cm_chnl_en = 0;
			rping_test.done_cnt++;
		}
		DEBUG_LOG("init done for app qp #%d\n", i);
	}

	while(rping_test.done_cnt < rping_test.qp_cnt);
	return 0;
}
