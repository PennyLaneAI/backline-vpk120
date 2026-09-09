/*
 ******************************************************************************
 *
 * copyright (C) 2018 Xilinx, Inc.  All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * XILINX  BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
 * WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF
 * OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 * Except as contained in this notice, the name of the Xilinx shall not be used
 * in advertising or otherwise to promote the sale, use or other dealings in
 * this Software without prior written authorization from Xilinx.
 *
 ******************************************************************************/

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
#include <rdma/rdma_verbs.h>


#ifndef _XRPING_H
#define _XRPING_H

static unsigned int dis_cq_thread = 0;

#define PAYLOAD_SIZE (4096)
#define ERNIC_SGE_SIZE (256)
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

struct rdma_info {
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

struct rdma_ctx_info {
	uint64_t	va;
	uint32_t	rkey;
	uint8_t		rsvd[4];
}__attribute__((__packed__));

struct rdma_cb {
	pthread_t cqthread;
	pthread_t persistent_server_thread;
	struct ibv_comp_channel *channel;
	struct ibv_cq *cq;
	struct ibv_pd *pd;
	struct ibv_qp *qp;

	struct ibv_recv_wr rq_wr;	/* recv work request record */
	struct ibv_sge recv_sgl;	/* recv single SGE */
	struct rdma_ctx_info recv_buf, send_buf;/* malloc'd buffer */
	struct ibv_mr *recv_mr;		/* MR associated with this buffer */

	struct ibv_send_wr sq_wr;	/* send work request record */
	struct ibv_sge send_sgl;
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
};

struct gen_test_info {
	struct rdma_cb	*cb;
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
	int	size_in_mb, pkt_size;
	int	count;
	int	verbose;
	int	validate;
	unsigned int	cpu_clk;
	unsigned int line_rate;
	struct sockaddr_storage sin, ssource;
	uint16_t port;			/* dst port in NBO */
	int	cq_init;
	int	read_op;
	volatile int done_cnt;
} gen_test;

enum rdma_cm_states {
        CONNECT_REQ,
        CONNECT_REQ_PROC,
        ESTABLISHED,
        READY_FOR_DATA_TRANSFER,
        INVALID_STATE
};

struct ctx_list {
        struct rdma_cb ctx;
        struct rdma_cm_id* client_cm_id;
        volatile enum rdma_cm_states state;
        struct ctx_list *next;
};

#endif

int app_req_regions(struct rdma_cb* rdma_ctx);
int init_server_cm(struct gen_test_info* srdma);
int set_ctx_to_rx_connect_req(struct gen_test_info *srdma, struct rdma_cm_id *id,
                              enum rdma_cm_event_type event);
static int debug = 0;
#define DEBUG_LOG if (debug) printf
int iter_cnt = 1;
struct gen_test_info sgen_test;
#define RPING_SQ_DEPTH 16
#define RPING_QP_CONNECTION_CNT 255

#define error_handler(x)        do {                                    \
                                        char str[100];                  \
                                        sprintf(str, "^[[0;31m%s",x);   \
                                        perror(str);                    \
                                        printf("^[[0;m");               \
                                        exit(EXIT_FAILURE);             \
                                } while(0);


