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
#include <string.h>
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
#include <stdbool.h>
#include <rdma/rdma_verbs.h>


#ifndef _XRPING_H
#define _XRPING_H

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
#define HH_CQ_RQ_DEPTH	(8 * 1024 + 32)
#define RPING_CQ_RQ_DEPTH 16
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
	uint8_t recv_buf[PAYLOAD_SIZE];/* malloc'd buffer */
	struct ibv_mr *recv_mr;		/* MR associated with this buffer */

	struct ibv_send_wr sq_wr;	/* send work request record */
	struct ibv_sge send_sgl;
	uint8_t send_buf[PAYLOAD_SIZE];/* single send buf */
	struct ibv_mr *send_mr;

	struct ibv_send_wr rdma_sq_wr;	/* rdma work request record */
	struct ibv_sge rdma_sgl;	/* rdma single SGE */
	char *rdma_buf;			/* used as rdma sink */
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
	char *rdma_buf_ofs;
	pthread_mutex_t	cm_finish_lock;
	int count;
	struct rdma_cm_event *event;
	volatile int cm_fail, cq_fail, cm_chnl_en, cm_established, test_done;
	volatile int poll_pkt_cnt;
	unsigned int qp_num;
	int wr_id;
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
        pthread_mutex_t cm_list_lock;
        volatile int rx_test_qp_cnt;
        int qp_cnt, test_qp_cnt, wqe_cnt;;
	int	server;
	int	size;
	int	count;
	int	verbose;
	int	validate;
	unsigned int	cpu_clk;
	unsigned int line_rate;
	struct sockaddr_storage sin, ssource;
	uint16_t port;			/* dst port in NBO */
	int	cq_init;
	volatile int done_cnt;
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

int app_req_regions(struct rping_cb* rdma_ctx);
int init_server_cm(struct rping_test_info* srping);
int set_ctx_to_rx_connect_req(struct rping_test_info *srping,
				struct rdma_cm_id *id,
				enum rdma_cm_event_type event);
static int debug = 0;
#define DEBUG_LOG if (debug) printf
int iter_cnt = 1;
struct rping_test_info srping_test;
#define RPING_SQ_DEPTH 16

#define error_handler(x)        do {                                    \
                                        char str[100];                  \
                                        sprintf(str, "^[[0;31m%s",x);   \
                                        perror(str);                    \
                                        printf("^[[0;m");               \
                                        exit(EXIT_FAILURE);             \
                                } while(0);


int rping_test_client(struct rping_cb *cb);
static int rping_cma_event_handler(struct rdma_cm_id *cma_id,
				    struct rdma_cm_event *event)
{
	int ret = 0;
	struct rping_cb *cb = cma_id->context;

