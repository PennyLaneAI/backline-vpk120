/*
 * AMD FPGA ERNIC Infiniband Driver
 *
 * Copyright (C) 2024 AMD, Inc. All rights reserved.
 *
 * Author: Syed S <syeds@xilinx.com>
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */
#include <linux/of_platform.h>
#include <linux/module.h>
#include <rdma/ib_umem.h>
#include <rdma/ib_cache.h>
#include <rdma/ib_pack.h>
#include <rdma/ib_mad.h>
#include <linux/etherdevice.h>
#include <rdma/xib-abi.h>
#include <asm/page.h>
#include "xib.h"
#include "ib_verbs.h"

#define DEBUG

/* #undef dev_dbg */
/* #undef dev_info */
/* #undef dev_warn */
/* #undef dev_notice */

/* #define dev_dbg dev_err */
/* #define dev_info dev_err */
/* #define dev_warn dev_err */
/* #define dev_notice dev_err */
/* In existing code, instead of using the user space buffers
 * for RDMA_READ, the code uses SGL memory configured when create qp is
 * called, and thus the data rx in READ response is being filled in the
 * SGL memory but not in the user space buffer directly. The fix, that's
 * being made here works only for 32Bit machines */
#define XIB_SGL_FIX 1
#define IB_QP_CREATE_HW_OFLD IB_QP_CREATE_RESERVED_START

int xib_bmap_alloc(struct xib_bmap *bmap, u32 max_count, char *name)
{
	unsigned long *bitmap;

	bitmap = kcalloc(BITS_TO_LONGS(max_count), sizeof(long),
			GFP_KERNEL);
	if(!bitmap)
		return -ENOMEM;

	bmap->bitmap = bitmap;
	bmap->max_count = max_count;
	snprintf(bmap->name, XIB_MAX_BMAP_NAME, "%s", name);

	return 0;
}

static int xib_get_user_qp_area_sz(struct xib_qp *qp)
{
	int total_uarea;
	unsigned int size = 0;
	struct xib_cq *sq_cq = qp->sq_cq;

	if (!sq_cq) {
		printk("Invalid CQ association to QP RQ %s:%d\n",
				__func__, __LINE__);
		return -EFAULT;
	}

	total_uarea = qp->sq.max_wr * XRNIC_SQ_WQE_SIZE;
	total_uarea += qp->rq.max_wr * qp->rq_buf_size;
#if 1
	total_uarea += qp->sq.max_wr* CQE_SIZE;
#else
	total_uarea += sq_cq->ib_cq.cqe * CQE_SIZE;
#endif
	total_uarea += qp->rq.max_wr * sizeof(struct xib_imm_inv);
	size = total_uarea;
	size = (total_uarea & ((1 << 12) - 1));
	size = ((1 << 12) - size);
	total_uarea += size;
	total_uarea += qp->rq_buf_size;
	return total_uarea;
}

int xib_bmap_alloc_id(struct xib_bmap *bmap, u32 *id_num)
{
	*id_num = find_first_zero_bit(bmap->bitmap, bmap->max_count);
	if (*id_num >= bmap->max_count) {
		printk("%s PD: 0x%x max 0x%x\n", __func__, *id_num, bmap->max_count);
		return -EINVAL;
	}

	__set_bit(*id_num, bmap->bitmap);

	return 0;
}

void xib_bmap_release_id(struct xib_bmap *bmap, u32 id_num)
{
	bool b_acquired;

	b_acquired = test_and_clear_bit(id_num, bmap->bitmap);
#if 0
	if (!b_acquired) {
		dev_err(NULL, "%s bitmap: id %d already released\n",
				bmap->name, id_num);
		return;
	}
#endif
}


int xib_dealloc_user_qp_buffers(struct ib_device *ibdev, struct xib_qp *qp)
{
	dma_free_coherent(&ibdev->dev,
			xib_get_user_qp_area_sz(qp),
			qp->ua_v, qp->ua_p);
	return 0;
}

/*
 *
 */
void xib_qp_add(struct xilinx_ib_dev *xib, struct xib_qp *qp)
{
	xib->qp_list[qp->hw_qpn] = qp;
}

int xib_dealloc_gsi_qp_buffers(struct ib_device *ibdev, struct xib_qp *qp)
{
	struct xilinx_ib_dev *xib = get_xilinx_dev(qp->ib_qp.device);
	struct xib_rq *rq = &qp->rq;

	/* free sgl */
	xib_free_coherent(xib, XRNIC_SEND_SGL_SIZE,
		qp->send_sgl_v, qp->send_sgl_p);

	xib_dealloc_qp_buffers(ibdev, qp);
	return 0;
}

int xib_dealloc_qp_buffers(struct ib_device *ibdev, struct xib_qp *qp)
{
	struct xib_rq *rq = &qp->rq;
	struct xilinx_ib_dev *xib = get_xilinx_dev(qp->ib_qp.device);

	/* free sq wqe entries */
	xib_free_coherent(xib,
		(qp->sq.max_wr * XRNIC_SQ_WQE_SIZE),
		qp->sq_ba_v, qp->sq_ba_p);

	/* free rq buffers */
	xib_free_coherent(xib,
		(qp->rq.max_wr * qp->rq_buf_size),
		rq->rq_ba_v, rq->rq_ba_p);
	return 0;
}

