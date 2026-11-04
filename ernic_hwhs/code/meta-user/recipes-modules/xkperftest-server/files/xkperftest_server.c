#include <linux/version.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/inet.h>
#include <linux/slab.h>
#include <linux/err.h>
#include <linux/string.h>
#include <linux/list.h>
#include <linux/in.h>
#include <linux/device.h>
#include <linux/workqueue.h>
#include <linux/wait.h>
#include <linux/preempt.h>
#include <rdma/ib_verbs.h>
#include <rdma/rdma_cm.h>
#include <net/addrconf.h>
#include "xib_kmm_export.h"

#define KPERFTEST_PORT		18515
#define PERF_TEST_DATA_QPS	2
/* BRAM has only 8K. From 8K, we also need to alloc SQ, RQ and so on */
#define MAX_DATA_SIZE		(4096)
#define EXT_DDR_STR		"eddr"

static char server_ip[50] = "--";
static char sgl_mem[10] = "bram";
static char ctx_buf[10] = "pl";

unsigned int size = 4; // 4kB
unsigned int osq = 16;
unsigned int rdma_send = 0;
unsigned int kperftest_port = 0;
unsigned int q_depth = 32;

module_param(size, int, 0664);
module_param(osq, int, 0664);
module_param(rdma_send, int, 0664);
module_param(q_depth, int, 0664);
module_param(kperftest_port, int, 0664);
module_param_string(ctx_buf, ctx_buf, sizeof(ctx_buf), S_IRUGO);
module_param_string(sgl_mem, sgl_mem, sizeof(sgl_mem), S_IRUGO);
module_param_string(server_ip, server_ip, sizeof(server_ip), S_IRUGO);

#define KPERF_SQ_CQ_DEPTH	(16)

enum app_qp_state {
	IDLE_STATE = 0,
	CON_ACCEPTED,
	SEND_COMPLETION,
	RX_COMPLETION,
	CTX_EXCHANGED,
};

struct pingpong_dest {
        u64                     rsvd1;
        int                     qpn;
        int                     rsvd2;
        unsigned 		rkey;
        u64      		vaddr;
        char                    rsvd3[24];
};

struct kpt_qp_ctx {
	wait_queue_head_t	wait_q;
	char			*data_buf;
	struct ib_mr		*rdma_mr;
	struct ib_cq		*cq;
	struct ib_pd		*pd;
	struct ib_qp		*qp;
	struct rdma_cm_id	*child_cm_id, *cm_id;
	u64			buf_pa;
	enum app_qp_state	state;
	struct ib_recv_wr	rq_wr;
	struct ib_sge		rx_sgl, send_sgl;/* recv single SGE */
	struct ib_send_wr	sq_wr;
	unsigned int		qpn;
	struct pingpong_dest	*send_buf, *rx_buf;
	u64			send_buf_pa, rx_buf_pa;
	void			*buf_va;
	struct work_struct	cq_work;
	int			cq_valid, connect_valid;
	u32			send_buf_id, rx_buf_id;
	unsigned int		sq_mem_type, rx_mem_type;
};

struct kperf_test {
	unsigned int	con_cnt;
	/* perftest only uses 2 QPs */
	struct kpt_qp_ctx	qp_ctx[2];
	struct rdma_cm_id *cm_id;
	struct ib_mr		*rdma_mr;
	struct ib_pd		*pd;
	unsigned int		mr_size;
	u64			buf_pa;
	void			*buf_va;
	struct workqueue_struct *wqueue;
	u32			buf_id;
	unsigned int		buf_mem_type;
} kpt_test;

int handle_cm_event(struct kpt_qp_ctx *ctx);
int qp_post_send(struct kpt_qp_ctx *qp_ctx);

u64 eddr_va;

int check_duplicate_con_req(struct rdma_cm_id *cma_id, struct kpt_qp_ctx *cnt_list)
{
	unsigned int i;
	for (i = 0; i < PERF_TEST_DATA_QPS; i++)
		if (cnt_list[i].child_cm_id == cma_id)
			return i;

	return -1;
}