int gen_test_client(struct rdma_cb *cb);
static int rdma_cma_event_handler(struct rdma_cm_id *cma_id,
				    struct rdma_cm_event *event)
{
	int ret = 0;
	struct rdma_cb *cb = cma_id->context;

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
		if (!gen_test.server) {
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
			gen_test.server ? "server" : "client");
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

static int client_recv(struct rdma_cb *cb, struct ibv_wc *wc)
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

static int rdma_cq_event_handler(struct rdma_cb *cb)
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
				dis_cq_thread = 1;
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

static void rdma_setup_wr(struct rdma_cb *cb)
{
       cb->recv_mr = ibv_reg_mr(cb->pd, &cb->recv_buf, sizeof cb->recv_buf,
                                        IBV_ACCESS_LOCAL_WRITE);
       if (!cb->recv_mr) {
             fprintf(stderr, "recv_buf reg_mr failed\n");
                     ibv_dereg_mr(cb->recv_mr);
             return;
       }

       cb->send_mr = ibv_reg_mr(cb->pd, &cb->send_buf, sizeof cb->send_buf, IBV_ACCESS_LOCAL_WRITE);
       if (!cb->send_mr) {
              fprintf(stderr, "send_buf reg_mr failed\n");
              ibv_dereg_mr(cb->send_mr);
              return;
        }

        cb->recv_sgl.addr = (uint64_t) (unsigned long) &cb->recv_buf;
        cb->recv_sgl.length = sizeof cb->recv_buf;
        cb->recv_sgl.lkey = cb->recv_mr->lkey;
        cb->rq_wr.sg_list = &cb->recv_sgl;
        cb->rq_wr.num_sge = 1;

        cb->send_sgl.addr = (uint64_t) (unsigned long) &cb->send_buf;
        cb->send_sgl.length = sizeof cb->send_buf;
        cb->send_sgl.lkey = cb->send_mr->lkey;

        cb->sq_wr.opcode = IBV_WR_SEND;
        cb->sq_wr.send_flags = IBV_SEND_SIGNALED;
        cb->sq_wr.sg_list = &cb->send_sgl;
        if (gen_test.server == 0) {
                cb->sq_wr.num_sge = 1;
        }
	cb->rdma_sgl.lkey = cb->rdma_mr->lkey;
        cb->rdma_sq_wr.send_flags = IBV_SEND_SIGNALED;
        cb->rdma_sq_wr.sg_list = &cb->rdma_sgl;
        cb->rdma_sq_wr.num_sge = 1;
}

static int rdma_setup_buffers(struct rdma_cb *cb)
{
	int ret;

	cb->rdma_buf = malloc(gen_test.pkt_size);
	if (!cb->rdma_buf) {
		printf("rdma_buf malloc failed\n");
		ret = -ENOMEM;
		goto err2;
	}


	cb->rdma_buf_ofs = cb->rdma_buf;
	cb->rdma_mr = ibv_reg_mr(cb->pd, cb->rdma_buf, gen_test.pkt_size,
				 IBV_ACCESS_LOCAL_WRITE |
				 IBV_ACCESS_REMOTE_READ |
				 IBV_ACCESS_REMOTE_WRITE);
	if (!cb->rdma_mr) {
		printf("rdma_buf reg_mr failed\n");
		ret = errno;
		goto err3;
	}

	rdma_setup_wr(cb);
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

static void rdma_free_buffers(struct rdma_cb *cb)
{
	ibv_dereg_mr(cb->rdma_mr);
	free(cb->rdma_buf);
	free(cb->start_buf);
}

static int create_qp(struct rdma_cb *cb)
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

static void rdma_free_qp(struct rdma_cb *cb)
{
	ibv_destroy_qp(cb->qp);
	ibv_destroy_cq(cb->cq);
	ibv_destroy_comp_channel(cb->channel);
	ibv_dealloc_pd(cb->pd);
}

static int rdma_setup_qp(struct rdma_cb *cb, struct rdma_cm_id *cm_id)
{
	int ret;

	cb->pd = ibv_alloc_pd(cm_id->verbs);
	if (!cb->pd) {
		printf("ibv_alloc_pd failed\n");
		return errno;
	}
	DEBUG_LOG("created pd %p\n", cb->pd);

	cb->channel = ibv_create_comp_channel(cm_id->verbs);
	if (!cb->channel) {
		printf("ibv_create_comp_channel failed\n");
		ret = errno;
		goto err1;
	}
	DEBUG_LOG("created channel %p\n", cb->channel);

	cb->cq = ibv_create_cq(cm_id->verbs, RPING_SQ_DEPTH * 2, cb,
				cb->channel, 0);
	if (!cb->cq) {
		printf( "ibv_create_cq failed\n");
		ret = errno;
		goto err2;
	}
	DEBUG_LOG("created cq %p\n", cb->cq);

	ret = ibv_req_notify_cq(cb->cq, 0);
	if (ret) {
		printf( "ibv_create_cq failed\n");
		ret = errno;
		goto err3;
	}

	ret = create_qp(cb);
	if (ret) {
		perror("create_qp");
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

static int rdma_setup_buffers_server(struct rdma_cb *cb)
{
        int ret;

        cb->recv_mr = ibv_reg_mr(cb->cm_id->pd, &cb->recv_buf,
                                       sizeof cb->recv_buf,
                                       IBV_ACCESS_LOCAL_WRITE);
        if (!cb->recv_mr) {
                printf("recv_buf reg_mr failed\n");
                return -1;
        }

        cb->send_mr = ibv_reg_mr(cb->cm_id->pd, &cb->send_buf,
                            sizeof cb->send_buf, IBV_ACCESS_LOCAL_WRITE);
        if (!cb->send_mr) {
                printf("send_buf reg_mr failed\n");
                ret = errno;
                goto err1;
        }

        cb->rdma_buf = malloc(gen_test.pkt_size);
        if (!cb->rdma_buf) {
                fprintf(stderr, "rdma_buf malloc failed\n");
                ret = -ENOMEM;
        }

        cb->rdma_mr = ibv_reg_mr(cb->cm_id->pd, cb->rdma_buf, gen_test.pkt_size,
                                 IBV_ACCESS_LOCAL_WRITE |
                                 IBV_ACCESS_REMOTE_READ |
                                 IBV_ACCESS_REMOTE_WRITE);
        if (!cb->rdma_mr) {
                fprintf(stderr, "rdma_buf reg_mr failed\n");
                ret = errno;
                goto err3;
        }

        rdma_setup_wr(cb);
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
       struct gen_test_info *rdma_info = arg;
        struct rdma_cb *cb;
        int i = 0, ret;
        struct ctx_list *temp = NULL;
        struct ibv_qp_init_attr  init_attr;
        struct rdma_conn_param conn_param;

	if (gen_test.server == 0) {
        	while(1) {
                	for (i = 0; i < rdma_info->qp_cnt; i++) {
                	        cb = &rdma_info->cb[i];
                	        if (cb->cm_established && (!cb->test_done)) {
                	                /* start the data transfers */
                	                ret = gen_test_client(cb);
                	                if (ret) {
                	                        printf("rdma test FAILED for app qp #%d\n", i);
                	                        /* disable the tranfers */
                	                        cb->cm_established = 0;
                	                } else
                	                        printf("rdma test PASSED for app QP #%d\n", i);
                	                cb->cm_established = 0;
                	                cb->test_done = 1;
                	                /* Once the test is over, just disable the channel to
                	                        avoid hearing for events on the channel */
                	                cb->cm_chnl_en = 0;
                	                gen_test.done_cnt++;
                	        }
                	}
 			pthread_yield();
		}
	} else {
        	while(1) {
			pthread_mutex_lock(&rdma_info->cm_list_lock);
                	if (!temp)
                	        temp = rdma_info->head;
                	while(temp && (i < 3)) {
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
                	                rdma_setup_buffers_server(&temp->ctx);
                	                temp->state = READY_FOR_DATA_TRANSFER;
                	                gen_test.connections++;
                	        }
                	        temp = temp->next;
                	        i++;
                	}
                	i = 0;
                	pthread_mutex_unlock(&rdma_info->cm_list_lock);
                	if (rdma_info->connections >= rdma_info->rx_test_qp_cnt)
                	        pthread_exit((void *)(uintptr_t)0);
 			pthread_yield();
		}
	}
}

static void *cm_thread(void *arg)
{
        struct gen_test_info *rdma_info = arg;
        struct rdma_cb *cb;
        int ret, i;
        struct rdma_cm_event *event;

        DEBUG_LOG("cm thread started\n");
	if (gen_test.server == 0) {
        	while (1) {
        	        for (i = 0; i < rdma_info->qp_cnt; i++) {
        	                cb = &rdma_info->cb[i];
        	                if ((!cb->cm_fail) && cb->cm_chnl_en) {
        	                        ret = rdma_get_cm_event(cb->cm_channel, &cb->event);
        	                        if (ret) {
        	                                printf("rdma_get_cm_event failed CB=%#lx", (uintptr_t)cb);
        	                                cb->cm_fail = 1;
        	                                continue;
        	                        }

        	                        ret = rdma_cma_event_handler(cb->event->id, cb->event);
        	                        rdma_ack_cm_event(cb->event);
        	                        if (ret) {
        	                                printf("Ack cm event failed for %#lx\n", (uintptr_t)cb);
        	                                cb->cm_fail = 1;
        	                        }
        	                }
        	        }
        	}
	} else {
		/* create event channel and ID */
 	       rdma_info->cm_channel = rdma_create_event_channel();
 	       if (!rdma_info->cm_channel) {
 	               printf("Failed to create event channel \n");
 	               pthread_exit((void *)(intptr_t)-1);
 	       }

 	       ret = rdma_create_id(rdma_info->cm_channel, &rdma_info->cm_id,
 	                            NULL, RDMA_PS_TCP);

 	       if (ret) {
 	               printf("Failed to create CM ID\n");
 	               pthread_exit((void *)(intptr_t)-1);
 	       }

 	       init_server_cm(rdma_info);
 	       do {
 	               if (rdma_get_cm_event(rdma_info->cm_channel, &event)) {
 	                       error_handler("GET_CM_EVENT Failed:");
 	                       continue;
 	               }
 	               ret = set_ctx_to_rx_connect_req(rdma_info, event->id, event->event);
 	               rdma_ack_cm_event(event);
 	       } while(rdma_info->disconnects < rdma_info->rx_test_qp_cnt);
	}
}

static void *cq_thread(void *arg)
{
	struct gen_test_info *rdma_info = arg;
	struct rdma_cb *cb;
	struct ibv_cq *ev_cq;
	void *ev_ctx;
	int ret, i;

	DEBUG_LOG("cq_thread started.\n");
	while (1) {
		for (i = 0; i < rdma_info->qp_cnt; i++) {
			cb = &rdma_info->cb[i];
			if ((!cb->cq_fail) && cb->cm_established) {
				ret = ibv_req_notify_cq(cb->cq, 0);
				if (ret) {
					printf("Failed to set cq notify for Cb %#lx\n", (uintptr_t)cb);
					cb->cq_fail = 1;
					continue;
				}
				ret = rdma_cq_event_handler(cb);
				if (cb->poll_pkt_cnt) {
					ibv_ack_cq_events(cb->cq, 1);
					cb->poll_pkt_cnt = 0;
				}
				if (cb->state == ERROR) {
					cb->cq_fail = 1;
					printf("Failed to ack cq events for cb %#lx\n", (uintptr_t)cb);
				}
				if (dis_cq_thread) {
					pthread_exit(0);
				}
			}
		}
	}
}

static void rdma_format_send(struct rdma_cb *cb, char *buf, struct ibv_mr *mr)
{
	struct rdma_info *info = (struct rdma_info *)&cb->send_buf;

	info->buf = htobe64((uint64_t) (unsigned long) buf);
	info->rkey = htobe32(mr->rkey);
	info->size = htobe32(gen_test.pkt_size);
	info->iter_cnt = htobe32(cb->count);
	info->qp_cnt = htobe32(gen_test.qp_cnt);
	DEBUG_LOG("RDMA addr %" PRIx64" rkey %x len %d\n",
		  be64toh(info->buf), be32toh(info->rkey), be32toh(info->size));
}

static void free_cb(struct rdma_cb *cb)
{
	free(cb);
}

int app_req_regions(struct rdma_cb* rdma_ctx)
{
        int err;
        struct rdma_cm_id* cm_id;

        cm_id = rdma_ctx->cm_id;
        cm_id->pd = ibv_alloc_pd(cm_id->verbs);
        if (!cm_id->pd)
                error_handler("alloc_pd Failed:");

        rdma_ctx->channel = ibv_create_comp_channel(cm_id->verbs);
        if (!rdma_ctx->channel)
                error_handler("create_comp_channel Failed:");

        rdma_ctx->cq = ibv_create_cq(cm_id->verbs, RPING_CQ_RQ_DEPTH, NULL,
                                     rdma_ctx->channel, 0);
        if (!rdma_ctx->cq)
                error_handler("create_cq Failed:");

        err = ibv_req_notify_cq(rdma_ctx->cq, 0);
        if (err)
                error_handler("req_notify_cq Failed:");

        return 0;
}

int init_server_cm(struct gen_test_info* srdma)
{
        int err;
        struct sockaddr_in sin4_addr;
        struct rdma_cm_event *event;

        sin4_addr.sin_family = AF_INET;
        sin4_addr.sin_port = htons(18516);
        sin4_addr.sin_addr.s_addr = INADDR_ANY;

        if (rdma_bind_addr(srdma->cm_id, (struct sockaddr*)&sin4_addr))
                error_handler("RDMA BIND Failed:");

        if (rdma_listen(srdma->cm_id, RPING_QP_CONNECTION_CNT))
                error_handler("RDMA Listen Failed:");

        return 0;
}

struct ctx_list *update_ctx_entry(struct gen_test_info *srdma, struct rdma_cm_id *id,
                                  enum rdma_cm_event_type event)
{
        struct ctx_list *temp;

        pthread_mutex_lock(&srdma->cm_list_lock);

        temp = srdma->head;
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
        pthread_mutex_unlock(&srdma->cm_list_lock);
        return temp;
}

int set_ctx_to_rx_connect_req(struct gen_test_info *srdma, struct rdma_cm_id *id,
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

                pthread_mutex_lock(&srdma->cm_list_lock);
                if (!srdma->head)
                        srdma->head = temp;
                else
                        srdma->prev->next = temp;

                srdma->prev = temp;
                temp->state = CONNECT_REQ;
                pthread_mutex_unlock(&srdma->cm_list_lock);

        } else if (event == RDMA_CM_EVENT_ESTABLISHED) {
		DEBUG_LOG("Connection established\n");
                temp = update_ctx_entry(srdma, id, event);
                if (!temp) {
                        printf("ERROR: Connect request must be Rx before RTU\n");
                        return -EINVAL;
                }
        } else if ((event == RDMA_CM_EVENT_ADDR_RESOLVED) ||
                   (event == RDMA_CM_EVENT_ROUTE_RESOLVED)) {
                /* no error */
        } else if (event == RDMA_CM_EVENT_DISCONNECTED) {
                srdma->disconnects++;
                if (srdma->disconnects >= srdma->rx_test_qp_cnt)
                        exit(0);
        } else {
                printf("Event %s:%d is not being handled\n",
                        rdma_event_str(event), event);
                /* remove entry */
                return -1;
        }
        return 0;
}

int rdmatest_poll_cq(struct rdma_cb* rdma_ctx, unsigned id)
{
        struct ibv_wc wc;
        void* cq_ctx;
        struct ibv_cq* cq_event;
	int ret;

        while (1) {
                if (ibv_get_cq_event(rdma_ctx->channel,&cq_event, &cq_ctx))
                        return -EFAULT;
                if (ibv_req_notify_cq(rdma_ctx->cq, 0))
                        return -EFAULT;
		ret = ibv_poll_cq(rdma_ctx->cq, 1, &wc);
		if (ret < 0) {
			printf("Poll failed\n");
			return -EINVAL;
		}

		if (ret == 0)
			continue;

                if (wc.status != IBV_WC_SUCCESS) {
                        printf("ernic wc status = %d(%s), id = %ld:%d\n",
                                        wc.status,
                                        ibv_wc_status_str(wc.status),
                                        wc.wr_id, id);
                        return -EINVAL;
                }

                if (wc.wr_id == id)
                        return 0;
        }
}


int test_server_rdma(struct rdma_cb *ctx)
{
        struct rdma_cm_id* cm_id;
        struct ibv_recv_wr *bad_wr;
        int err, r, i = 0, cq_depth = 1;
        static int qp_cnt = 0;
	struct rdma_info *rx_buf;

        return 0;
}


int run_server()
{
        struct ctx_list *list;
        int err;
        int cnt = 0;

        /* Create rdma CM thread */
        err = pthread_create(&sgen_test.cmthread, NULL, cm_thread,
                             (void *)&sgen_test);
        if (err) {
                printf("Failed to create CM thread in rdma\n");
                return err;
        }

        err = pthread_create(&sgen_test.datathread, NULL, data_thread,
                             (void *)&sgen_test);
        if (err) {
                printf("Failed to create establishment thread\n");
                return err;
        }
        while (1) {
                pthread_mutex_lock(&sgen_test.cm_list_lock);
                if (!sgen_test.head) {
                        pthread_mutex_unlock(&sgen_test.cm_list_lock);
                        continue;
                }

                list = sgen_test.head;
                while (list) {
                        if (list->state == READY_FOR_DATA_TRANSFER) {
                                err = test_server_rdma(&list->ctx);
                                cnt++;
                                if (cnt >= sgen_test.rx_test_qp_cnt)
                                        return 0;
                                list->state = INVALID_STATE;
                                /* delete the entry */
                        }
                        list = list->next;
                }
                pthread_mutex_unlock(&sgen_test.cm_list_lock);
        }
}


int gen_test_client(struct rdma_cb *cb)
{
	int ping, start, cc, i, ret = 0;
	struct ibv_send_wr *bad_wr;
	struct ibv_recv_wr *bad_rx_wr;
	struct ibv_wc wc;
	unsigned char c;
	int r = rand();

	/* Execute normal rdma test */
	start = 65;
	for (ping = 0; !cb->count || ping < cb->count; ping++) {
		cb->state = RDMA_READ_ADV;

		/* Put some ascii text in the buffer. */
		rdma_format_send(cb, cb->rdma_buf_ofs, cb->rdma_mr);
		cb->sq_wr.wr_id = r;
		ret = ibv_post_send(cb->qp, &cb->sq_wr, &bad_wr);
		if (ret) {
			printf( "post send error %d\n", ret);
			break;
		}

		sem_wait(&cb->sem_data);
		if (cb->state != RDMA_WRITE_ADV) {
			printf("Expected post recv\n");
			return -EFAULT;
		}

		ret = ibv_post_recv(cb->qp, &cb->rq_wr, &bad_rx_wr);
		if (ret) {
			printf("ibv_post_recv failed: %d\n", ret);
		}

		/* send RDMA */
		{
			uint64_t va = be64toh(cb->recv_buf.va), lp_var = 0;
		        uint64_t tot_iter = (uint64_t)((uint64_t)(gen_test.size_in_mb * 1024)) / ((uint64_t)gen_test.pkt_size);
			struct ibv_wc wc;
			uint32_t rkey = be32toh(cb->recv_buf.rkey);

			while(lp_var < tot_iter) {
				if (gen_test.read_op)
					ret = rdma_post_read(cb->cm_id, (void *)(uintptr_t)(r + lp_var + 1),
						(char *) cb->rdma_buf,	gen_test.pkt_size,
						cb->rdma_mr, IBV_SEND_SIGNALED,
						va + lp_var * gen_test.pkt_size * 1024, rkey);
				else
					ret = rdma_post_write(cb->cm_id, (void *)(uintptr_t)(r + lp_var + 1),
								(char *) cb->rdma_buf,	gen_test.pkt_size,
								cb->rdma_mr, IBV_SEND_SIGNALED,
								va + lp_var * gen_test.pkt_size * 1024, rkey);
				if (ret) {
					printf("Failed to RDMA %s:%d\n", __func__, __LINE__);
					return -EFAULT;
				}

				ret = rdmatest_poll_cq(cb,r + lp_var + 1);
				if (ret)
					return ret;
				lp_var++;
			}
		}
	}

	return (cb->state == DISCONNECTED) ? 0 : ret;
}

static int rdma_connect_client(struct rdma_cb *cb)
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

static int rdma_bind_client(struct rdma_cb *cb)
{
	int ret;

	if (gen_test.sin.ss_family == AF_INET)
		((struct sockaddr_in *) &gen_test.sin)->sin_port = gen_test.port;
	else
		((struct sockaddr_in6 *) &gen_test.sin)->sin6_port = gen_test.port;

	if (gen_test.ssource.ss_family)
		ret = rdma_resolve_addr(cb->cm_id, (struct sockaddr *) &gen_test.ssource,
					(struct sockaddr *) &gen_test.sin, 2000);
	else
		ret = rdma_resolve_addr(cb->cm_id, NULL, (struct sockaddr *) &gen_test.sin, 2000);

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

static int rdma_run_client(struct rdma_cb *cb)
{
	struct ibv_recv_wr *bad_wr;
	int ret;

	ret = rdma_bind_client(cb);
	if (ret)
		return ret;

	ret = rdma_setup_qp(cb, cb->cm_id);
	if (ret) {
		printf("setup_qp failed: %d\n", ret);
		return ret;
	}

	ret = rdma_setup_buffers(cb);
	if (ret) {
		printf("rdma_setup_buffers failed: %d\n", ret);
		goto err1;
	}

	ret = ibv_post_recv(cb->qp, &cb->rq_wr, &bad_wr);
	if (ret) {
		printf("ibv_post_recv failed: %d\n", ret);
		goto err2;
	}

	if (!gen_test.cq_init) {
		ret = pthread_create(&gen_test.cqthread, NULL, cq_thread, &gen_test);
		if (ret) {
			printf("pthread_create");
			goto err2;
		}
		gen_test.cq_init = 1;
	}

	ret = rdma_connect_client(cb);
	if (ret) {
		printf("connect error %d\n", ret);
		goto err2;
	}

	return 0;
err4:
	rdma_disconnect(cb->cm_id);
err2:
	rdma_free_buffers(cb);
err1:
	rdma_free_qp(cb);

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
	printf("\t-p <port> \t:port number\n");
	printf("\t-a <addr> \t:address\n");
	printf("\t-d\t:debug prints\n");
	printf("\t-s <val> : total payload size in MB\n");
	printf("\t-P <val> : Payload per RDMA packet in KB\n");
	printf("\t-R : Do RDMA READ. If this option is not specified, default option is WRITE\n");
}

int main(int argc, char *argv[])
{
	struct rdma_cb *cb;
	char *end;
	int op, i = 0;
	int ret = 0, data[3];
	float qp_bw, tot_bw = 0;
	int persistent_server = 0;

	gen_test.server = 0;
	gen_test.pkt_size = PAYLOAD_SIZE;
	gen_test.sin.ss_family = PF_INET;
	gen_test.port = htobe16(18516);
	gen_test.qp_cnt = 1;
	gen_test.cq_init = 0;
	gen_test.count = 0;
	opterr = 0;
	gen_test.server = 0;
	gen_test.read_op = 0;
	gen_test.size_in_mb = 0;
	gen_test.pkt_size = 512; //512KB

	if (argc == 1) {
		usage(argv[0]);
		return 0;
	}

	while ((op = getopt(argc, argv, "a:p:dRs:P:h")) != -1) {
		switch (op) {
		case 'P':
			gen_test.pkt_size = atoi(optarg);
			printf("Payload per packet is %d KB\n", gen_test.pkt_size);
			break;
		case 's':
			gen_test.size_in_mb = atoi(optarg);
			printf("Total payload to be tested is %d MB\n", gen_test.size_in_mb);
			break;
		case 'R':
			gen_test.read_op = 1;
			break;
		case 'a':
			ret = get_addr(optarg, (struct sockaddr *) &gen_test.sin);
			break;
		case 'p':
			gen_test.port = htobe16(atoi(optarg));
			DEBUG_LOG("port %d\n", gen_test.port);
			break;
		case 'd':
			debug++;
			break;
		case 'h':
			usage(argv[0]);
			return 0;
		default:
			printf("Rx invalid option\n");
			return -EINVAL;
		}
	}


	if (!gen_test.count)
		gen_test.count = 1;

	if (gen_test.server == 0) {
		i = sizeof(struct rdma_cb) * gen_test.qp_cnt;
		gen_test.cb = malloc(i);
		if (!gen_test.cb) {
			printf("Failed to allocate memory for contexts\n");
			return -ENOMEM;
		}
		memset(gen_test.cb, 0, i);

		/* create reqd threads */
		ret = pthread_create(&gen_test.cmthread, NULL, cm_thread, &gen_test);
		if (ret) {
			perror("failed to create cm thread");
			free(gen_test.cb);
			return -1;
		}

		ret = pthread_create(&gen_test.datathread, NULL, data_thread, &gen_test);
		if (ret) {
			perror("Failed to create data transfer threads\n");
			free(gen_test.cb);
			return -1;
		}

		if (!gen_test.qp_cnt) {
			printf("Qp count cant be 0\n");
			return -1;
		}

		for (i = 0; i < gen_test.qp_cnt; i++) {
			cb = &gen_test.cb[i];
			memset(cb, 0, sizeof(struct rdma_cb));
			sem_init(&cb->sem, 0, 0);
			sem_init(&cb->sem_data, 0, 0);
			cb->qp_num = i;
			cb->cm_fail = cb->cq_fail = 0;
			cb->cm_chnl_en = 0;
			cb->cm_established = 0;
			cb->count = gen_test.count;
			cb->test_done = 0;
			cb->cm_channel = rdma_create_event_channel();
			if (!cb->cm_channel) {
				ret = errno;
				printf("event channel creation failed for app qp #%d\n", i);
				gen_test.done_cnt++;
				return -EFAULT;
			}
			ret = rdma_create_id(cb->cm_channel, &cb->cm_id, cb, RDMA_PS_TCP);
			if (ret) {
				printf("ID creation failed for app qp #%d\n", i);
				gen_test.done_cnt++;
				return -EFAULT;
			}
			DEBUG_LOG("created cm_id %p\n", cb->cm_id);
			cb->cm_chnl_en = 1;
			ret = rdma_run_client(cb);
			if (ret) {
				printf("CM Failed for application QP #%d\n", i);
				/* Disable looking for events on this channel */
				cb->cm_chnl_en = 0;
				gen_test.done_cnt++;
			}

			DEBUG_LOG("Init done for app qp #%d\n", i);
		}

		if (ret)
			return -EFAULT;

		while(gen_test.done_cnt < gen_test.qp_cnt);

		for (i = 0; i < gen_test.qp_cnt; i++) {
			rdma_destroy_qp(gen_test.cb[i].cm_id);
			rdma_destroy_id(gen_test.cb[i].cm_id);
			rdma_destroy_event_channel(gen_test.cb[i].cm_channel);
		}
		return ret;
	} else {
		printf("Server functionlity is not implemented\n");
	}
}