static int xib_alloc_gsi_qp_buffers(struct ib_device *ibdev, struct xib_qp *qp)
{
	struct xilinx_ib_dev *xib = get_xilinx_dev(ibdev);
	struct xrnic_local *xl = xib->xl;
	struct xib_rq *rq = &qp->rq;
	dma_addr_t db_addr;
	char *from;

	/* TODO 256bit aligned
	 * allocate RQ buffer
	 */
	rq->rq_ba_v = xib_zalloc_coherent(xib_get_rq_mem(), xib,
					(qp->rq.max_wr * qp->rq_buf_size),
					&rq->rq_ba_p, GFP_KERNEL );
	if (!rq->rq_ba_v) {
		dev_err(&ibdev->dev, "failed to alloc rq dma mem\n");
		goto fail_rq;
	}
	dev_dbg(&xib->ib_dev.dev, "%s: rq_ba_v: %px rq_ba_p : %x qpn: %d", __func__, rq->rq_ba_v,
                //printk("%s: rq_ba_v: %px rq_ba_p : %x qpn: %d", __func__, rq->rq_ba_v,
			rq->rq_ba_p, qp->hw_qpn);

	xrnic_iow(xl, XRNIC_RCVQ_BUF_BASE_LSB(qp->hw_qpn), rq->rq_ba_p);
	xrnic_iow(xl, XRNIC_RCVQ_BUF_BASE_MSB(qp->hw_qpn),
				UPPER_32_BITS(rq->rq_ba_p));
	wmb();

#if 0
	/* dont allocate qp1 from bram
	 * instead use pl
	 * if pl not present use ps
	 */
	if (xib_pl_present())
		from = "pl";
	else
		from = "ps";
#endif

	/* 32 bit aligned
	* allocate SQ WQE buffer
	*/
	qp->sq_ba_v = xib_zalloc_coherent(xib_get_sq_mem(), xib,
					(qp->sq.max_wr * XRNIC_SQ_WQE_SIZE),
					&qp->sq_ba_p, GFP_KERNEL );
	if (!qp->sq_ba_v) {
		dev_err(&ibdev->dev, "failed to alloc sq dma mem\n");
		goto fail_sq;
	}
	qp->send_sgl_v = xib_zalloc_coherent(xib_get_sq_mem(), xib,
				XRNIC_SEND_SGL_SIZE,
				&qp->send_sgl_p, GFP_KERNEL);
	if (!qp->send_sgl_v) {
		dev_err(&ibdev->dev, "failed to alloc sgl dma mem\n");
		goto fail_sgl;
	}
	//dev_dbg(&xib->ib_dev.dev, "%s: sq_ba_v: %px sq_ba_p : %x qpn: %d", __func__, qp->sq_ba_v,
	printk("%s: sq_ba_v: %px sq_ba_p : %x qpn: %d", __func__, qp->sq_ba_v,
			qp->sq_ba_p, qp->hw_qpn);

	xrnic_iow(xl, XRNIC_SNDQ_BUF_BASE_LSB(qp->hw_qpn), qp->sq_ba_p);
	xrnic_iow(xl, XRNIC_SNDQ_BUF_BASE_MSB(qp->hw_qpn),
				UPPER_32_BITS(qp->sq_ba_p));
	wmb();

	/* tell hw sq and rq depth */
	xrnic_iow(xl, XRNIC_QUEUE_DEPTH(qp->hw_qpn),
			(qp->sq.max_wr | (qp->rq.max_wr << 16)));
	wmb();

	//dev_dbg(&xib->ib_dev.dev, "send_sgl_p: %lx\n", qp->send_sgl_p);
	printk("%s: send_sgl_v: %px send_sgl_p: %llx\n", __func__, qp->send_sgl_v, qp->send_sgl_p);
	qp->send_sgl_busy = false;

	/* get pre-allocated buffer pointer */
	db_addr = (dma_addr_t)xrnic_get_sq_db_addr(xl, qp->hw_qpn);
	xrnic_iow(xl, XRNIC_CQ_DB_ADDR_LSB(qp->hw_qpn), db_addr);
	xrnic_iow(xl, XRNIC_CQ_DB_ADDR_MSB(qp->hw_qpn), UPPER_32_BITS(db_addr));
	wmb();

	db_addr = (dma_addr_t)xrnic_get_rq_db_addr(xl, qp->hw_qpn);
	xrnic_iow(xl, XRNIC_RCVQ_WP_DB_ADDR_LSB(qp->hw_qpn), db_addr);
	xrnic_iow(xl, XRNIC_RCVQ_WP_DB_ADDR_MSB(qp->hw_qpn),
			UPPER_32_BITS(db_addr));
	wmb();

	return 0;
fail_sgl:
	xib_free_coherent(xib,
			XRNIC_SEND_SGL_SIZE,
			qp->send_sgl_v, qp->send_sgl_p);
fail_sq:
	dma_free_coherent(&ibdev->dev,
			(qp->rq.max_wr * qp->rq_buf_size),
			rq->rq_ba_v, rq->rq_ba_p);
fail_rq:
	return -ENOMEM;
}

/*
 *
 */
int xib_alloc_qp_buffers(struct ib_device *ibdev, struct xib_qp *qp)
{
	struct xilinx_ib_dev *xib = get_xilinx_dev(ibdev);
	struct xrnic_local *xl = xib->xl;
	struct xib_rq *rq = &qp->rq;
	dma_addr_t db_addr;
	char *from;

	/* TODO 256bit aligned
	 * allocate RQ buffer
	 */
	rq->rq_ba_v = xib_zalloc_coherent(xib_get_rq_mem(), xib,
					(qp->rq.max_wr * qp->rq_buf_size),
					&rq->rq_ba_p, GFP_KERNEL);
	if (!rq->rq_ba_v) {
		dev_err(&ibdev->dev, "failed to alloc rq dma mem\n");
		goto fail_rq;
	}
	dev_dbg(&xib->ib_dev.dev, "%s: rq_ba_v: %px rq_ba_p : %x qpn: %d", __func__, rq->rq_ba_v,
			rq->rq_ba_p, qp->hw_qpn);

	xrnic_iow(xl, XRNIC_RCVQ_BUF_BASE_LSB(qp->hw_qpn), rq->rq_ba_p);
	xrnic_iow(xl, XRNIC_RCVQ_BUF_BASE_MSB(qp->hw_qpn),
					UPPER_32_BITS(rq->rq_ba_p));
	wmb();

        /* 32 bit aligned
         * allocate SQ WQE buffer
         */
        if (strcasecmp(xib_get_sq_mem(), "bram") == 0) {
                if (qp->io_qp)
                        from = "bram";
                else
                        from = "pl";
        }
        else
                from = xib_get_sq_mem();

	qp->sq_ba_v = xib_zalloc_coherent(from, xib,
					(qp->sq.max_wr * XRNIC_SQ_WQE_SIZE),
					&qp->sq_ba_p, GFP_KERNEL);
	if (!qp->sq_ba_v) {
		dev_err(&ibdev->dev, "failed to alloc sq dma mem\n");
		goto fail_sq;
	}

	dev_dbg(&xib->ib_dev.dev, "%s: sq_ba_v: %px sq_ba_p : %x qpn: %d", __func__, qp->sq_ba_v,
			qp->sq_ba_p, qp->hw_qpn);

	xrnic_iow(xl, XRNIC_SNDQ_BUF_BASE_LSB(qp->hw_qpn), qp->sq_ba_p);
	xrnic_iow(xl, XRNIC_SNDQ_BUF_BASE_MSB(qp->hw_qpn),
					UPPER_32_BITS(qp->sq_ba_p));
	wmb();

	/* tell hw sq and rq depth */
	xrnic_iow(xl, XRNIC_QUEUE_DEPTH(qp->hw_qpn),
			(qp->sq.max_wr | (qp->rq.max_wr<< 16)));
	wmb();

	/* get pre-allocated buffer pointer */
	db_addr = xrnic_get_sq_db_addr(xl, qp->hw_qpn);
	xrnic_iow(xl, XRNIC_CQ_DB_ADDR_LSB(qp->hw_qpn), db_addr);
	xrnic_iow(xl, XRNIC_CQ_DB_ADDR_MSB(qp->hw_qpn), UPPER_32_BITS(db_addr));
	wmb();

	db_addr = xrnic_get_rq_db_addr(xl, qp->hw_qpn);
	xrnic_iow(xl, XRNIC_RCVQ_WP_DB_ADDR_LSB(qp->hw_qpn), db_addr);
	xrnic_iow(xl, XRNIC_RCVQ_WP_DB_ADDR_MSB(qp->hw_qpn),
				UPPER_32_BITS(db_addr));
	wmb();

	return 0;
fail_sq:
	xib_free_coherent(xib,
			(qp->rq.max_wr * qp->rq_buf_size),
			rq->rq_ba_v, rq->rq_ba_p);
fail_rq:
	return -ENOMEM;
}

int xib_create_kernel_qp(struct ib_qp *kqp,
                                   struct ib_pd *ib_pd,
                                   struct ib_qp_init_attr *init_attr)
{
	struct xilinx_ib_dev *xib = get_xilinx_dev(ib_pd->device);
	struct xrnic_local *xl = xib->xl;
	struct xib_pd *pd = get_xib_pd(ib_pd);
	struct xib_qp *qp = get_xib_qp(kqp);
	int ret = 0;

	dev_dbg(&xib->ib_dev.dev, "%s : <---------- \n", __func__);

	if (!init_attr->cap.max_send_wr) {
		printk("Received SQ depth is 0, setting it to default SQ depth\n");
		return (-EINVAL);
	}

	if (!init_attr->cap.max_recv_wr) {
		printk("Received RQ depth is 0, setting it to default RQ depth\n");
		return (-EINVAL);
	}

	/* qp = kzalloc(sizeof(*qp), GFP_KERNEL); */
	/* if (!qp) */
	/* 	return ERR_PTR(-ENOMEM); */

	spin_lock_bh(&xib->lock);
	ret = xib_bmap_alloc_id(&xib->qp_map, &qp->hw_qpn);
	spin_unlock_bh(&xib->lock);

	if (ret < 0) {
		pr_err("%s: QPs are not available\n", __func__);
		return (-EFAULT);
	}