void qp_cq_handler(struct work_struct *work)
{
	int ret;
	const struct ib_send_wr *bad_send_wr;
	struct kpt_qp_ctx *qp_ctx;
        const struct ib_recv_wr *bad_rx_wr;
	struct ib_cq *cq;// = ctx->cq;
	struct ib_wc wc;

	qp_ctx = container_of(work, struct kpt_qp_ctx, cq_work);
	if (!qp_ctx) {
		return;
	}

	cq = qp_ctx->cq;
        ret = ib_poll_cq(cq, 1, &wc);
        if (ret == 1) {
                /* 1 completion rx */
                if (wc.status) {
                        printk("Error completion [%d]\n", wc.status);
                        return;
                }

                switch (wc.opcode) {
                case IB_WC_SEND:
                        qp_ctx->state = SEND_COMPLETION;
                break;
                case IB_WC_RECV:
                        qp_ctx->state = RX_COMPLETION;
                        ret = ib_post_recv(qp_ctx->qp, &qp_ctx->rq_wr, &bad_rx_wr);
                        if (ret) {
                                printk("post recv failed on qp [%d]:[%d]\n", qp_ctx->qpn, __LINE__);
                                return;
                        }
			ret = qp_post_send(qp_ctx);
                break;
                }
        }
}

int qp_post_send(struct kpt_qp_ctx *qp_ctx)
{
	int ret;
	const struct ib_send_wr *bad_send_wr;
	ret = ib_post_send(qp_ctx->qp, &qp_ctx->sq_wr, &bad_send_wr);
	if (ret) {
		printk("post send failed on qp [%d]:[%d]\n", qp_ctx->qpn, __LINE__);
		return ret;
	}
	return 0;
}

void kpt_cq_event_handler(struct ib_cq *cq, void *ctx)
{
	struct kpt_qp_ctx *qp_ctx = ctx;
	int ret;
	const struct ib_recv_wr *bad_rx_wr;

	if (qp_ctx->cq_valid)
	        queue_work(kpt_test.wqueue, &qp_ctx->cq_work);
}

int kpt_create_qp(struct kpt_qp_ctx *ctx)
{
	struct ib_qp_init_attr init_attr;
	int ret;

	memset(&init_attr, 0, sizeof(init_attr));
	
	/* For flush_qp() */
	init_attr.cap.max_send_wr++;
	init_attr.cap.max_recv_wr++;

	init_attr.cap.max_recv_wr = q_depth;
	if (rdma_send) {
#if 1
		/*DMA alloc fails for more then 4MB of payload*/
		if (size > (4096 / init_attr.cap.max_recv_wr)) {
			printk("Can not allocate more than 4MB using dma alloc\n");
			return -ENOMEM;
		}
		init_attr.cap.max_recv_sge = (size * 1024) / 256;
#endif
	} else
		init_attr.cap.max_recv_sge = 16;
	init_attr.cap.max_send_sge = 1;
	init_attr.cap.max_send_wr = q_depth;
	init_attr.qp_type = IB_QPT_RC;
	init_attr.send_cq = ctx->cq;
	init_attr.recv_cq = ctx->cq;
	init_attr.sq_sig_type = IB_SIGNAL_REQ_WR;

	ret = rdma_create_qp(ctx->cm_id, ctx->pd, &init_attr);
	if (!ret) {
		ctx->qp = ctx->cm_id->qp;
	}
	return ret;
}

int kpt_setup_qp(struct kpt_qp_ctx *ctx)
{
	int ret;
	struct ib_cq_init_attr attr = {0};

	ctx->cm_id = ctx->child_cm_id;
	attr.cqe = q_depth;
	attr.comp_vector = 0;

	ctx->cq = ib_create_cq(ctx->cm_id->device, kpt_cq_event_handler, NULL,
				ctx, &attr);
	if (IS_ERR(ctx->cq)) {
		printk("ib_create_cq failed\n");
		ret = PTR_ERR(ctx->cq);
		goto err1;
	}

	ctx->cq_valid = 1;
	ret = kpt_create_qp(ctx);
	if (ret) {
		printk("Failed to create QP\n");
		goto err1;
	}
	return 0;
err1:
	ib_destroy_cq(ctx->cq);
	ctx->cq_valid = 0;
	return ret;
}

