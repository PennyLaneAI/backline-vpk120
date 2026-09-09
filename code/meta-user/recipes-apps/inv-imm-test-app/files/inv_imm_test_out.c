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
#include <umm_export.h>
#include "inv_imm.h"

static int debug = 0;
#define DEBUG_LOG if (debug) printf

unsigned int mem_type = XMEM_PL_DDR;

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
		cb->child_cm_id = cma_id;
		DEBUG_LOG("child cma %p\n", cb->child_cm_id);
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

static int server_recv(struct rping_cb *cb, struct ibv_wc *wc)
{
	cb->remote_rkey = be32toh(cb->recv_buf.rkey);
	cb->remote_addr = be64toh(cb->recv_buf.buf);
	cb->remote_len  = be32toh(cb->recv_buf.size);
	DEBUG_LOG("Received rkey %x addr %" PRIx64 " len %d from peer\n",
		  cb->remote_rkey, cb->remote_addr, cb->remote_len);

	if (cb->state <= CONNECTED || cb->state == RDMA_WRITE_COMPLETE) {
		cb->state = RDMA_READ_ADV;
	} else {
		cb->state = RDMA_WRITE_ADV;
	}
	return 0;
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
				printf("send completion\n");
				break;
			case IBV_WC_RDMA_WRITE:
				printf("rdma write completion\n");
				cb->state = RDMA_WRITE_COMPLETE;
				sem_post(&cb->sem_data);
				break;

			case IBV_WC_RDMA_READ:
				printf("rdma read completion\n");
				cb->state = RDMA_READ_COMPLETE;
				sem_post(&cb->sem_data);
				break;

			case IBV_WC_RECV:
				printf("recv completion\n");
				ret = rping_test.server ? server_recv(cb, &wc[loop]) :
						   client_recv(cb, &wc[loop]);
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
				printf("unknown!!!!! completion\n");
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

static int rping_accept(struct rping_cb *cb)
{
	int ret;

	DEBUG_LOG("accepting client connection request\n");

	ret = rdma_accept(cb->child_cm_id, NULL);
	if (ret) {
		perror("rdma_accept");
		return ret;
	}

	sem_wait(&cb->sem);
	if (cb->state == ERROR) {
		printf("wait for CONNECTED state %d\n", cb->state);
		return -1;
	}
	return 0;
}

static void rping_setup_wr(struct rping_cb *cb)
{
	cb->recv_sgl.addr = (uint64_t) (unsigned long) &cb->recv_buf;
	cb->recv_sgl.length = sizeof cb->recv_buf;

	cb->rq_wr.sg_list = &cb->recv_sgl;
	cb->rq_wr.num_sge = 1;

	cb->send_sgl.addr = (uint64_t) (unsigned long) cb->send_buf;
	cb->send_sgl.length = sizeof (*cb->send_buf);
	cb->send_sgl.lkey = cb->send_buf_chunk_id;

	cb->sq_wr.opcode = IBV_WR_SEND_WITH_IMM;
	cb->sq_wr.send_flags = IBV_SEND_SIGNALED;
	cb->sq_wr.sg_list = &cb->send_sgl;
	cb->sq_wr.num_sge = 1;

	cb->rdma_sgl.addr = (uint64_t) (unsigned long) cb->rdma_buf;
	cb->rdma_sgl.lkey = cb->rdma_mr->lkey;
	cb->rdma_sq_wr.send_flags = IBV_SEND_SIGNALED;
	cb->rdma_sq_wr.sg_list = &cb->rdma_sgl;
	cb->rdma_sq_wr.num_sge = 1;
}

static int rping_setup_buffers(struct rping_cb *cb)
{
	int ret;
	unsigned int chunk_id, data_size;
	uint64_t vaddr;
	bool proc_access = true;

	data_size = sizeof(*cb->send_buf);
	chunk_id = xib_umem_alloc_chunk(cb->cm_id->verbs,
			XMEM_PL_DDR, data_size, data_size,
			proc_access);

	if (chunk_id < 0) {
		printf("Failed to alloc chunk %d\n", __LINE__);
		cb->send_buf_chunk_id = -EINVAL;
		return -EFAULT;
        }
	cb->send_buf_chunk_id = chunk_id;

        vaddr = xib_umem_alloc_mem(cb->cm_id->verbs, chunk_id, data_size);
        if (XMEM_IS_INVALID_VADDR(vaddr)) {
                printf("Failed to alloc memory from chunk\n");
		ret |= xib_umem_free_chunk(cb, cb->send_buf_chunk_id);
		cb->send_buf_chunk_id = -EINVAL;
                return -EFAULT;
        }

	cb->send_buf = vaddr;

	data_size = (4 * 1024);
	chunk_id = xib_umem_alloc_chunk(cb->cm_id->verbs,
			mem_type, PAYLOAD_SIZE, PAYLOAD_SIZE,
			proc_access);
	if (chunk_id < 0) {
		printf("Failed to alloc chunk %d\n", __LINE__);
		cb->rdma_buf_chunk_id = -EINVAL;
		goto err1;
        }
	cb->rdma_buf_chunk_id = chunk_id;

        vaddr = xib_umem_alloc_mem(cb->cm_id->verbs, chunk_id, data_size);
        if (XMEM_IS_INVALID_VADDR(vaddr)) {
                printf("Failed to alloc memory from chunk\n");
		ret |= xib_umem_free_chunk(cb, cb->rdma_buf_chunk_id);
		cb->rdma_buf_chunk_id = -EINVAL;
		goto err1;
        }

	cb->rdma_buf = vaddr;

	cb->rdma_mr = ibv_reg_mr_ex(cb->pd, cb->rdma_buf, rping_test.size,
				 IBV_ACCESS_LOCAL_WRITE |
				 IBV_ACCESS_REMOTE_READ |
				 IBV_ACCESS_REMOTE_WRITE);
	if (!cb->rdma_mr) {
		printf("rdma_buf reg_mr failed\n");
		ret = errno;
		goto err2;
	}

	rping_setup_wr(cb);
	DEBUG_LOG("allocated & registered buffers...\n");
	return 0;

err2:
	ret |= xib_umem_free_mem(cb, cb->rdma_buf_chunk_id,
			(uint64_t)cb->rdma_buf,
			PAYLOAD_SIZE);
	ret |= xib_umem_free_chunk(cb, cb->rdma_buf_chunk_id);
	cb->rdma_buf_chunk_id = -EINVAL;
err1:
	ret |= xib_umem_free_mem(cb, cb->send_buf_chunk_id,
			(uint64_t)cb->send_buf,
			sizeof(*cb->send_buf));
	ret |= xib_umem_free_chunk(cb, cb->send_buf_chunk_id);
	cb->send_buf_chunk_id = -EINVAL;
	return -EFAULT;
}

static void rping_free_buffers(struct rping_cb *cb)
{
	int ret;

	ibv_dereg_mr(cb->rdma_mr);
	ret |= xib_umem_free_mem(cb, cb->rdma_buf_chunk_id,
			(uint64_t)cb->rdma_buf,
			PAYLOAD_SIZE);
	ret |= xib_umem_free_chunk(cb, cb->rdma_buf_chunk_id);
	cb->rdma_buf_chunk_id = -EINVAL;

	ret |= xib_umem_free_mem(cb, cb->send_buf_chunk_id,
			(uint64_t)cb->send_buf,
			sizeof(*cb->send_buf));
	ret |= xib_umem_free_chunk(cb, cb->send_buf_chunk_id);
	cb->send_buf_chunk_id = -EINVAL;
}

static int rping_create_qp(struct rping_cb *cb)
{
	struct ibv_qp_init_attr init_attr;
	int ret;

	memset(&init_attr, 0, sizeof(init_attr));
	init_attr.cap.max_send_wr = rping_test.q_depth;
	init_attr.cap.max_recv_wr = rping_test.q_depth;
	init_attr.cap.max_recv_sge = (PAYLOAD_SIZE / ERNIC_SGE_SIZE);
	init_attr.cap.max_send_sge = 1;
	init_attr.qp_type = IBV_QPT_RC;
	init_attr.send_cq = cb->cq;
	init_attr.recv_cq = cb->cq;

	if (rping_test.server) {
		ret = rdma_create_qp(cb->child_cm_id, cb->pd, &init_attr);
		if (!ret)
			cb->qp = cb->child_cm_id->qp;
	} else {
		ret = rdma_create_qp(cb->cm_id, cb->pd, &init_attr);
		if (!ret)
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

	cb->channel = ibv_create_comp_channel(cm_id->verbs);
	if (!cb->channel) {
		printf("ibv_create_comp_channel failed\n");
		ret = errno;
		goto err1;
	}
	DEBUG_LOG("created channel %p\n", cb->channel);

	cb->cq = ibv_create_cq(cm_id->verbs, rping_test.q_depth, cb,
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
	int ret, i;

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
		sched_yield();
	}
}

static void *cm_thread(void *arg)
{
	struct rping_test_info *rping_info = arg;
	struct rping_cb *cb;
	int ret, i;

	DEBUG_LOG("cm thread started\n");
	while (1) {
		for (i = 0; i < rping_info->qp_cnt; i++) {
			cb = &rping_info->cb[i];
			if ((!cb->cm_fail) && cb->cm_chnl_en) {
				ret = rdma_get_cm_event(cb->cm_channel, &cb->event);
				if (ret) {
					printf("rdma_get_cm_event failed CB=%#x", cb);
					cb->cm_fail = 1;
					continue;
				}

				ret = rping_cma_event_handler(cb->event->id, cb->event);
				rdma_ack_cm_event(cb->event);
				if (ret) {
					printf("Ack cm event failed for %#x\n", cb);
					cb->cm_fail = 1;
				}
			}
		}
#if 0
		if (rping_test.est_qp_cnt == rping_info->qp_cnt)
			pthread_exit((void *)(uintptr_t)0);
#endif
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
#if 0
		if (rping_test.done_cnt >= rping_info->qp_cnt)
			pthread_exit((void *)(uintptr_t)0);
#endif
		for (i = 0; i < rping_info->qp_cnt; i++) {
			cb = &rping_info->cb[i];
			if ((!cb->cq_fail) && cb->cm_established) {
				ret = ibv_req_notify_cq(cb->cq, 0);
				if (ret) {
					printf("Failed to set cq notify for Cb %#x\n", cb);
					cb->cq_fail = 1;
					continue;
				}
				ret = rping_cq_event_handler(cb);
				if (cb->poll_pkt_cnt) {
					ibv_ack_cq_events(cb->cq, 1);
					cb->poll_pkt_cnt = 0;
				}
				if (cb->state == ERROR) {
					cb->cq_fail = 1;
					printf("Failed to ack cq events for cb %#x\n", cb);
				}
			}
		}
	}
}

static void rping_format_send(struct rping_cb *cb, uint64_t buf, struct ibv_mr *mr)
{
	struct rping_rdma_info *info = (struct rping_rdma_info *)cb->send_buf;

	info->buf = htobe64(buf);
	info->rkey = htobe32(mr->rkey);
	info->size = htobe32(rping_test.size);
	info->iter_cnt = htobe32(cb->count);
	info->qp_cnt = htobe32(rping_test.qp_cnt);
	DEBUG_LOG("RDMA addr %" PRIx64" rkey %x len %d\n",
		  be64toh(info->buf), be32toh(info->rkey), be32toh(info->size));
}

static int rping_bind_server(struct rping_cb *cb)
{
	int ret;

	if (rping_test.sin.ss_family == AF_INET)
		((struct sockaddr_in *) &rping_test.sin)->sin_port = rping_test.port;
	else
		((struct sockaddr_in6 *) &rping_test.sin)->sin6_port = rping_test.port;

	ret = rdma_bind_addr(cb->cm_id, (struct sockaddr *) &rping_test.sin);
	if (ret) {
		perror("rdma_bind_addr");
		return ret;
	}
	DEBUG_LOG("rdma_bind_addr successful\n");

	DEBUG_LOG("rdma_listen\n");
	ret = rdma_listen(cb->cm_id, 3);
	if (ret) {
		perror("rdma_listen");
		return ret;
	}

	return 0;
}

static struct rping_cb *clone_cb(struct rping_cb *listening_cb)
{
	struct rping_cb *cb = malloc(sizeof *cb);
	if (!cb)
		return NULL;
	memset(cb, 0, sizeof *cb);
	*cb = *listening_cb;
	cb->child_cm_id->context = cb;
	return cb;
}

static void free_cb(struct rping_cb *cb)
{
	free(cb);
}

int poll_cq(struct rping_cb *cb, int id)
{
        struct ibv_wc wc;
        int ret;
        struct ibv_cq* cq_event;
        void* cq_ctx;

        while(1) {
#if 0
                if (ibv_get_cq_event(cb->channel, &cq_event, &cq_ctx))
                        continue;
                if (ibv_req_notify_cq(cb->cq, 0))
                        continue;
#endif
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


int rping_test_client(struct rping_cb *cb)
{
	int ping, start, cc, i, ret = 0;
	struct ibv_send_wr *bad_wr;
	struct ibv_recv_wr *bad_rx_wr;
	unsigned char c;
	int r = rand();
	struct rping_rdma_info *rx_buf;

	/* Execute normal rping test */
	start = 65;
	if(1) {

		/* post recv to recv va and rkey */
		cb->rq_wr.wr_id = r;
		ret = ibv_post_recv(cb->qp, &cb->rq_wr, &bad_rx_wr);
		if (ret) {
			printf("ibv_post_recv failed: %d\n", ret);
			return ret;
		}

		r++;

		rping_format_send(cb, cb->rdma_buf, cb->rdma_mr);
		cb->sq_wr.wr_id = r;
		cb->sq_wr.opcode = IBV_WR_SEND;
		ret = ibv_post_send(cb->qp, &cb->sq_wr, &bad_wr);
		if (ret) {
			printf( "post send error %d\n", ret);
			return ret;
		}

		/* poll for rx completions*/
		ret = poll_cq(cb, r - 1);
		if (ret) {
			printf("Poll cq error %d\n", __LINE__);
			return ret;
		}
		r++;

		rx_buf = cb->rq_wr.sg_list[0].addr;
                cb->remote_addr = rx_buf->buf;
                cb->remote_rkey = rx_buf->rkey;
                cb->remote_len  = rx_buf->size;

		if (rping_test.test_flags & (1U << SEND_IMM_TEST)) {
			cb->sq_wr.wr_id = r;
			cb->sq_wr.imm_data = htobe32(0x12345678);
			cb->sq_wr.opcode = IBV_WR_SEND_WITH_IMM;
			ret = ibv_post_send(cb->qp, &cb->sq_wr, &bad_wr);
			if (ret) {
				printf( "post send error %d at line %d\n", ret, __LINE__);
				return -EFAULT;
			}

			ret = poll_cq(cb, r);
			if (ret) {
				printf("Send with immediate failed%d\n", __LINE__);
				return -EFAULT;
			}
			printf("Send immediate successful\n");
			return 0;
		} else if (rping_test.test_flags & (1U << WRITE_IMM_TEST)) {
			/* Post write with immediate */
			cb->sq_wr.wr_id = r;
			cb->sq_wr.opcode = IBV_WR_RDMA_WRITE;
			cb->sq_wr.wr.rdma.rkey = cb->remote_rkey;
			cb->sq_wr.wr.rdma.remote_addr = cb->remote_addr;
			memcpy((char*)&cb->sq_wr.wr.rdma.remote_addr, &cb->remote_addr, sizeof(cb->remote_addr));
			cb->sq_wr.sg_list->length = PAYLOAD_SIZE;

			cb->sq_wr.imm_data = htobe32(0x12345678);
			rping_format_send(cb, cb->rdma_buf, cb->rdma_mr);
			ret = ibv_post_send(cb->qp, &cb->sq_wr, &bad_wr);
			if (ret) {
				printf("post RDMA write failed %d\n", ret);
				return -EFAULT;
			}

			ret = poll_cq(cb, r);
			if (ret) {
				printf("Poll CQ for RDMA write failed %d\n", __LINE__);
				return -EFAULT;
			}
			r++;

			cb->sq_wr.wr_id = r;
			cb->sq_wr.sg_list->length = PAYLOAD_SIZE;
			cb->sq_wr.opcode = IBV_WR_RDMA_WRITE_WITH_IMM;
		} else if (rping_test.test_flags & (1U << INV_TEST)) {
			cb->sq_wr.wr_id = r;
			cb->sq_wr.sg_list->length = PAYLOAD_SIZE;
			cb->sq_wr.wr.rdma.remote_addr = cb->remote_addr;
			cb->sq_wr.opcode = IBV_WR_SEND_WITH_INV;

			cb->sq_wr.invalidate_rkey = cb->remote_rkey;
		}
		ret = ibv_post_send(cb->qp, &cb->sq_wr, &bad_wr);
		if (ret) {
			printf( "post send error %d at line %d\n", ret, __LINE__);
			return -EFAULT;
		}

		ret = poll_cq(cb, r);
		if (ret) {
			printf("Poll cq error %d at line %d\n", ret, __LINE__);
			return -EFAULT;
		}

		if (rping_test.test_flags & (1U << WRITE_IMM_TEST)) {
			if (ret)
				printf("Write immediate test failed\n");
			else
				printf("Write immediate test passed\n");
		} else {
			if (ret)
				printf("Send invalidate test failed\n");
			else
				printf("Send invalidate test passed\n");
		}
		return ret;
	}
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
	printf("\ninv_imm_test_out client application:\n");
	printf("\t-h\t: Display help\n");
	printf("\t-d\t:debug prints\n");
	printf("\t-a <addr> \t:address\n");
	printf("\t-D <depth> \t:Queue depth\n");
	printf("\t-t <test name> : Test name\n"
		"\t\t simm : Send with immediate test\n"
		"\t\t wimm : write with immediate test\n"
		"\t\t inv : Send with invalidate test\n");
	printf("\t-m <type> : RDMA buffers memory type [pl, eddr, ps , bram]\n");
}

static void init_comm_defaults(struct rping_cb *cb)
{
	memset(cb, 0, sizeof(struct rping_cb));
	sem_init(&cb->sem, 0, 0);
	sem_init(&cb->sem_data, 0, 0);
	cb->cm_fail = cb->cq_fail = 0;
	cb->cm_chnl_en = 0;
	cb->cm_established = 0;
	cb->count = rping_test.count;
	cb->test_done = 0;
}

int main(int argc, char *argv[])
{
	struct rping_cb *cb;
	char *end;
	int op, i = 0;
	int ret = 0, data[3];
	float qp_bw, tot_bw = 0;
	int persistent_server = 0;
	char test_str[15] = "\0";

	rping_test.server = 0;
	rping_test.size = PAYLOAD_SIZE;
	rping_test.sin.ss_family = PF_INET;
	rping_test.port = htobe16(7179);
	rping_test.qp_cnt = 1;
	rping_test.cq_init = 0;
	rping_test.count = 0;
	opterr = 0;
	rping_test.q_depth = 32;

	if (argc == 1) {
		usage(argv[0]);
		return 0;
	}

	while ((op = getopt(argc, argv, "a:p:C:cdQ:t:m:D:")) != -1) {
		switch (op) {
		case 'D':
			rping_test.q_depth = atoi(optarg);
			printf("Queue depths is %d\n", rping_test.q_depth);
			break;
		case 't':
			strncpy(test_str, optarg, strlen(optarg));
			printf("Test name is: %s\n", test_str);
			break;

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
		case 'C':
			rping_test.count = atoi(optarg);
			if (rping_test.count < 0) {
				printf( "Invalid count %d\n",
					rping_test.count);
				ret = EINVAL;
			} else
				DEBUG_LOG("count %d\n", (int) rping_test.count);
			break;
		case 'm':
			if ((ret = xib_mem_str_to_mem_type(optarg)) < 0)
				return ret;
			mem_type = ret;
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
			return -1;;
		}
	}

	/* parse test names and set corresponding test flags */
	if (!strncasecmp(test_str, SEND_IMM_TEST_STR, sizeof(SEND_IMM_TEST_STR))) {
		rping_test.test_flags |= (1U << SEND_IMM_TEST);
		printf("Immediate opcode test is requested\n");
	} else if (!strncasecmp(test_str, INV_TEST_STR, sizeof(INV_TEST_STR))) {
		rping_test.test_flags |= (1U << INV_TEST);
		printf("Invalidate opcode test is requested\n");
	} else if (!strncasecmp(test_str, WRITE_IMM_TEST_STR, sizeof(WRITE_IMM_TEST_STR))) {
		rping_test.test_flags |= (1U << WRITE_IMM_TEST);
		printf("write immediate opcode test is requested\n");
	} else {
		printf("Invalid test string %s\n", test_str);
		return -EINVAL;
	}

	if (!rping_test.count)
		rping_test.count = 1;

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
		init_comm_defaults(cb);
		cb->qp_num = i;
		cb->cm_channel = rdma_create_event_channel();
		if (!cb->cm_channel) {
			ret = errno;
			printf("event channel creation failed for app qp #%d\n", i);
			rping_test.done_cnt++;
			return;
		}
		ret = rdma_create_id(cb->cm_channel, &cb->cm_id, cb, RDMA_PS_TCP);
		if (ret) {
			printf("ID creation failed for app qp #%d\n", i);
			rping_test.done_cnt++;
			return;
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
	}

	if (ret)
		return -EFAULT;

	while(rping_test.done_cnt < rping_test.qp_cnt);

	for (i = 0; i < rping_test.qp_cnt; i++) {
		rdma_destroy_qp(rping_test.cb[i].cm_id);
		rdma_destroy_id(rping_test.cb[i].cm_id);
		rdma_destroy_event_channel(rping_test.cb[i].cm_channel);
	}
	return ret;
}