	if (init_attr->create_flags & IB_QP_CREATE_HW_OFLD) {
		printk("xib_create_kernel_qp: io_qp\n");
		qp->io_qp = true;
	}

	qp->sq.max_wr = init_attr->cap.max_send_wr;
	qp->rq.max_wr = init_attr->cap.max_recv_wr;

	/*TODO: To support existing NVMeoF applications */
	if (init_attr->cap.max_recv_sge < 2)
		init_attr->cap.max_recv_sge = 2;

	qp->rq_buf_size = init_attr->cap.max_recv_sge * XRNIC_RQ_BUF_SGE_SIZE;

	dev_dbg(&xib->ib_dev.dev, "%s : qpn: %d \n", __func__, qp->hw_qpn);
        if (init_attr->create_flags & IB_QP_CREATE_HW_OFLD) {
                printk("xib_create_kernel_qp: io_qp\n");
                qp->io_qp = true;
        }

	ret = xib_alloc_qp_buffers(ib_pd->device, qp);
	if (ret < 0) {
		dev_err(&xib->ib_dev.dev, "%s: qp buf alloc fail\n", __func__);
		xib_bmap_release_id(&xib->qp_map, qp->hw_qpn);
		kfree(qp);
		return (-ENOMEM);
	}

	qp->ib_qp.qp_num = qp->hw_qpn + 1;
	qp->qp_type	 = XIB_QP_TYPE_KERNEL;
	qp->is_ipv6 = false;

	qp->rq.rq_wrptr_db_local = 0;
	qp->rq.rq_ci_db_local = 0;

	qp->sq.sq_cmpl_db_local = 0;
	qp->sq.send_cq_db_local = 0;

	/* assign the pd to qp */
	xrnic_qp_set_pd(xib, qp->hw_qpn, pd->pdn);

	qp->rq.rqe_list = kcalloc(qp->rq.max_wr, sizeof(struct xib_rqe),
				GFP_KERNEL);
	if (!qp->rq.rqe_list)
		goto fail_1;

	/* xrnic only supports 16bit wrid
	 * array to store 64bit wrid from the stack
	 */
	qp->sq.wr_id_array = kcalloc(qp->sq.max_wr, sizeof(u64), GFP_KERNEL);

	qp->sq.pl_buf_list = (struct xib_pl_buf *) kcalloc(qp->sq.max_wr,
					sizeof(struct xib_pl_buf),
					GFP_KERNEL);

	qp->sq_cq = get_xib_cq(init_attr->send_cq);
	qp->sq_cq->cq_type = XIB_CQ_TYPE_KERNEL;
	if(qp->sq_cq->qp == NULL)
		qp->sq_cq->qp = qp;
	else if(qp->sq_cq->qp != qp)
		dev_err(&xib->ib_dev.dev,
			"%s: cq already associated with some other qp\n", __func__);

	/*TODO: ERNIC doesn't have a configurable CQ depth. It assumes CQ depth is same
		as SQ depth. Its not a problem if the CQ depth is more than SQ depth, because
		create CQ allocates more buffer and it uses a depth = SQ DEPTH*/
	if (qp->sq_cq->ib_cq.cqe < qp->sq.max_wr) {
		dev_err(&xib->ib_dev.dev, "ERNIC CQ depth and SQ depth must be equal\n");
		goto fail_1;
	}

	/* program the send cq base */
	xrnic_iow(xl, XRNIC_CQ_BUF_BASE_LSB(qp->hw_qpn), qp->sq_cq->buf_p);
	xrnic_iow(xl, XRNIC_CQ_BUF_BASE_MSB(qp->hw_qpn),
					UPPER_32_BITS(qp->sq_cq->buf_p));
	wmb();

	qp->rq_cq = get_xib_cq(init_attr->recv_cq);
	qp->rq_cq->cq_type = XIB_CQ_TYPE_KERNEL;
	if(qp->rq_cq->qp == NULL)
		qp->rq_cq->qp = qp;
	else if(qp->rq_cq->qp != qp)
		dev_err(&xib->ib_dev.dev,
			"%s: cq already associated with some other qp\n", __func__);

	qp->imm_inv_data = kcalloc(qp->rq.max_wr, sizeof(struct xib_imm_inv),
				GFP_KERNEL);
	if (!qp->imm_inv_data)
		goto fail_2;

	tasklet_init(&qp->fatal_hdlr_task, xib_fatal_handler,
			(unsigned long)qp);
	tasklet_init(&qp->comp_task, xib_comp_handler,
			(unsigned long)qp);
	tasklet_init(&qp->cnp_task, xib_cnp_handler,
			(unsigned long)qp);

	spin_lock_init(&qp->sq_lock);
	spin_lock_init(&qp->rq_lock);

	qp->state = XIB_QP_STATE_RESET;

	init_completion(&qp->sq_drained);
	xib_qp_add(xib, qp);

	//return &qp->ib_qp;
        return(ret);
fail_2:
	kfree(qp->rq.rqe_list);
fail_1:
	//kfree(qp); this is handled by rdma core
	return (-ENOMEM);
}

dma_addr_t get_phys_addr(uint64_t va)
{
	return va;
}

int xib_create_user_qp(struct ib_qp *kqp,
                                 struct ib_pd *ib_pd,
                                 struct ib_qp_init_attr *init_attr,
                                 struct ib_udata *udata)
{
	struct xilinx_ib_dev *xib = get_xilinx_dev(ib_pd->device);
	struct xrnic_local *xl = xib->xl;
	struct xib_qp *qp = get_xib_qp(kqp);
	struct xib_pd *pd = get_xib_pd(ib_pd);
	struct xib_ib_create_qp_resp uresp;
	struct xib_ib_create_qp req;
	dma_addr_t db_addr;
	int ret = 0;
	size_t min_len;

	/* qp = kzalloc(sizeof(*qp), GFP_KERNEL); */
	/* if (!qp) */
	/* 	return ERR_PTR(-ENOMEM); */

	spin_lock_bh(&xib->lock);
	ret = xib_bmap_alloc_id(&xib->qp_map, &qp->hw_qpn);
	spin_unlock_bh(&xib->lock);

	if (ret < 0) {
		pr_err("%s : QPs are not available\n", __func__);
		return (-EFAULT);
	}

	ret = ib_copy_from_udata((void *)&req, udata, sizeof(req));

	dev_dbg(&xib->ib_dev.dev, "%s : qpn: %d \n", __func__, qp->hw_qpn);
	qp->is_ipv6 = false;
	qp->sq.max_wr = init_attr->cap.max_send_wr;
	qp->rq.max_wr = init_attr->cap.max_recv_wr;
	qp->rq_buf_size = init_attr->cap.max_recv_sge * XRNIC_RQ_BUF_SGE_SIZE;

	qp->sq_cq = get_xib_cq(init_attr->send_cq);
	qp->sq_cq->cq_type = XIB_CQ_TYPE_USER;

	qp->rq.rq_ba_p = get_phys_addr(req.rq_ba);
	xrnic_iow(xl, XRNIC_RCVQ_BUF_BASE_LSB(qp->hw_qpn), qp->rq.rq_ba_p);
	xrnic_iow(xl, XRNIC_RCVQ_BUF_BASE_MSB(qp->hw_qpn),
					UPPER_32_BITS(qp->rq.rq_ba_p));

	qp->sq_ba_p = get_phys_addr(req.sq_ba);
	xrnic_iow(xl, XRNIC_SNDQ_BUF_BASE_LSB(qp->hw_qpn), qp->sq_ba_p);
	xrnic_iow(xl, XRNIC_SNDQ_BUF_BASE_MSB(qp->hw_qpn),
					UPPER_32_BITS(qp->sq_ba_p));