int setup_wr(struct kpt_qp_ctx *ctx)
{
	/* Setup RQE */
	ctx->rx_sgl.addr = ctx->rx_buf_pa;
	ctx->rx_sgl.length = sizeof(*ctx->rx_buf);
	ctx->rx_sgl.lkey = 0;
	ctx->rq_wr.sg_list = &ctx->rx_sgl;
	ctx->rq_wr.num_sge = 1;

	/* Setup SQE */
	ctx->send_sgl.addr = ctx->send_buf_pa;
	ctx->send_sgl.length = sizeof(*ctx->send_buf);
	ctx->send_sgl.lkey = 0;
	ctx->sq_wr.sg_list = &ctx->send_sgl;
	ctx->sq_wr.num_sge = 1;
	ctx->sq_wr.opcode = IB_WR_SEND;
	ctx->sq_wr.send_flags = IB_SEND_SIGNALED;
	return 0;
}

int kpt_setup_bufs(struct kpt_qp_ctx *ctx)
{
	int ret;
	const struct ib_recv_wr *bad_wr;
	u64 va = ctx->buf_va;

#ifndef CONFIG_64BIT
	if (!strcmp(sgl_mem, EXT_DDR_STR)) {
		va = eddr_va;
	} else {
		va = (u32)(uintptr_t)ctx->buf_va;
	}
#endif

	ctx->sq_mem_type = xib_mem_str_to_type(ctx_buf);
	ctx->rx_mem_type = xib_mem_str_to_type(ctx_buf);
	ctx->send_buf_pa = xib_kmm_alloc_mem(ctx->sq_mem_type, sizeof(*ctx->send_buf), &ctx->send_buf_id);
	ctx->send_buf = __va(ctx->send_buf_pa);
	if (ret) {
		printk("Error while send buf for QP [%d] allocating memory\n", ctx->qpn);
		return -ENOMEM;
	}

	ctx->rx_buf_pa = xib_kmm_alloc_mem(ctx->rx_mem_type, sizeof(*ctx->rx_buf), &ctx->rx_buf_id);
	ctx->rx_buf = __va(ctx->rx_buf_pa);
	if (ret) {
		printk("Error while rx buf for QP [%d] allocating memory\n", ctx->qpn);
		return -ENOMEM;
	}

	ctx->send_buf->rkey = htonl(ctx->rdma_mr->rkey);
	ctx->send_buf->vaddr = cpu_to_be64(va);
	ret = setup_wr(ctx);

	ret = ib_post_recv(ctx->qp, &ctx->rq_wr, &bad_wr);
	if (ret) {
		printk("post recv failed on qp [%d]\n", ctx->qpn);
	}
	return ret;
}

int kpt_accept(struct kpt_qp_ctx *ctx)
{
	struct rdma_conn_param conn_param;
	int ret;

	memset(&conn_param, 0, sizeof conn_param);
	conn_param.responder_resources = osq;
	conn_param.initiator_depth = 16;
	conn_param.retry_count = 10;
	conn_param.rnr_retry_count = 7;

	ret = rdma_accept(ctx->child_cm_id, &conn_param);
	if (ret) {
		printk("%s: Failed\n");
	}
	return ret;
}

static int kperftst_cm_handler(struct rdma_cm_id *cma_id, struct rdma_cm_event *event)
{
	int ret = 0;
	struct kperf_test *ctx = (struct kperf_test*)&kpt_test;
	struct ib_qp_attr qp_attr;
	int flags = 0;

        switch (event->event) {
        case RDMA_CM_EVENT_CONNECT_REQUEST:
	       if (ctx->con_cnt >= PERF_TEST_DATA_QPS) {
	             ret = check_duplicate_con_req(cma_id, ctx->qp_ctx);
	             if (ret >= 0) {
	                 /* Duplicate connect request, accept it*/
	              } else {
	                    pr_err("Only %d connections are accepted\n", PERF_TEST_DATA_QPS);
	                           /* reject the connection */
	                    return -EFAULT;
	              }
	        }

	        ctx->qp_ctx[ctx->con_cnt].child_cm_id = cma_id;
		ret = handle_cm_event(&ctx->qp_ctx[ctx->con_cnt]);
		if (!ret)
			ctx->con_cnt++;
        break;
	case RDMA_CM_EVENT_ESTABLISHED:
		/* Enable HW Send offload for 2nd data QP */
		if (ctx->con_cnt > 1) {
			flags |= IB_ENABLE_QP_HW_ACCL;
			ret = ib_modify_qp(ctx->qp_ctx[ctx->con_cnt - 1].qp, &qp_attr, flags);
			if (ret) {
				printk("Failed to configure QP acceleration\n");
				return -EFAULT;
			}
		}
	break;
	deafult:
		printk("Event is not being handled\n");
	break;
	}
	return ret;
}