	DEBUG_LOG("cma_event type %s cma_id %p \n",
		  rdma_event_str(event->event), cma_id);

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
		if (!rping_test.server) {
			cb->state = CONNECTED;
		}
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
		DEBUG_LOG("%s DISCONNECT EVENT...\n",
			rping_test.server ? "server" : "client");
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

static int client_recv(struct rping_cb *cb, struct ibv_wc *wc)
{
	if (wc->byte_len != sizeof(cb->recv_buf)) {
		printf("Received bogus data, size %d\n", wc->byte_len);
		return -1;
	}

	if (cb->state == RDMA_READ_ADV)
		cb->state = RDMA_WRITE_ADV;
	else
		cb->state = RDMA_WRITE_COMPLETE;

	return 0;
}

static int rping_cq_event_handler(struct rping_cb *cb)
{
	#define WC_CNT	10
	struct ibv_wc wc[WC_CNT];
	struct ibv_recv_wr *bad_wr;
	int ret;
	int flushed = 0, i = 0, loop = 0;

	while (1) {
		ret = 0;
		if (i >= 3) {
			cb->poll_pkt_cnt = 0;
			return 0;
		}
		i++;
		ret = ibv_poll_cq(cb->cq, WC_CNT, wc);
		if (ret <= 0)
			continue;
		loop = 0;

		cb->poll_pkt_cnt = ret;
		while (loop < ret) {
			if (wc[loop].status) {
				if (wc[loop].status == IBV_WC_WR_FLUSH_ERR) {
					flushed = 1;
					continue;
	
				}
				printf("cq completion failed status %d\n",
					wc[loop].status);
				ret = -1;
				goto error;
			}
	
			switch (wc[loop].opcode) {
			case IBV_WC_SEND:
				DEBUG_LOG("send completion\n");
				break;
			case IBV_WC_RDMA_WRITE:
				DEBUG_LOG("rdma write completion\n");
				cb->state = RDMA_WRITE_COMPLETE;
				sem_post(&cb->sem_data);
				break;
	
			case IBV_WC_RDMA_READ:
				DEBUG_LOG("rdma read completion\n");
				cb->state = RDMA_READ_COMPLETE;
				sem_post(&cb->sem_data);
				break;
	
			case IBV_WC_RECV:
				DEBUG_LOG("recv completion\n");
				ret = client_recv(cb, &wc[loop]);
				if (ret) {
					printf("recv wc error: %d\n", ret);
					goto error;
				}
		
				ret = ibv_post_recv(cb->qp, &cb->rq_wr, &bad_wr);
				if (ret) {
					printf("post recv error: %d\n", ret);
					goto error;
				}
				sem_post(&cb->sem_data);
				break;
	
			default:
				DEBUG_LOG("unknown!!!!! completion\n");
				ret = -1;
				goto error;
			}
			loop++;
		}
	}
	if (ret) {
		printf("poll error %d\n", ret);
		goto error;
	}
	return flushed;

error:
	cb->state = ERROR;
	sem_post(&cb->sem_data);
	return ret;
}

static void rping_setup_wr(struct rping_cb *cb)
{
	if (rping_test.server == 0) {
                cb->recv_mr = ibv_reg_mr(cb->pd, cb->recv_buf, sizeof cb->recv_buf,
                                        IBV_ACCESS_LOCAL_WRITE);
                if (!cb->recv_mr) {
                        fprintf(stderr, "recv_buf reg_mr failed\n");
                        ibv_dereg_mr(cb->recv_mr);
                        return;
                }

                cb->send_mr = ibv_reg_mr(cb->pd, cb->send_buf, sizeof cb->send_buf, IBV_ACCESS_LOCAL_WRITE);
                if (!cb->send_mr) {
                        fprintf(stderr, "send_buf reg_mr failed\n");
                        ibv_dereg_mr(cb->send_mr);
                        return;
                }
        }
        cb->recv_sgl.addr = (uint64_t) (unsigned long) cb->recv_buf;
        cb->recv_sgl.length = sizeof cb->recv_buf;
        cb->recv_sgl.lkey = cb->recv_mr->lkey;
        cb->rq_wr.sg_list = &cb->recv_sgl;
        cb->rq_wr.num_sge = 1;

        cb->send_sgl.addr = (uint64_t) (unsigned long) cb->send_buf;
        cb->send_sgl.length = sizeof cb->send_buf;
        cb->send_sgl.lkey = cb->send_mr->lkey;

        cb->sq_wr.opcode = IBV_WR_SEND;
        cb->sq_wr.send_flags = IBV_SEND_SIGNALED;
        cb->sq_wr.sg_list = &cb->send_sgl;
        if (rping_test.server == 0) {
                cb->sq_wr.num_sge = 1;
        }
	cb->rdma_sgl.lkey = cb->rdma_mr->lkey;
        cb->rdma_sq_wr.send_flags = IBV_SEND_SIGNALED;
        cb->rdma_sq_wr.sg_list = &cb->rdma_sgl;
        cb->rdma_sq_wr.num_sge = 1;
}

static int rping_setup_buffers(struct rping_cb *cb)
{
	int ret;

	cb->rdma_buf = malloc(rping_test.size);
	if (!cb->rdma_buf) {
		printf("rdma_buf malloc failed\n");
		ret = -ENOMEM;
		goto err2;
	}

	cb->rdma_buf_ofs = cb->rdma_buf;
	cb->rdma_mr = ibv_reg_mr(cb->pd, cb->rdma_buf, rping_test.size,
				 IBV_ACCESS_LOCAL_WRITE |
				 IBV_ACCESS_REMOTE_READ |
				 IBV_ACCESS_REMOTE_WRITE);
	if (!cb->rdma_mr) {
		printf("rdma_buf reg_mr failed\n");
		ret = errno;
		goto err3;
	}

	cb->start_buf = malloc(rping_test.size);
	if (!cb->start_buf) {
		printf("start_buf malloc failed\n");
		ret = -ENOMEM;
		goto err4;
	}
	rping_setup_wr(cb);
	DEBUG_LOG("allocated & registered buffers...\n");
	return 0;

err5:
	free(cb->start_buf);
err4:
	ibv_dereg_mr(cb->rdma_mr);
err3:
	free(cb->rdma_buf);
err2:
	ibv_dereg_mr(cb->send_mr);
err1:
	ibv_dereg_mr(cb->recv_mr);
	return ret;
}

static void rping_free_buffers(struct rping_cb *cb)
{
	ibv_dereg_mr(cb->rdma_mr);
	free(cb->rdma_buf);
	free(cb->start_buf);
}

static int rping_create_qp(struct rping_cb *cb)
{
	struct ibv_qp_init_attr init_attr;
	int ret;

	memset(&init_attr, 0, sizeof(init_attr));
	init_attr.cap.max_send_wr = RPING_SQ_DEPTH;
	init_attr.cap.max_recv_wr = 16;
	init_attr.cap.max_recv_sge = (PAYLOAD_SIZE / ERNIC_SGE_SIZE);
	init_attr.cap.max_send_sge = 1;
	init_attr.qp_type = IBV_QPT_RC;
	init_attr.send_cq = cb->cq;
	init_attr.recv_cq = cb->cq;

	ret = rdma_create_qp(cb->cm_id, cb->pd, &init_attr);
	if (!ret) {
		cb->qp = cb->cm_id->qp;
	}

	return ret;
}

static void rping_free_qp(struct rping_cb *cb)
{
	ibv_destroy_qp(cb->qp);
	ibv_destroy_cq(cb->cq);
	ibv_destroy_comp_channel(cb->channel);
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

	cb->cq = ibv_create_cq(cm_id->verbs, RPING_SQ_DEPTH * 2, cb,
				cb->channel, 0);
	if (!cb->cq) {
		printf( "ibv_create_cq failed\n");
		ret = errno;
	}
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

static int rping_setup_buffers_server(struct rping_cb *cb)
{
        int ret;

        cb->recv_mr = ibv_reg_mr(cb->cm_id->pd, cb->recv_buf,
                                       sizeof cb->recv_buf,
                                       IBV_ACCESS_LOCAL_WRITE);
        if (!cb->recv_mr) {
                printf("recv_buf reg_mr failed\n");
                return -1;
        }

        cb->send_mr = ibv_reg_mr(cb->cm_id->pd, cb->send_buf,
                            sizeof cb->send_buf, IBV_ACCESS_LOCAL_WRITE);
        if (!cb->send_mr) {
                printf("send_buf reg_mr failed\n");
                ret = errno;
                goto err1;
        }

        cb->rdma_buf = malloc(rping_test.size);
        if (!cb->rdma_buf) {
                fprintf(stderr, "rdma_buf malloc failed\n");
                ret = -ENOMEM;
        }

        cb->rdma_mr = ibv_reg_mr(cb->cm_id->pd, cb->rdma_buf, rping_test.size,
                                 IBV_ACCESS_LOCAL_WRITE |
                                 IBV_ACCESS_REMOTE_READ |
                                 IBV_ACCESS_REMOTE_WRITE);
        if (!cb->rdma_mr) {
                fprintf(stderr, "rdma_buf reg_mr failed\n");
                ret = errno;
                goto err3;
        }

        rping_setup_wr(cb);
        return 0;

err3:
        free(cb->rdma_buf);
err2:
        ibv_dereg_mr(cb->send_mr);
err1:
        ibv_dereg_mr(cb->recv_mr);
        return ret;
}

static void *data_thread(void *arg)
{
       struct rping_test_info *rping_info = arg;
        struct rping_cb *cb;
        int i = 0, ret;
        struct ctx_list *temp = NULL;
        struct ibv_qp_init_attr  init_attr;
        struct rdma_conn_param conn_param;

	if (rping_test.server == 0) {
        	while(1) {
                	for (i = 0; i < rping_info->qp_cnt; i++) {
                	        cb = &rping_info->cb[i];
                	        if (cb->cm_established && (!cb->test_done)) {
                	                /* start the data transfers */
                	                ret = rping_test_client(cb);
                	                if (ret) {
                	                        printf("rping test FAILED for app qp #%d\n", i);
                	                        /* disable the tranfers */
                	                        cb->cm_established = 0;
                	                } else
                	                        printf("rping test PASSED for app QP #%d\n", i);
                	                cb->cm_established = 0;
                	                cb->test_done = 1;
                	                /* Once the test is over, just disable the channel to
                	                        avoid hearing for events on the channel */
                	                cb->cm_chnl_en = 0;
                	                rping_test.done_cnt++;
                	        }
                	}
 			pthread_yield();
		}
	} else {
        	while(1) {
                	if (!temp)
                	        temp = rping_info->head;
                	while(temp && (i < 9)) {
                	        if (temp->state == CONNECT_REQ) {
                	                /* start processing the test */
                	                temp->ctx.cm_id = temp->client_cm_id;
                	                if (app_req_regions(&temp->ctx))
                	                        error_handler("request regions Failed:");
                	                init_attr.cap.max_send_wr = 16;
                	                init_attr.cap.max_send_sge = 1;
                	                init_attr.cap.max_recv_wr = RPING_CQ_RQ_DEPTH;
                	                init_attr.cap.max_recv_sge = 1;

                	                init_attr.send_cq = temp->ctx.cq;
                	                init_attr.recv_cq = temp->ctx.cq;
                	                init_attr.qp_type = IBV_QPT_RC;

                	                conn_param.initiator_depth      = 16;
                	                conn_param.responder_resources  = 16;
                	                conn_param.retry_count          = 10;
                	                conn_param.rnr_retry_count      = 7;
                	                ret = rdma_create_qp(temp->client_cm_id, temp->client_cm_id->pd,
                	                                     &init_attr);

                	                if (ret)
                	                        error_handler("create_qp Failed:");

                	                rdma_accept(temp->client_cm_id, &conn_param);
                	                temp->state = CONNECT_REQ_PROC;
                	        } else if (temp->state == ESTABLISHED) {
                	                rping_setup_buffers_server(&temp->ctx);
                	                temp->state = READY_FOR_DATA_TRANSFER;
                	                rping_test.connections++;
                	        }
                	        temp = temp->next;
                	        i++;
                	}
                	i = 0;
                	if (rping_info->connections >= rping_info->rx_test_qp_cnt) {
                	        pthread_exit((void *)(uintptr_t)0);
			}
 			pthread_yield();
		}
	}               
}

static void *cm_thread(void *arg)
{
        struct rping_test_info *rping_info = arg;
        struct rping_cb *cb;
        int ret, i;
        struct rdma_cm_event *event;

        DEBUG_LOG("cm thread started\n");
	if (rping_test.server == 0) {
        	while (1) {
        	        for (i = 0; i < rping_info->qp_cnt; i++) {
        	                cb = &rping_info->cb[i];
        	                if ((!cb->cm_fail) && cb->cm_chnl_en) {
        	                        ret = rdma_get_cm_event(cb->cm_channel, &cb->event);
        	                        if (ret) {
						printf("rdma_get_cm_event failed CB=%p", cb);
        	                                cb->cm_fail = 1;
        	                                continue;
        	                        }

        	                        ret = rping_cma_event_handler(cb->event->id, cb->event);
        	                        rdma_ack_cm_event(cb->event);
        	                        if (ret) {
						printf("Ack cm event failed for %p\n", cb);
        	                                cb->cm_fail = 1;
        	                        }
        	                }
        	        }
        	}
	} else {
		/* create event channel and ID */
		rping_info->cm_channel = rdma_create_event_channel();
		if (!rping_info->cm_channel) {
			printf("Failed to create event channel \n");
			pthread_exit((void *)(intptr_t)-1);
		}

		ret = rdma_create_id(rping_info->cm_channel, &rping_info->cm_id,
				NULL, RDMA_PS_TCP);

		if (ret) {
			printf("Failed to create CM ID\n");
			pthread_exit((void *)(intptr_t)-1);
		}

		init_server_cm(rping_info);
		do {
			if (rdma_get_cm_event(rping_info->cm_channel, &event)) {
				error_handler("GET_CM_EVENT Failed:");
				continue;
			}
			ret = set_ctx_to_rx_connect_req(rping_info, event->id, event->event);
			rdma_ack_cm_event(event);
#if 0
			if (event->event == RDMA_CM_EVENT_ESTABLISHED)
				pthread_yield();
#endif
		} while(rping_info->disconnects < rping_info->rx_test_qp_cnt);
	}
}

static void *cq_thread(void *arg)
{
	struct rping_test_info *rping_info = arg;
	struct rping_cb *cb;
	struct ibv_cq *ev_cq;
	void *ev_ctx;
	int ret, i;

	DEBUG_LOG("cq_thread started.\n");
	while (1) {
		for (i = 0; i < rping_info->qp_cnt; i++) {
			cb = &rping_info->cb[i];
			if ((!cb->cq_fail) && cb->cm_established) {
				ret = rping_cq_event_handler(cb);
				if (cb->state == ERROR) {
					cb->cq_fail = 1;
					printf("Failed to ack cq events for cb %p\n", cb);
				}
			}
		}
	}
}

static void rping_format_send(struct rping_cb *cb, char *buf, struct ibv_mr *mr)
{
	struct rping_rdma_info *info = (struct rping_rdma_info *)cb->send_buf;

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

int app_req_regions(struct rping_cb* rdma_ctx)
{
        int err;
        struct rdma_cm_id* cm_id;

        cm_id = rdma_ctx->cm_id;
        cm_id->pd = ibv_alloc_pd(cm_id->verbs);
        if (!cm_id->pd)
                error_handler("alloc_pd Failed:");

	rdma_ctx->channel = NULL;

        rdma_ctx->cq = ibv_create_cq(cm_id->verbs, RPING_CQ_RQ_DEPTH, NULL,
                                     rdma_ctx->channel, 0);
        if (!rdma_ctx->cq)
                error_handler("create_cq Failed:");

        return 0;
}

int init_server_cm(struct rping_test_info* srping)
{
        int err;
        struct sockaddr_in sin4_addr;
        struct rdma_cm_event *event;

        sin4_addr.sin_family = AF_INET;
        sin4_addr.sin_port = rping_test.port;
        sin4_addr.sin_addr.s_addr = INADDR_ANY;

        if (rdma_bind_addr(srping->cm_id, (struct sockaddr*)&sin4_addr))
                error_handler("RDMA BIND Failed:");

        if (rdma_listen(srping->cm_id, (rping_test.qp_cnt + 1)))
                error_handler("RDMA Listen Failed:");

        return 0;
}

struct ctx_list *update_ctx_entry(struct rping_test_info *srping, struct rdma_cm_id *id,
                                  enum rdma_cm_event_type event)
{
        struct ctx_list *temp;

        pthread_mutex_lock(&srping->cm_list_lock);

        temp = srping->head;
        while (temp) {
                if (temp->client_cm_id == id) {
                        break;
                }
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
        pthread_mutex_unlock(&srping->cm_list_lock);
        return temp;
}

int set_ctx_to_rx_connect_req(struct rping_test_info *srping, struct rdma_cm_id *id,
                              enum rdma_cm_event_type event)
{
        struct ctx_list *temp;

        if (event == RDMA_CM_EVENT_CONNECT_REQUEST) {
                /* create app region and wait for CONNECT establishment */
		DEBUG_LOG("Connect request received\n");
                /* create a cm channel and ID to wait */
                temp = malloc(sizeof(struct ctx_list));
                if (!temp) {
                        printf("Failed to allocate memory for CM ctx\n");
                        return -ENOMEM;
                }
                memset(temp, 0, sizeof (temp));
                temp->next = NULL;
                temp->client_cm_id = id;

                if (!srping->head)
                        srping->head = temp;
                else
                        srping->prev->next = temp;

                srping->prev = temp;
                temp->state = CONNECT_REQ;

        } else if (event == RDMA_CM_EVENT_ESTABLISHED) {
		DEBUG_LOG("Connection established\n");
                temp = update_ctx_entry(srping, id, event);
                if (!temp) {
                        printf("ERROR: Connect request must be Rx before RTU\n");
                        return -EINVAL;
                }
        } else if ((event == RDMA_CM_EVENT_ADDR_RESOLVED) ||
                   (event == RDMA_CM_EVENT_ROUTE_RESOLVED)) {
                /* no error */
        } else if (event == RDMA_CM_EVENT_DISCONNECTED) {
                srping->disconnects++;
                if (srping->disconnects >= srping->rx_test_qp_cnt)
                        exit(0);
        } else {
                printf("Event %s:%d is not being handled\n",
                        rdma_event_str(event), event);
                /* remove entry */
                return -1;
        }
        return 0;
}

void rdmatest_poll_cq(struct rping_cb* rdma_ctx, unsigned id)
{
        struct ibv_wc wc;
        void* cq_ctx;
        struct ibv_cq* cq_event;

        while (1) {
                if (ibv_poll_cq(rdma_ctx->cq, 1, &wc) != 1)
			continue;
                if (wc.status != IBV_WC_SUCCESS)
                {
                        printf("ernic wc status = %d(%s), id = %lx:%d\n",
                                        wc.status,
                                        ibv_wc_status_str(wc.status),
                                        wc.wr_id, id);
                        return ;
                }

                if (wc.wr_id == id)
                        break;
        }
}


int test_server_rping(struct rping_cb *ctx)
{
        struct rdma_cm_id* cm_id;
        struct ibv_recv_wr *bad_wr;
        int err, r, i = 0, cq_depth = 1;
        static int qp_cnt = 0;
	struct rping_rdma_info *rx_buf;

        cm_id = ctx->cm_id;
        /* implement rping server data path */
        /* else run the normal rping functionality */
        ctx->iter_cnt = 1;
        for (i = 0; i < ctx->iter_cnt; i++) {
		rx_buf = (struct rping_rdma_info *)ctx->recv_buf;
                r = rand();
                /* 1. Wait for incoming SEND */
                err = rdma_post_recv(cm_id, (void *)(uintptr_t)r, ctx->recv_buf,
                                     sizeof(ctx->recv_buf),
                                     ctx->recv_mr);
                if (err < 0) {
                        printf("Failed to post the RQE for incoming SEND at %d\n",
                               __LINE__);
                        /* free qp */
                        return -1;
                }

                /* poll for CQ */
                rdmatest_poll_cq(ctx, r);
                ctx->remote_rkey = ntohl(rx_buf->rkey);
                ctx->remote_addr = be64toh(rx_buf->buf);
                ctx->remote_len  = ntohl(rx_buf->size);
                if (!i) {
                        srping_test.rx_test_qp_cnt = ntohl(rx_buf->qp_cnt);
                        ctx->iter_cnt = be32toh(rx_buf->iter_cnt);
                }
                /* Update count only once. Updating multiple times doesn't make
                 * sense though it isn't harmful */
                DEBUG_LOG("Received rkey %#x addr %#lx len %d from peer\n",
				ctx->remote_rkey, ctx->remote_addr, ctx->remote_len);

                /* 2. Do RDMA READ to the Rx R-Key & address */
                err = rdma_post_read(cm_id, (void *)(uintptr_t)(r + 1), ctx->rdma_buf,
                                     ctx->remote_len, ctx->rdma_mr,
                                     IBV_SEND_SIGNALED, ctx->remote_addr,
                                     ctx->remote_rkey);
                if (err < 0) {
                        printf("Failed to post RDMA READ\n");
                        return -1;
                }
                rdmatest_poll_cq(ctx, r+1);

                /* 3. Send RDMA_SEND indicating SEND completion */
                /* 4. Wait for Incoming SEND */
                err = rdma_post_recv(cm_id, (void *)(uintptr_t)(r + 3), ctx->recv_buf,
                                     sizeof(ctx->recv_buf),
                                     ctx->recv_mr);
                if (err < 0) {
                        printf("Failed to post the RQE for incoming SEND at %d\n",
                               __LINE__);
                        /* free qp */
                        return -1;
                }

                err = rdma_post_send(cm_id, (void *)(uintptr_t)(r + 2), ctx->send_buf,
                                     sizeof(ctx->send_buf),
                                     ctx->send_mr, IBV_SEND_SIGNALED);
                if (err < 0) {
                        printf("Failed to post the RQE for incoming SEND at %d\n",
                               __LINE__);
                        /* free qp */
                        return -1;
                }
                rdmatest_poll_cq(ctx, r + 2);

                rdmatest_poll_cq(ctx, r + 3);

                /* 5. Write back the read data in step 2 */
                err = rdma_post_write(cm_id, (void *)(uintptr_t)(r + 4), ctx->rdma_buf,
                                      ctx->remote_len, ctx->rdma_mr,
                                      IBV_SEND_SIGNALED, ctx->remote_addr,
                                      ctx->remote_rkey);
                if (err < 0) {
                        printf("Failed to post RDMA WRITE\n");
                        return -1;
                }
                rdmatest_poll_cq(ctx, r + 4);

                /* 6. Send completion */
                /* Tell the client its done */
                err = rdma_post_send(cm_id, (void *)(uintptr_t)(r + 5), ctx->send_buf,
                                     sizeof(ctx->send_buf),
                                     ctx->send_mr, IBV_SEND_SIGNALED);
                if (err < 0) {
                        printf("Failed to post the RDMA SEND to indicate test commpletion at %d\n",
                               __LINE__);

                        /* free qp */
                        return -1;
                }
                rdmatest_poll_cq(ctx, r + 5);
        }
        return 0;
}


int run_server()
{
        struct ctx_list *list;
        int err;
        int cnt = 0;

        /* Create rping CM thread */
        err = pthread_create(&srping_test.cmthread, NULL, cm_thread,
                             (void *)&srping_test);
        if (err) {
                printf("Failed to create CM thread in rping\n");
                return err;
        }

        err = pthread_create(&srping_test.datathread, NULL, data_thread,
                             (void *)&srping_test);
        if (err) {
                printf("Failed to create establishment thread\n");
                return err;
        }
        while (1) {
                if (!srping_test.head) {
                        continue;
                }

                list = srping_test.head;
                while (list) {
                        if (list->state == READY_FOR_DATA_TRANSFER) {
                                err = test_server_rping(&list->ctx);
                                cnt++;
                                if (cnt >= srping_test.rx_test_qp_cnt)
                                        return 0;
                                list->state = INVALID_STATE;
                                /* delete the entry */
                        }
                        list = list->next;
                }
        }
}


int rping_test_client(struct rping_cb *cb)
{
	int ping, start, cc, i, ret = 0;
	struct ibv_send_wr *bad_wr;
	unsigned char c;

	/* Execute normal rping test */
	start = 65;
	for (ping = 0; !cb->count || ping < cb->count; ping++) {
		cb->state = RDMA_READ_ADV;

		/* Put some ascii text in the buffer. */
		cc = snprintf(cb->start_buf, rping_test.size, RPING_MSG_FMT, ping);
		for (i = cc, c = start; i < rping_test.size; i++) {
			cb->start_buf[i] = c;
			c++;
			if (c > 122)
				c = 65;
		}
		start++;
		if (start > 122)
			start = 65;
		cb->start_buf[rping_test.size - 1] = 0;

		memcpy(cb->rdma_buf_ofs, cb->start_buf, rping_test.size);
		rping_format_send(cb, cb->rdma_buf_ofs, cb->rdma_mr);
		ret = ibv_post_send(cb->qp, &cb->sq_wr, &bad_wr);
		if (ret) {
			printf( "post send error %d\n", ret);
			break;
		}

		/* Wait for server to ACK */
		sem_wait(&cb->sem_data);
		if (cb->state != RDMA_WRITE_ADV) {
			printf("Rx %d state insted of RDMA_WRITE_ADV\n",
				cb->state);
			ret = -1;
			break;
		}

		memset(cb->rdma_buf_ofs, 0x0F, rping_test.size);
		rping_format_send(cb, cb->rdma_buf_ofs, cb->rdma_mr);
		ret = ibv_post_send(cb->qp, &cb->sq_wr, &bad_wr);
		if (ret) {
			printf( "post send error %d\n", ret);
			break;
		}

		/* Wait for the server to say the RDMA Write is complete. */
		sem_wait(&cb->sem_data);
		if (cb->state != RDMA_WRITE_COMPLETE) {
			printf( "Rx %d state instead of RDMA_WRITE_COMPLETE\n",
				cb->state);
			ret = -1;
			break;
		}

		cb->rdma_buf_ofs[rping_test.size] = '\0';
		if (rping_test.validate) {
			if (memcmp(cb->start_buf, cb->rdma_buf_ofs, rping_test.size)) {
				printf( "data mismatch!\n");
				ret = -1;
				break;
			}
		}
		if (rping_test.verbose)
			printf("ping data: %s\n", cb->rdma_buf_ofs);
	}

	return (cb->state == DISCONNECTED) ? 0 : ret;
}

static int rping_connect_client(struct rping_cb *cb)
{
	struct rdma_conn_param conn_param;
	int ret;

	memset(&conn_param, 0, sizeof conn_param);
	conn_param.responder_resources = 16;
	conn_param.initiator_depth = 16;
	conn_param.retry_count = 7;
	conn_param.rnr_retry_count = 7;

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

static int rping_run_client(struct rping_cb *cb)
{
	struct ibv_recv_wr *bad_wr;
	int ret;

	ret = rping_bind_client(cb);
	if (ret)
		return ret;

	ret = rping_setup_qp(cb, cb->cm_id);
	if (ret) {
		printf("setup_qp failed: %d\n", ret);
		return ret;
	}

	ret = rping_setup_buffers(cb);
	if (ret) {
		printf("rping_setup_buffers failed: %d\n", ret);
		goto err1;
	}

	ret = ibv_post_recv(cb->qp, &cb->rq_wr, &bad_wr);
	if (ret) {
		printf("ibv_post_recv failed: %d\n", ret);
		goto err2;
	}

	if (!rping_test.cq_init) {
		ret = pthread_create(&rping_test.cqthread, NULL, cq_thread, &rping_test);
		if (ret) {
			printf("pthread_create");
			goto err2;
		}
		rping_test.cq_init = 1;
	}

	ret = rping_connect_client(cb);
	if (ret) {
		printf("connect error %d\n", ret);
		goto err2;
	}

	return 0;
err4:
	rdma_disconnect(cb->cm_id);
err2:
	rping_free_buffers(cb);
err1:
	rping_free_qp(cb);

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
	printf("\t-c\t:client side\n");
	printf("\t-s\t:server side\n");
	printf("\t-I\t:source address to bind to for client\n");
	printf("\t-v\t:display ping data\n");
	printf("\t-V\t:validate ping data\n");
	printf("\t-d\t:debug prints\n");
	printf("\t-S <size> \t:ping data size\n");
	printf("\t-C <count>\t:ping count times\n");
	printf("\t-a <addr> \t:server address\n");
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
	struct rdma_event_channel *cm_channel;

	rping_test.server = 0;
	rping_test.size = PAYLOAD_SIZE;
	rping_test.sin.ss_family = PF_INET;
	rping_test.port = htobe16(7175);
	rping_test.qp_cnt = 1;
	rping_test.cq_init = 0;
	rping_test.count = 0;
	opterr = 0;

	if (argc == 1) {
		usage(argv[0]);
		return 0;
	}

	while ((op = getopt(argc, argv, "a:I:p:C:S:csvVdQ:")) != -1) {
		switch (op) {
		case 'Q':
			rping_test.qp_cnt = atoi(optarg); 
			if (rping_test.qp_cnt <= 0) {
				printf("Invalid QP count \n");
				return -1;
			}
			break;

		case 'a':
			ret = get_addr(optarg, (struct sockaddr *) &rping_test.sin);
			break;
		case 'I':
			ret = get_addr(optarg, (struct sockaddr *) &rping_test.ssource);
			break;
		case 'p':
			rping_test.port = htobe16(atoi(optarg));
			DEBUG_LOG("port %d\n", rping_test.port);
			break;
		case 'c':
			rping_test.server = 0;
			break;
		case 's':
			rping_test.server = 1;
			break;
		case 'S':
			rping_test.size = atoi(optarg);
			DEBUG_LOG("size %d\n", (int) atoi(optarg));
			break;
		case 'C':
			rping_test.count = atoi(optarg);
			if (rping_test.count < 0) {
				printf( "Invalid count %d\n",
					rping_test.count);
				return -EINVAL;
			} else
				DEBUG_LOG("count %d\n", (int) rping_test.count);
			break;
		case 'v':
			rping_test.verbose++;
			DEBUG_LOG("verbose\n");
			break;
		case 'V':
			rping_test.validate++;
			DEBUG_LOG("validate data\n");
			break;
		case 'd':
			debug++;
			break;
		default:
			usage("rping");
			return -EINVAL;
		}
	}

	if (!rping_test.count)
		rping_test.count = 1;

	cm_channel = rdma_create_event_channel();
	if (!cm_channel) {
		ret = errno;
		printf("event channel creation failed for app qp #%d\n", i);
		rping_test.done_cnt++;
		return -EFAULT;
	}

	rping_test.cm_channel = cm_channel;
	if (rping_test.server == 0) {

		i = sizeof(struct rping_cb) * rping_test.qp_cnt;
		rping_test.cb = malloc(i);
		if (!rping_test.cb) {
			printf("Failed to allocate memory for contexts\n");
			return -ENOMEM;
		}
		memset(rping_test.cb, 0, i);

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

		if (!rping_test.qp_cnt) {
			printf("Qp count cant be 0\n");
			return -1;
		}


		for (i = 0; i < rping_test.qp_cnt; i++) {
			cb = &rping_test.cb[i];
			memset(cb, 0, sizeof(struct rping_cb));
			sem_init(&cb->sem, 0, 0);
			sem_init(&cb->sem_data, 0, 0);
			cb->cm_channel = rping_test.cm_channel;
			cb->qp_num = i;
			cb->cm_fail = cb->cq_fail = 0;
			cb->cm_chnl_en = 0;
			cb->cm_established = 0;
			cb->count = rping_test.count;
			cb->test_done = 0;
			ret = rdma_create_id(cb->cm_channel, &cb->cm_id, cb, RDMA_PS_TCP);
			if (ret) {
				printf("ID creation failed for app qp #%d\n", i);
				rping_test.done_cnt++;
				return -EFAULT;
			}
			DEBUG_LOG("created cm_id %p\n", cb->cm_id);
			cb->cm_chnl_en = 1;
			ret = rping_run_client(cb);
			if (ret) {
				printf("CM Failed for application QP #%d\n", i);
				/* Disable looking for events on this channel */
				cb->cm_chnl_en = 0;
				rping_test.done_cnt++;
			}

			DEBUG_LOG("rping init done for app qp #%d\n", i);
		}

		if (ret)
			return -EFAULT;

		while(rping_test.done_cnt < rping_test.qp_cnt);

		for (i = 0; i < rping_test.qp_cnt; i++) {
			rdma_destroy_qp(rping_test.cb[i].cm_id);
			rdma_destroy_id(rping_test.cb[i].cm_id);
		}
	} else {
	        srping_test.rx_test_qp_cnt = rping_test.qp_cnt;
		run_server();
	        /* wait for the disconnects to come */
        	while(srping_test.disconnects < srping_test.rx_test_qp_cnt);
	}
	rdma_destroy_event_channel(cm_channel);
}