	xrnic_iow(xl, XRNIC_QUEUE_DEPTH(qp->hw_qpn),
			(qp->sq.max_wr | (qp->rq.max_wr << 16)));

	/* get imm base */
	db_addr = xrnic_get_sq_db_addr(xl, qp->hw_qpn);
	xrnic_iow(xl, XRNIC_CQ_DB_ADDR_LSB(qp->hw_qpn), db_addr);
	xrnic_iow(xl, XRNIC_CQ_DB_ADDR_MSB(qp->hw_qpn), UPPER_32_BITS(db_addr));
	wmb();

	db_addr = xrnic_get_rq_db_addr(xl, qp->hw_qpn);
	xrnic_iow(xl, XRNIC_RCVQ_WP_DB_ADDR_LSB(qp->hw_qpn), db_addr);
	xrnic_iow(xl, XRNIC_RCVQ_WP_DB_ADDR_MSB(qp->hw_qpn),
					UPPER_32_BITS(db_addr));
	wmb();

	qp->rq.rq_wrptr_db_local = 0;
	qp->rq.rq_ci_db_local = 0;

	qp->sq.sq_cmpl_db_local = 0;
	qp->sq.send_cq_db_local = 0;

	/* assign the pd to qp */
	xrnic_qp_set_pd(xib, qp->hw_qpn, pd->pdn);

	/* there is no QP0 in ernic
	 * so QP1 index starts with 0
	 */
	qp->ib_qp.qp_num = qp->hw_qpn + 1;
	qp->qp_type	 = XIB_QP_TYPE_USER;

	/* program the send cq base */
	qp->sq_cq->buf_p = get_phys_addr(req.cq_ba);
	xrnic_iow(xl, XRNIC_CQ_BUF_BASE_LSB(qp->hw_qpn), qp->sq_cq->buf_p);
	xrnic_iow(xl, XRNIC_CQ_BUF_BASE_MSB(qp->hw_qpn),
				UPPER_32_BITS(qp->sq_cq->buf_p));
	wmb();

	qp->imm_inv_data = __va(req.imm_inv_ba);
	uresp.qpn = qp->hw_qpn;
	min_len = min_t(size_t, sizeof(struct xib_ib_create_qp_resp), udata->outlen);
	ib_copy_to_udata(udata, &uresp, min_len);

	qp->state = XIB_QP_STATE_RESET;
	xib_qp_add(xib, qp);
	tasklet_init(&qp->fatal_hdlr_task, xib_fatal_handler,
				(unsigned long)qp);
	tasklet_init(&qp->cnp_task, xib_cnp_handler,
			(unsigned long)qp);
	//return &qp->ib_qp;
        return ret;
}

/*
 *
 */
int xib_gsi_create_qp(struct ib_qp *ib_qp,
                      struct ib_pd *pd,
                      struct ib_qp_init_attr *init_attr)
{
    //struct ib_pd *pd = ib_qp->pd;
	struct xilinx_ib_dev *xib = get_xilinx_dev(pd->device);
	struct xrnic_local *xl = xib->xl;
 	struct xib_qp *qp;
	int ret;

	dev_dbg(&xib->ib_dev.dev, " %s <----------- \n", __func__);

	dev_dbg(&xib->ib_dev.dev, "%s: send_cq %px recv_cq: %px \n", __func__, init_attr->send_cq,
			init_attr->recv_cq);

	/* qp = kzalloc(sizeof(*qp), GFP_KERNEL); */
	/* if (!qp) */
	/* 	return (-ENOMEM); */

        //qp->ib_qp = kqp;
        qp = get_xib_qp(ib_qp);
	qp->hw_qpn = 0; /* QP1 */
	qp->qp_type = XIB_QP_TYPE_GSI;
	qp->rq.rq_wrptr_db_local = 0;
	qp->rq.rq_ci_db_local = 0;

	qp->sq.sq_cmpl_db_local = 0;
	qp->sq.send_cq_db_local = 0;

	#if 1
	qp->rq.max_wr = init_attr->cap.max_recv_wr;
	qp->sq.max_wr = init_attr->cap.max_send_wr;
	#else
	qp->rq.max_wr = XRNIC_GSI_RQ_DEPTH;
	qp->sq.max_wr = XRNIC_GSI_SQ_DEPTH;
	#endif
	qp->is_ipv6 = false;
	qp->rq_buf_size = XRNIC_GSI_RECV_PKT_SIZE;

	qp->rq.rqe_list = kcalloc(qp->rq.max_wr, sizeof(struct xib_rqe),
				GFP_KERNEL);
	if (!qp->rq.rqe_list)
		goto fail_1;

	/* xrnic only supports 16bit wrid
	 * array to store 64bit wrid from the stack
	 */
	qp->sq.wr_id_array = kcalloc(qp->sq.max_wr, sizeof(*qp->sq.wr_id_array), GFP_KERNEL);

	ret = xib_alloc_gsi_qp_buffers(pd->device, qp);
	if (ret < 0) {
		dev_err(&xib->ib_dev.dev, "%s: qp buf alloc fail\n", __func__);
		goto fail_2;
	}

	qp->sq_cq = get_xib_cq(init_attr->send_cq);
	qp->sq_cq->cq_type = XIB_CQ_TYPE_GSI;
        qp->sq_cq->qp = qp;

	/* program the send cq base */
	xrnic_iow(xl, XRNIC_CQ_BUF_BASE_LSB(qp->hw_qpn), qp->sq_cq->buf_p);
	xrnic_iow(xl, XRNIC_CQ_BUF_BASE_MSB(qp->hw_qpn),
					UPPER_32_BITS(qp->sq_cq->buf_p));
	wmb();

	qp->rq_cq = get_xib_cq(init_attr->recv_cq);
	qp->rq_cq->cq_type = XIB_CQ_TYPE_GSI;
        qp->rq_cq->qp = qp;

	qp->ib_qp.qp_num = 1;

	xib->gsi_qp = qp;

	tasklet_init(&qp->comp_task, xib_gsi_comp_handler,
			(unsigned long)xib);

	spin_lock_init(&qp->sq_lock);
	spin_lock_init(&qp->rq_lock);

	qp->state = XIB_QP_STATE_RESET;

	qp->imm_inv_data = kcalloc(qp->rq.max_wr, sizeof(struct xib_imm_inv),
				GFP_KERNEL);
	if (!qp->imm_inv_data)
		goto fail_2;

	xib_qp_add(xib, qp);
	tasklet_init(&qp->cnp_task, xib_cnp_handler,
			(unsigned long)qp);

	//return &qp->ib_qp;
        return 0;
fail_2:
	kfree(qp->rq.rqe_list);
fail_1:
	//kfree(qp);
	return -ENOMEM;

}

struct ib_mr *xib_reg_user_mr(struct ib_pd *ib_pd, u64 start, u64 length,
				   u64 virt_addr, int access_flags,
				   struct ib_udata *udata)
{
	struct xib_mr *mr = NULL;
	struct xib_pd *pd = get_xib_pd(ib_pd);
	struct xilinx_ib_dev *xib = get_xilinx_dev(ib_pd->device);
	struct ib_umem *umem;
	u64 *pbl_tbl, *pbl_tbl_orig, phy_addr;
	int i, umem_pgs, pages;
	struct scatterlist *sg;
	int entry, ret = 0;
	u32 mr_idx;
	u8 rkey;
	struct page **page_list;
	char *k_va;

	/* printk("xib_reg_user_mr: user mr pd = %d start = %llx, len = %lld," */
	/* 		"usr_addr = %llx, acc = %d\n", */
	/* 	 pd->pdn, start, length, virt_addr, access_flags); */
	dev_dbg(&xib->ib_dev.dev, "xib_reg_user_mr: user mr pd = %d start = %llx, len = %lld,"
			"usr_addr = %llx, acc = %d\n",
		 pd->pdn, start, length, virt_addr, access_flags);