int parse_ip_addr(struct sockaddr_storage *sadd, char *buf)
{
	size_t buf_len = strlen(buf);
	int ret, local_link = 0;
	char *temp;

	memset(sadd, 0, sizeof(*sadd));
	if (buf_len <= INET_ADDRSTRLEN) {
		struct sockaddr_in* sin_addr = (struct sockaddr_in*)sadd;

		in4_pton(buf, buf_len, (u8*)&sin_addr->sin_addr.s_addr, '\0', NULL);
		sin_addr->sin_family = AF_INET;
		if (kperftest_port == 0) {
			sin_addr->sin_port = htons(KPERFTEST_PORT);
		} else {
			sin_addr->sin_port = htons(kperftest_port);
		}
		return 0;
	} else if (buf_len <= INET6_ADDRSTRLEN) {
		struct sockaddr_in6* sin6_addr = (struct sockaddr_in6*)sadd;

		/* check for link local IP */
		temp = strstr(buf, "%");
		if (temp != NULL) {
			struct net_device *ndev;

			ndev = __dev_get_by_name(&init_net, (temp + 1));
			if (!ndev) {
				printk("Couldn't find any inteface %s\n", (temp + 1));
				return -EINVAL;
			}
			sin6_addr->sin6_scope_id = ndev->ifindex;
			dev_put(ndev);
			local_link = 1;
		}

		ret = in6_pton(buf, -1, (u8*)&sin6_addr->sin6_addr,
				-1, NULL);
		sin6_addr->sin6_family = AF_INET6;
		if (kperftest_port == 0) {
			sin6_addr->sin6_port = htons(KPERFTEST_PORT);
		} else {
			sin6_addr->sin6_port = htons(kperftest_port);
		}

		/* Check local link IP*/
		if (ipv6_addr_type(&sin6_addr->sin6_addr) & IPV6_ADDR_LINKLOCAL) {
			if (!local_link) {
				printk("For local link IPV6 addresses, ifname is must. Expected <ip-addr>%<if-name>\n");
				ret = -EINVAL;
			}
		}
		return ret;
	}

	return -EINVAL;
}

int setup_server(struct kperf_test *ctx, struct sockaddr_storage *s_addr)
{
	int ret;

        ret = rdma_listen(ctx->cm_id, 5);
	if (ret) {
		printk("RDMA listen failed\n");
		return ret;
	}
	return ret;
}

int handle_cm_event(struct kpt_qp_ctx *ctx)
{
	int ret = 0;

        ret = kpt_setup_qp(ctx);
        if (ret)
		return ret;
        ret = kpt_setup_bufs(ctx);
        if (ret)
		return ret;
        ret = kpt_accept(ctx);
        if (ret)
		return ret;
        ctx->state = CON_ACCEPTED;
	ctx->connect_valid = 1;
	return ret;
}

int init_test_ctx(struct kperf_test *ctx)
{
	unsigned int i = 0;
	u64 pa = 0;
	int ret;
	struct scatterlist sg;
	struct ib_udata udata;
	int mr_type = 0;
	ctx->wqueue = create_workqueue("own_wq");
#if 0
	ctx->mr_size = MAX_DATA_SIZE;
#endif
	/* register memory */
	ctx->pd = ib_alloc_pd(ctx->cm_id->device, 0);
	if (ctx->pd == NULL) {
		printk("Failed to alloc PD\n");
		return -EFAULT;
	}
#if 0
	{
		dma_addr_t pa1, pa2, pa3;
		void *va1, *va2, *va3;

		va1 = xib_alloc_mem("bram", 1024, &pa1);
		if (!va1)
			return -ENOMEM;
		printk("pa is %#lx\n", pa1);

		va2 = xib_alloc_mem("bram", 1024, &pa2);
		if (!va2)
			return -ENOMEM;
		printk("pa is %#lx\n", pa2);

		va3 = xib_alloc_mem("bram", 1024, &pa3);
		if (!va3)
			return -ENOMEM;
		printk("pa is %#lx\n", pa3);

		ret = xib_dealloc_mem(1024, va3, pa3);
		ret = xib_dealloc_mem(1024, va1, pa1);
		ret = xib_dealloc_mem(1024, va2, pa2);

		va1 = xib_alloc_mem("bram", 512 * 1024, &pa1);
		if (!va1)
			return -ENOMEM;

		printk("pa is %#lx\n", pa1);

		va2 = xib_alloc_mem("bram", 512 * 1024, &pa2);
		if (!va2)
			return -ENOMEM;
		printk("pa is %#lx\n", pa2);

		ret = xib_dealloc_mem(512 * 1024, va1, pa1);
		ret = xib_dealloc_mem(512 * 1024, va2, pa2);

		va1 = xib_alloc_mem("bram", 512 * 1024, &pa1);
		if (!va1)
			return -ENOMEM;
		printk("pa is %#lx\n", pa1);

		va2 = xib_alloc_mem("bram", 512 * 1024, &pa2);
		if (!va2)
			return -ENOMEM;
		printk("pa is %#lx\n", pa2);
		ret = xib_dealloc_mem(512 * 1024, va2, pa2);
		ret = xib_dealloc_mem(512 * 1024, va1, pa1);

		va1 = xib_alloc_mem("bram", 512 * 1024, &pa1);
		if (!va1)
			return -ENOMEM;

		printk("pa is %#lx\n", pa1);
		ret = xib_dealloc_mem(512 * 1024, va1, pa1);

		va2 = xib_alloc_mem("bram", 512 * 1024, &pa2);
		if (!va2)
			printk("alloc failed\n");
			return -ENOMEM;

		printk("pa is %#lx\n", pa2);
		ret = xib_dealloc_mem(512 * 1024, va2, pa2);
	}
#endif

	printk("Requested size of the transfer is %d KB\n", size); 
	ctx->mr_size = size * 1024;
	if (!strcmp(sgl_mem, EXT_DDR_STR)) {
		ctx->buf_mem_type = XMEM_EXT_DDR;
		ctx->buf_pa = xib_kmm_alloc_mem(XMEM_EXT_DDR, ctx->mr_size, &ctx->buf_id);
		eddr_va = __va(ctx->buf_pa);
		if (!eddr_va) {
			ib_dealloc_pd(ctx->pd);
			printk("Failed to alloc memory\n");
			return -ENOMEM;
		}
		sg.dma_address = ctx->buf_pa;
		sg_dma_len(&sg) = ctx->mr_size;
		sg.offset = eddr_va;
		ctx->rdma_mr = ib_alloc_mr(ctx->pd,mr_type,1);
		ret = ib_map_mr_sg(ctx->rdma_mr, &sg, 1, NULL,0);	
		printk("data buffer va is %llx\n", eddr_va);
	} else {
		ctx->buf_mem_type = xib_mem_str_to_type(sgl_mem);
		ctx->buf_pa = xib_kmm_alloc_mem(ctx->buf_mem_type, ctx->mr_size, &ctx->buf_id);
		ctx->buf_va = __va(ctx->buf_pa);
		if (!ctx->buf_va) {
			ib_dealloc_pd(ctx->pd);
			printk("Failed to alloc memory\n");
			return -ENOMEM;
		}
		sg.dma_address = ctx->buf_pa;
		sg_dma_len(&sg) = ctx->mr_size;
		sg.offset = ctx->buf_va;
		ctx->rdma_mr = ib_alloc_mr(ctx->pd,mr_type,1);
		ret = ib_map_mr_sg(ctx->rdma_mr, &sg, 1, NULL,0);	
		printk("data buffer va is %#lx\n", ctx->buf_va);
	}

	pa = ctx->buf_pa;
	printk("data buffer pa is %#llx\n", pa);
	if (ctx->rdma_mr == NULL || ret) {
		printk("Failed to register MR\n");
		xib_kmm_free_mem(ctx->buf_mem_type, ctx->buf_id);
		ib_dealloc_pd(ctx->pd);
		return -EFAULT;
	}
	printk("buffer rkey is %#x\n", ctx->rdma_mr->rkey);

	for (; i < 2; i++) {
		ctx->qp_ctx[i].cm_id = ctx->cm_id;
		ctx->qp_ctx[i].pd = ctx->pd;
		ctx->qp_ctx[i].state = IDLE_STATE;
		ctx->qp_ctx[i].qpn = (i + 1);
		ctx->qp_ctx[i].rdma_mr = ctx->rdma_mr;
		ctx->qp_ctx[i].buf_va = ctx->buf_va;
		ctx->qp_ctx[i].cq_valid = 0;
		ctx->qp_ctx[i].connect_valid = 0;
		INIT_WORK(&ctx->qp_ctx[i].cq_work, qp_cq_handler);
	}
	return 0;
}