	mr = kzalloc(sizeof(*mr), GFP_KERNEL);
	if (!mr)
		return ERR_PTR(-ENOMEM);

#ifndef CONFIG_MICROBLAZE
	page_list = (struct page **) __get_free_page(GFP_KERNEL);
	if (!page_list) {
		printk("Unable to allocate free page");
		return ERR_PTR(-ENOMEM);
	}

	down_read(&current->mm->mmap_lock);
	ret = get_user_pages(start, 1, FOLL_WRITE,
					page_list);
	if (ret < 0) {
		up_read(&current->mm->mmap_lock);
		goto fail;
	}

	up_read(&current->mm->mmap_lock);
	k_va = kmap(page_list[0]);
	phy_addr = virt_to_phys(k_va);
	pbl_tbl_orig = &phy_addr;
#else
	phy_addr = start;
	pbl_tbl_orig = &phy_addr;
#endif

	spin_lock_bh(&xib->lock);
	ret = xib_bmap_alloc_id(&xib->mr_map, &mr_idx);
	spin_unlock_bh(&xib->lock);
	if (ret < 0)
		goto fail;

	dev_dbg(&xib->ib_dev.dev, "umem_pgs: %d pbl_tbl: %px pdn: %d, mr_idx: %d\n", umem_pgs, pbl_tbl,
			pd->pdn, mr_idx);
	get_random_bytes(&rkey, sizeof(rkey));

	/* Lkey must be the chunk ID of the reg mr */
	mr->ib_mr.lkey = (mr_idx << 8) | rkey;
	mr->ib_mr.rkey = (mr_idx << 8) | rkey;
	mr->ib_mr.device = ib_pd->device;

	mr->mr_idx = mr_idx;
	mr->rkey = rkey;
	mr->type = XIB_MR_USER;

	xrnic_reg_mr(xib, virt_addr, length, pbl_tbl_orig, 1 /*umem_pgs*/, pd->pdn, mr_idx, rkey);

	free_page((unsigned long)page_list);
	return &mr->ib_mr;
fail:
	free_page((unsigned long)page_list);
	kfree(mr);
	return ERR_PTR(ret);
}

/*
 *
 */

int xib_build_qp1_send_v2(struct ib_qp *ib_qp,
			const struct ib_send_wr *wr,
			int payload_sz,
			bool *is_udp,
			u8 *ip_version)
{
	struct xilinx_ib_dev *xib = get_xilinx_dev(ib_qp->device);
	struct xib_qp *qp = get_xib_qp(ib_qp);
	struct rdma_ah_init_attr *ah_init_attr = &get_xib_ah(ud_wr(wr)->ah)->attr;
        if (!virt_addr_valid(ah_init_attr))
        {
            dev_err(&xib->ib_dev.dev, "%s: ah_init_attr is not valid virtual address\n", __func__);
            return -1;
        }
        if (!(ah_init_attr->ah_attr->ah_flags & IB_AH_GRH))
        {
            dev_err(&xib->ib_dev.dev, "%s: ah_flags does not have IB_AH_GRH set\n", __func__);
            return -1;
        }

	const struct ib_global_route *grh = rdma_ah_read_grh(ah_init_attr->ah_attr);
        if (!virt_addr_valid(grh))
        {
            dev_dbg(&xib->ib_dev.dev, "%s: grh is not valid virtual address\n", __func__);
            return -1;
        }
	const struct ib_gid_attr *sgid_attr = grh->sgid_attr;
	u16 ether_type;
	bool is_eth = true;
	bool is_grh = false;
	int ret;

	//printk("%s <---------- \n", __func__);

/* TODO : Getting sgid_attr as NULL for the client-Connect Request if the
 *        server is not initiated in ERNIC. This occurs in ARM architect,
 *        works fine in microblaze. This NULL check is the work around.
 */
	if (!sgid_attr) {
		dev_err(&xib->ib_dev.dev, "%s: sgid_attr is NULL\n", __func__);
		return -1;
	}

        if (!virt_addr_valid(sgid_attr))
        {
            dev_err(&xib->ib_dev.dev, "%s: sgid_attr is not valid virtual address\n", __func__);
            return -1;
        }

	*is_udp = sgid_attr->gid_type == IB_GID_TYPE_ROCE_UDP_ENCAP;

	if (*is_udp) {
		if (ipv6_addr_v4mapped((struct in6_addr *)&sgid_attr->gid)) {
			*ip_version = 4;
			ether_type = ETH_P_IP;
			is_grh = false;
		} else {
			*ip_version = 6;
			ether_type = ETH_P_IPV6;
			is_grh = true;
		}
	} else {
            dev_err(&xib->ib_dev.dev, "not udp!! %d\n", sgid_attr->gid_type);
            ether_type = ETH_P_IBOE;
            is_grh = true;
            return -1;
	}

	ib_ud_header_init(payload_sz, !is_eth, is_eth, false, is_grh,
			*ip_version, *is_udp, 0, &qp->qp1_hdr);

	/* ETH */
	ether_addr_copy(qp->qp1_hdr.eth.dmac_h, ah_init_attr->ah_attr->roce.dmac);
	ether_addr_copy(qp->qp1_hdr.eth.smac_h, xib->netdev->dev_addr);

	qp->qp1_hdr.eth.type = cpu_to_be16(ether_type);

	if (is_grh || (*ip_version == 6)) {
			/* copy the GIDs / IPV6 addresses */
		qp->qp1_hdr.grh.hop_limit = 64;
		qp->qp1_hdr.grh.traffic_class = 0;
		qp->qp1_hdr.grh.flow_label= 0;
		memcpy(&qp->qp1_hdr.grh.source_gid, sgid_attr->gid.raw, 16);
		memcpy(&qp->qp1_hdr.grh.destination_gid, grh->dgid.raw, 16);
	}

	if (*ip_version == 4) {
		qp->qp1_hdr.ip4.tos = 0;
		qp->qp1_hdr.ip4.id = 0;
		qp->qp1_hdr.ip4.frag_off = htons(IP_DF);
		/* qp->qp1_hdr.ip4.ttl = grh->hop_limit; */
		/* TODO check why its coming back as zero */
		qp->qp1_hdr.ip4.ttl = 64;

		memcpy(&qp->qp1_hdr.ip4.saddr, sgid_attr->gid.raw + 12, 4);
		memcpy(&qp->qp1_hdr.ip4.daddr, grh->dgid.raw + 12, 4);
		qp->qp1_hdr.ip4.check = ib_ud_ip4_csum(&qp->qp1_hdr);
	}

	if (*is_udp) {
		qp->qp1_hdr.udp.dport = htons(ROCE_V2_UDP_DPORT);
		qp->qp1_hdr.udp.sport = htons(0x8CD1);
		qp->qp1_hdr.udp.csum = 0;
	}

	/* BTH */
	if (wr->opcode == IB_WR_SEND_WITH_IMM) {
		qp->qp1_hdr.bth.opcode = IB_OPCODE_UD_SEND_ONLY_WITH_IMMEDIATE;
		qp->qp1_hdr.immediate_present = 1;
	} else {
		qp->qp1_hdr.bth.opcode = IB_OPCODE_UD_SEND_ONLY;
	}

	if (wr->send_flags & IB_SEND_SOLICITED)
		qp->qp1_hdr.bth.solicited_event = 1;

	/* pad_count */
	qp->qp1_hdr.bth.pad_count = (4 - payload_sz) & 3;

	/* P_key for QP1 is for all members */
	qp->qp1_hdr.bth.pkey = cpu_to_be16(0xFFFF);
	qp->qp1_hdr.bth.destination_qpn = IB_QP1;
	qp->qp1_hdr.bth.ack_req = 0;
	qp->send_psn++;
	qp->qp1_hdr.bth.psn = cpu_to_be32(qp->send_psn);
	/* DETH */
	/* Use the priviledged Q_Key for QP1 */
	qp->qp1_hdr.deth.qkey = cpu_to_be32(IB_QP1_QKEY);
	qp->qp1_hdr.deth.source_qpn = IB_QP1;

	return 0;
fail:
	return ret;
}

/*
 *
 */
int xib_kernel_qp_post_recv(struct ib_qp *ibqp, const struct ib_recv_wr *wr,
			const struct ib_recv_wr **bad_wr)
{
	struct xib_qp *qp = get_xib_qp(ibqp);
	struct xib_rqe *rqe;
	unsigned long flags;
	int i,ret, data_len;

	spin_lock_irqsave(&qp->rq_lock, flags);

	while (wr) {
		rqe = &qp->rq.rqe_list[qp->rq.prod];
		memset(rqe, 0, sizeof(struct xib_rqe));
		data_len = xib_get_payload_size(wr->sg_list, wr->num_sge);
		if ((data_len > qp->rq_buf_size) ||
			 (wr->num_sge > XIB_MAX_RQE_SGE)) {
			ret = -ENOMEM;
			pr_err("%s: no mem, wr size: %d, num_sge: %d\n",
				__func__, data_len, wr->num_sge);
			goto fail;
		}

		//if (wr->num_sge > 1) /* suppress this warning */
		for (i = 0; i < wr->num_sge; i++)
			rqe->sg_list[i] = wr->sg_list[i];
		rqe->wr_id = wr->wr_id;
		rqe->num_sge = wr->num_sge;
		xib_rq_prod_inc(&qp->rq);

		wr = wr->next;
	}

	spin_unlock_irqrestore(&qp->rq_lock, flags);

	return 0;
fail:
	spin_unlock_irqrestore(&qp->rq_lock, flags);
	*bad_wr = wr;
	return ret;
}

int xib_get_rq_recd(struct xib_rq *rq, u32 rq_wr_current)
{
	int received;

	if (rq_wr_current == rq->rq_wrptr_db_local) {
		return 0;
	}

	if (rq->rq_wrptr_db_local > rq_wr_current) {
		received = (rq_wr_current + rq->max_wr) -
				rq->rq_wrptr_db_local;
	} else {
		received = rq_wr_current - rq->rq_wrptr_db_local;
	}

	return received;
}


/* check for RQ */
int xib_pop_rq(struct xilinx_ib_dev *xib, struct xib_qp *qp, int qpn)
{
	int i = 0, rq_pkt_num, loop, len=0;
	int received = 0, rq_wr_current;
	struct xrnic_local *xl = xib->xl;
	struct xib_rq *rq = &qp->rq;
	u8 *buf;
	void *sgl_va;
	struct xib_rqe *rqe;
	volatile u32 *rq_db_addr;

	#if 0
	rq_db_addr = (volatile u32 *)
			phys_to_virt(xrnic_ior(xib->xl, XRNIC_RCVQ_WP_DB_ADDR(qpn)));
	dev_dbg(&xib->ib_dev.dev, "rq_db_addr: %lx\n", rq_db_addr);

	rq_wr_current = *rq_db_addr;
	#else
	rq_db_addr = (volatile u32 *)(xl->qp1_rq_db_v + qpn);
	rq_wr_current = *rq_db_addr;
	#endif

	dev_dbg(&xib->ib_dev.dev, "rq_wr_current: %d, rq_wrptr_db_local: %d\n", rq_wr_current,
			rq->rq_wrptr_db_local);

	received = xib_get_rq_recd(rq, rq_wr_current);

	dev_dbg(&xib->ib_dev.dev, "received %d frames on RQ\n", received);

	for (loop = 0; loop < received; loop++) {
		if(rq->rq_wrptr_db_local == rq->max_wr)
			rq->rq_wrptr_db_local = 0;
		rq_pkt_num = rq->rq_wrptr_db_local;
		if (rq_pkt_num >= rq->max_wr) {
			rq_pkt_num = rq_pkt_num - rq->max_wr;
		}
		buf = (u8 *)(rq->rq_ba_v + (rq_pkt_num * qp->rq_buf_size));

		/* pop out the wqe */
		rqe = &rq->rqe_list[rq->gsi_cons];
		/* we check if wr total len <= XRNIC_RECV_PKT_SIZE
		 * at post_recv
		 */
		for (i = 0; i < rqe->num_sge; i++) {
			sgl_va = (void *)phys_to_virt
				((unsigned long)(rqe->sg_list[i].addr));
			memcpy(sgl_va, buf, rqe->sg_list[i].length);
			buf = buf + rqe->sg_list[i].length;
			len += rqe->sg_list[i].length;
		}
		xib_inc_sw_gsi_cons(rq);
		rq->rq_wrptr_db_local++;

		/* tell hw we consumed */
		xrnic_iow(xib->xl, XRNIC_RQ_CONS_IDX(qpn),
				rq->rq_wrptr_db_local);
		wmb();
	}
	return received;
}


/*
 *
 */