static int __init perftest_init(void)
{
        struct rdma_cm_id *cm_id;
	int ret;
	struct sockaddr_storage s_addr;

	if (!strcmp(server_ip, "--")) {
		pr_err("Server IP address not provided\n");
		return -EFAULT;
	}

	ret = parse_ip_addr(&s_addr, server_ip);
	if (ret < 0) {
		pr_err("Invalid IP address\n");
		return -EFAULT;
	}

        kpt_test.cm_id = rdma_create_id(&init_net, kperftst_cm_handler, &kpt_test,
                        RDMA_PS_TCP, IB_QPT_RC);

	if (kpt_test.cm_id == NULL) {
		pr_err("Unable to create CM ID\n");
		return -EFAULT;
	}

        ret = rdma_bind_addr(kpt_test.cm_id, (struct sockaddr *)&s_addr);
	if (ret) {
		printk("RDMA Bind failed [%d]\n", ret);
		rdma_destroy_id(kpt_test.cm_id);
		return ret;
	}

	ret = init_test_ctx(&kpt_test);
	if (ret) {
		rdma_destroy_id(kpt_test.cm_id);
		return ret;
	}
	ret = setup_server(&kpt_test, &s_addr);
	if (ret) {
		xib_kmm_free_mem(kpt_test.buf_mem_type, kpt_test.buf_id);
		ib_dealloc_pd(kpt_test.pd);
		rdma_destroy_id(kpt_test.cm_id);
		return ret;
	}
	return 0;
}

static void __exit perftest_exit(void)
{
	int i, ret;

	/* We only have 2 data QPs involved in perftest */
	for (i = 0; i < 2; i++) {
		if (kpt_test.qp_ctx[i].connect_valid == 1) {
			rdma_disconnect(kpt_test.qp_ctx[i].cm_id);

			ret = xib_kmm_free_mem(	kpt_test.qp_ctx[i].sq_mem_type,
						kpt_test.qp_ctx[i].send_buf_id);
			ret = xib_kmm_free_mem(	kpt_test.qp_ctx[i].rx_mem_type,
						kpt_test.qp_ctx[i].rx_buf_id);
			ib_destroy_qp(kpt_test.qp_ctx[i].qp);
			ib_destroy_cq(kpt_test.qp_ctx[i].cq);
			rdma_destroy_id(kpt_test.qp_ctx[i].cm_id);
		}
	}

	ib_dereg_mr(kpt_test.rdma_mr);
	ret = xib_kmm_free_mem(kpt_test.buf_mem_type, kpt_test.buf_id);
	ib_dealloc_pd(kpt_test.pd);
	rdma_destroy_id(kpt_test.cm_id);
}
module_init(perftest_init);
module_exit(perftest_exit);
MODULE_LICENSE("GPL");