int xib_poll_kernel_cq(struct ib_cq *ibcq, int num_entries, struct ib_wc *wc)
{
	struct xilinx_ib_dev *xib = get_xilinx_dev(ibcq->device);
	struct xib_cq *cq = get_xib_cq(ibcq);
	struct xib_qp *qp = cq->qp;
	struct xib_rq *rq = &qp->rq;
	struct xib_sq *sq = &qp->sq;
        struct xib_sqd *temp, *head;
	struct xrnic_cqe *cqe;
	struct xib_rqe *rqe;
	u32 cur_send_cq_head, err_flag;
	unsigned long flags;
	u32 opcode, wc_opc;
	int i = 0, received;
	struct device *alloc_dev;
	struct xib_pl_buf *plb;

	spin_lock_irqsave(&cq->cq_lock, flags);

	dev_dbg(&xib->ib_dev.dev, "xib_poll_kernel_cq\n" );

	received = xib_pop_rq(xib, qp, qp->hw_qpn);

	while (i < num_entries && qp->rq.cons != qp->rq.gsi_cons) {
		memset(&wc[i], 0, sizeof(*wc));

		rqe = &rq->rqe_list[rq->cons];

		wc[i].qp = &qp->ib_qp;
		wc[i].wr_id = rqe->wr_id;
		wc[i].opcode = IB_WC_RECV;
		wc[i].byte_len = rqe->sg_list[0].length;
		wc[i].status = IB_WC_SUCCESS;
		if (qp->imm_inv_data[rq->cons].isvalid) {
			if (qp->imm_inv_data[rq->cons].type == SEND_INVALIDATE) {
				wc[i].wc_flags |= IB_WC_WITH_INVALIDATE;
				wc[i].ex.invalidate_rkey = qp->imm_inv_data[rq->cons].data;
			} else if (qp->imm_inv_data[rq->cons].type == SEND_IMMEDIATE) {
				wc[i].wc_flags |= IB_WC_WITH_IMM;
				wc[i].ex.imm_data = qp->imm_inv_data[rq->cons].data;
			}
			qp->imm_inv_data[rq->cons].isvalid = false;
		}
		xib_rq_cons_inc(rq);
		i++;
	}

	/* read current send cq db head */
	cur_send_cq_head = xrnic_ior(xib->xl, XRNIC_CQ_HEAD_PTR(qp->hw_qpn)) &
				XRNIC_CQ_HEAD_PTR_MASK;

	dev_dbg(&xib->ib_dev.dev, "cur_send_cq_head: %d, send_cq_db_local: %d, num_entries: %d\n",
			cur_send_cq_head, sq->send_cq_db_local, num_entries);

	/* send queue completions */
	while (i < num_entries && sq->send_cq_db_local != cur_send_cq_head) {
		if (sq->send_cq_db_local == qp->sq.max_wr)
			sq->send_cq_db_local = 0;
		cqe = cq->buf_v + (sq->send_cq_db_local * sizeof(struct
					xrnic_cqe));

		opcode = (cqe->entry & XRNIC_CQE_OPCODE_MASK) >>
			XRNIC_CQE_OPCODE_SHIFT;

		switch (opcode) {
			case XRNIC_SEND_WITH_IMM:
			case XRNIC_SEND_WITH_INV:
			case XRNIC_SEND_ONLY:
				plb = &sq->pl_buf_list[qp->sq.send_cq_db_local];
				#ifdef CONFIG_MICROBLAZE
				/* for microblaze cant free buffer with irqs
				 * disabled
				 */
				spin_unlock_irqrestore(&cq->cq_lock, flags);
				#endif
				xib_free_coherent(xib,
						plb->len,
						plb->va,
						plb->pa);
				#ifdef CONFIG_MICROBLAZE
				spin_lock_irqsave(&cq->cq_lock, flags);
				#endif
				wc_opc = IB_WC_SEND;
				if (opcode == XRNIC_SEND_WITH_IMM)
					wc[i].wc_flags |= IB_WC_WITH_IMM;
				else if (opcode == XRNIC_SEND_WITH_INV)
					wc[i].wc_flags |= IB_WC_WITH_INVALIDATE;
			break;
			case XRNIC_RDMA_READ:
				#ifdef ARCH_HAS_PS
				if (strcasecmp(xib_get_sq_mem(), "ps") != 0) {
					void *sgl_va;
					plb = &sq->pl_buf_list[qp->sq.send_cq_db_local];
					/* copy from temp rdma buf to stack
					 * provided buf
					 */
					sgl_va = (void *)phys_to_virt((unsigned long) plb->sgl_addr);
					memcpy(sgl_va, plb->va, plb->len);
					xib_free_coherent(xib,
							plb->len,
							plb->va,
							plb->pa);
				}
				#endif
				wc_opc = IB_WC_RDMA_READ;
			break;
			case XRNIC_RDMA_WRITE_WITH_IMM:
			case XRNIC_RDMA_WRITE:
				#ifdef ARCH_HAS_PS
				if (strcasecmp(xib_get_sq_mem(), "ps") != 0) {
					plb = &sq->pl_buf_list[qp->sq.send_cq_db_local];
					xib_free_coherent(xib,
							plb->len,
							plb->va,
							plb->pa);
				}
				#endif
				wc_opc = IB_WC_RDMA_WRITE;
				if (opcode == XRNIC_RDMA_WRITE_WITH_IMM)
					wc[i].wc_flags |= IB_WC_WITH_IMM;
			break;
			default:
				dev_err(&xib->ib_dev.dev, "%s: err in cqe\n", __func__);
			break;
		}

		err_flag = (cqe->entry & XRNIC_CQE_ERR_MASK) >>
				XRNIC_CQE_ERR_SHIFT;
		if (err_flag) {
			dev_err(&xib->ib_dev.dev, "%s: err in cqe\n", __func__);
		}

		if (!sq->wr_id_array[sq->send_cq_db_local].signaled)
			goto skip_cqe;

		wc[i].opcode = wc_opc;
		wc[i].qp = &qp->ib_qp;
		wc[i].wr_id = sq->wr_id_array[sq->send_cq_db_local].wr_id;
		qp->sq_polled_count++;
		if (qp->state > XIB_QP_STATE_RTS) {
			if (qp->post_send_count == qp->sq_polled_count)
				complete(&qp->sq_drained);
		}

		if (!err_flag)
			wc[i].status = IB_WC_SUCCESS;
		else
			wc[i].status = IB_WC_FATAL_ERR;
		i++;
	skip_cqe:
		sq->send_cq_db_local++;
	}
	temp = sq->sqd_wr_list;
        while (i < num_entries && temp) {
        	wc[i].wr_id = temp->wr_id;
                wc[i].status = IB_WC_WR_FLUSH_ERR;
                i++;
		head = temp;
		temp = temp->next;
		kfree(head);
       		sq->sqd_length--;
        }
	sq->sqd_wr_list = temp;

	spin_unlock_irqrestore(&cq->cq_lock, flags);

	return i;
}

/*
 *
 */
int xib_gsi_poll_cq(struct ib_cq *ibcq, int num_entries, struct ib_wc *wc)
{
	struct xilinx_ib_dev *xib = get_xilinx_dev(ibcq->device);
	struct xrnic_local *xl = xib->xl;
	struct xib_cq *cq = get_xib_cq(ibcq);
	struct xib_qp *qp = xib->gsi_qp;
	struct xib_rq *rq = &qp->rq;
	struct xib_sq *sq = &qp->sq;
        struct xib_sqd *temp, *head;
	struct xrnic_cqe *cqe;
	struct xib_rqe *rqe;
	u32 cur_send_cq_head, err_flag;
	unsigned long flags;
	u32 opcode;
	int i = 0;

	spin_lock_irqsave(&cq->cq_lock, flags);

	/* receive queue completions */
	while (i < num_entries && qp->rq.cons != qp->rq.gsi_cons ) {
		memset(&wc[i], 0, sizeof(*wc));

		rqe = &rq->rqe_list[rq->cons];

		wc[i].qp = &qp->ib_qp;
		wc[i].wr_id = rqe->wr_id;
		wc[i].opcode = IB_WC_RECV;
		wc[i].pkey_index = 0;
		wc[i].byte_len = rqe->sg_list[0].length;
		wc[i].status = IB_WC_SUCCESS;
		wc[i].wc_flags = IB_WC_GRH;
		wc[i].wc_flags |= IB_WC_WITH_NETWORK_HDR_TYPE;
		/* TODO */
#ifdef DEBUG_IPV6
		wc[i].network_hdr_type = RDMA_NETWORK_IPV4;
#else
		/* TODO */
		if (rqe->ip_version == 4) {
			wc[i].network_hdr_type = RDMA_NETWORK_IPV4;
		}
		else {
			wc[i].network_hdr_type = RDMA_NETWORK_IPV6;
		}
#endif
		if (qp->imm_inv_data[rq->cons].isvalid) {
			if (qp->imm_inv_data[rq->cons].type == SEND_INVALIDATE) {
				wc[i].wc_flags |= IB_WR_SEND_WITH_INV;
				wc[i].ex.invalidate_rkey = qp->imm_inv_data[rq->cons].data;
			} else if (qp->imm_inv_data[rq->cons].type == SEND_IMMEDIATE) {
				wc[i].wc_flags |= IB_WR_SEND_WITH_IMM;
				wc[i].ex.imm_data = qp->imm_inv_data[rq->cons].data;
			}
			qp->imm_inv_data[rq->cons].isvalid = false;
		}
		i++;

		xib_rq_cons_inc(rq);
		dev_dbg(&xib->ib_dev.dev, "IB_WC_RECV \n");
	}
	/* read current send cq db head */
	cur_send_cq_head = xrnic_ior(xl, XRNIC_CQ_HEAD_PTR(qp->hw_qpn)) &
				XRNIC_CQ_HEAD_PTR_MASK;

	dev_dbg(&xib->ib_dev.dev, "cur_send_cq_head: %d, send_cq_db_local: %d\n",
			cur_send_cq_head, sq->send_cq_db_local);
	/* send queue completions */
	while (i < num_entries && sq->send_cq_db_local != cur_send_cq_head) {
		if (sq->send_cq_db_local == sq->max_wr)
			sq->send_cq_db_local = 0;
		if (!sq->wr_id_array[sq->send_cq_db_local].signaled)
			goto skip_cqe;

		cqe = cq->buf_v + (sq->send_cq_db_local * sizeof(struct
					xrnic_cqe));
		wc[i].qp = &qp->ib_qp;
		wc[i].wr_id = sq->wr_id_array[sq->send_cq_db_local].wr_id;

		opcode = (cqe->entry & XRNIC_CQE_OPCODE_MASK) >>
			XRNIC_CQE_OPCODE_SHIFT;
		dev_dbg(&xib->ib_dev.dev, "cq opcode:%d \n", opcode);

		BUG_ON(opcode != XRNIC_SEND_ONLY);

		wc[i].opcode = IB_WC_SEND;

		err_flag = (cqe->entry & XRNIC_CQE_ERR_MASK) >>
				XRNIC_CQE_ERR_SHIFT;
		if (!err_flag)
			wc[i].status = IB_WC_SUCCESS;
		else {
			dev_err(&xib->ib_dev.dev, "%s: err in cqe\n", __func__);
			wc[i].status = IB_WC_FATAL_ERR;
			goto skip_cqe;
		}

		i++;
	skip_cqe:
		sq->send_cq_db_local++;
		dev_dbg(&xib->ib_dev.dev, "IB_WC_SEND \n");
	}
       	temp = sq->sqd_wr_list;
        while (i < num_entries && temp) {
		wc[i].wr_id = temp->wr_id;
		wc[i].status = IB_WC_WR_FLUSH_ERR;
                i++;
		head = temp;
		temp = temp->next;
		kfree(head);
                sq->sqd_length--;
	}
	sq->sqd_wr_list = temp;

	spin_unlock_irqrestore(&cq->cq_lock, flags);

	return i;
}


/*
 * rest rq for nvmf use case
 * with Hardware handshake
 */
int xib_rst_rq(struct xib_qp *qp)
{
	struct xilinx_ib_dev *xib = get_xilinx_dev(qp->ib_qp.device);
	struct xrnic_local *xl = xib->xl;
	struct xib_rq *rq = &qp->rq;
	u32 config;

	/* Enable SW override enable */
	xrnic_iow(xl, XRNIC_ADV_CONF, 1);
	wmb();

	xrnic_iow(xib->xl, XRNIC_RQ_CONS_IDX(qp->hw_qpn), 0);
	xrnic_iow(xib->xl, XRNIC_STAT_RQ_PROD_IDX(qp->hw_qpn), 0);

	xrnic_iow(xl, XRNIC_RCVQ_BUF_BASE_LSB(qp->hw_qpn), rq->rq_ba_p);
	xrnic_iow(xl, XRNIC_RCVQ_BUF_BASE_MSB(qp->hw_qpn),
					UPPER_32_BITS(rq->rq_ba_p));
	wmb();

	config = (QP_ENABLE | QP_HW_HSK_DIS | QP_CQE_EN);
	config |= QP_RQ_IRQ_EN | QP_CQ_IRQ_EN;

	/* TODO set pmtu ? */
	config |= (QP_PMTU_4096 << QP_PMTU_SHIFT);
	/* rq buf size in multiple of 256 */
	config |= ((qp->rq_buf_size >> 8) << QP_RQ_BUF_SZ_SHIFT);
	xrnic_iow(xl, XRNIC_QP_CONF(qp->hw_qpn), config);
	wmb();

	/* Disable SW override enable */
	xrnic_iow(xl, XRNIC_ADV_CONF, 0);
	wmb();

	/* TODO do we need these dummy reads ?*/
	xrnic_ior(xib->xl, XRNIC_RQ_CONS_IDX(qp->hw_qpn));
	xrnic_ior(xib->xl, XRNIC_STAT_RQ_PROD_IDX(qp->hw_qpn));

	xrnic_ior(xl, XRNIC_RCVQ_BUF_BASE_LSB(qp->hw_qpn));
	xrnic_ior(xl, XRNIC_RCVQ_BUF_BASE_MSB(qp->hw_qpn));

	return 0;
}

/*
 * rest sq and cq for nvmf use case
 * with Hardware handshake
 */
int xib_rst_cq_sq(struct xib_qp *qp, int nvmf_rhost)
{
	struct xilinx_ib_dev *xib = get_xilinx_dev(qp->ib_qp.device);
	struct xrnic_local *xl = xib->xl;
	dma_addr_t db_addr;
	u32 config, cnct_io_cnf;

	/* Enable SW override enable */
	xrnic_iow(xl, XRNIC_ADV_CONF, 1);

	xrnic_iow(xl, XRNIC_CQ_HEAD_PTR(qp->hw_qpn), 0);
	xrnic_iow(xl, XRNIC_SQ_PROD_IDX(qp->hw_qpn), 0);
	xrnic_iow(xl, XRNIC_STAT_CUR_SQ_PTR(qp->hw_qpn), 0);
	wmb();

	db_addr = 0x8E004000 + nvmf_rhost*4;
	xrnic_iow(xl, XRNIC_RCVQ_WP_DB_ADDR_LSB(qp->hw_qpn), db_addr);
	xrnic_iow(xl, XRNIC_RCVQ_WP_DB_ADDR_MSB(qp->hw_qpn),
						UPPER_32_BITS(db_addr));
	wmb();

	cnct_io_cnf = db_addr & 0xffff;

	db_addr = 0x8E004400 + nvmf_rhost*4;
	xrnic_iow(xl, XRNIC_CQ_DB_ADDR_LSB(qp->hw_qpn), db_addr);
	xrnic_iow(xl, XRNIC_CQ_DB_ADDR_MSB(qp->hw_qpn), UPPER_32_BITS(db_addr));
	wmb();

	config = xrnic_ior(xl, XRNIC_STAT_RQ_PROD_IDX(qp->hw_qpn));

	cnct_io_cnf |= (config & 0xffff) << 16;
	xrnic_iow(xl, XRNIC_CNCT_IO_CONF, cnct_io_cnf);


	/* enable HW Handshake */
	config = QP_ENABLE;
	config |= (QP_PMTU_4096 << QP_PMTU_SHIFT);
	/* rq buf size in multiple of 256 */
	config |= ((qp->rq_buf_size >> 8) << QP_RQ_BUF_SZ_SHIFT);
	xrnic_iow(xl, XRNIC_QP_CONF(qp->hw_qpn), config);
	wmb();

	/* Disable SW override enable */
	xrnic_iow(xl, XRNIC_ADV_CONF, 0);

	return 0;
}

/**
 * xib_drain_sq - drain sq
 * @ibqp: pointer to ibqp
 */
void xib_drain_sq(struct ib_qp *ibqp)
{
	struct xilinx_ib_dev *xib = get_xilinx_dev(ibqp->device);
	struct xib_qp *qp = get_xib_qp(ibqp);
        struct xib_sq *sq = &qp->sq;
}

/**
 * xib_drain_rq - drain rq
 * @ibqp: pointer to ibqp
 */
void xib_drain_rq(struct ib_qp *ibqp)
{
	struct xilinx_ib_dev *xib = get_xilinx_dev(ibqp->device);
	struct xib_qp *qp = get_xib_qp(ibqp);
	struct ib_qp_attr attr = { .qp_state = IB_QPS_ERR };
	int ret, val;
	unsigned long timeout;

	ret = ib_modify_qp(ibqp, &attr, IB_QP_STATE);
	if (ret) {
		pr_warn("failed to drain sq :%d\n", ret);
		return;
	}

	/* Wait till STAT_RQ_PI_DB == RQ_CI_DB */
	timeout = jiffies;
	do {
		val = xrnic_ior(xib->xl, XRNIC_RQ_CONS_IDX(qp->hw_qpn));
		ret = xrnic_ior(xib->xl, XRNIC_STAT_RQ_PROD_IDX(qp->hw_qpn));
		if (time_after(jiffies, (timeout + 1 * HZ)))
			break;
	} while(!(val == ret));
}
