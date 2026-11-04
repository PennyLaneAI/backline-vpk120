// SPDX-License-Identifier: GPL-2.0
/*
 * axidma.c - This file for axi dma stream implementation
 * to get the Immediate and Rkey data to XRNIC driver
 * 
 * Copyright (C) 2024 AMD, Inc. All rights reserved.
 *
 * Author : Priyadarshini Babu <priyadar@xilinx.com>
 * 
 * MODIFICATION HISTORY:
 * Ver   Who                 Date         Changes
 * ----- ----                --------     ----------------------------
 * 1.00  Priydarshini Babu   14/04/2020   Original code.
 * */

#include <linux/of_irq.h>
#include <linux/of_platform.h>
#include <linux/of_address.h>
#include <linux/spinlock.h>
#include <linux/interrupt.h>
#include <linux/dma-mapping.h>
#include <rdma/ib_pack.h>

#include "ib_verbs.h"
#include "xib.h"
#include "axidma.h"

#define RX_BD_NUM   128

static u32 axidma_in32(struct axidma_q *dq, off_t reg)
{
#ifdef CONFIG_MICROBLAZE
	return in_be32(dq->dma_regs + reg);
#else
	return __raw_readl(dq->dma_regs + reg);
#endif
}

static void axidma_out32(struct axidma_q *dq, off_t reg, u32 value)
{
#ifdef CONFIG_MICROBLAZE
	out_be32(dq->dma_regs + reg, value);
#else
	__raw_writel(dq->dma_regs + reg, value);
#endif
}

static void axidma_bdout(struct axidma_q *dq,
		                        off_t reg, dma_addr_t value)
{
#if defined(CONFIG_PHYS_ADDR_T_64BIT)
	writeq(value, (dq->dma_regs + reg));
#else
	writel(value, (dq->dma_regs + reg));
#endif
}

static void axidma_bd_release(struct xilinx_ib_dev *xib)
{
	struct axidma_q	*dq = xib->dmaq;

	if (dq->rq_noti_buf[0])
		dma_free_coherent(&xib->pdev->dev,
				sizeof(**dq->rq_noti_buf) * RX_BD_NUM,
				dq->rq_noti_buf[0], dq->rx_bd_v[0].phys);

	if (dq->rq_noti_buf)
		kfree(dq->rq_noti_buf);

	if (dq->rx_bd_v)
		dma_free_coherent(&xib->pdev->dev,
				sizeof(*dq->rx_bd_v) * RX_BD_NUM,
				dq->rx_bd_v, dq->rx_bd_p);
}

static int axidma_bd_init(struct xilinx_ib_dev *xib)
{
	struct axidma_q	*dq = xib->dmaq;
	int i;
	struct rq_noti_buffer *buf = NULL;
	u64 phy_addr;
	u32 buf_size;
	unsigned int cr;

	dq->rq_noti_buf = kcalloc(RX_BD_NUM, sizeof(*dq->rq_noti_buf),
					GFP_KERNEL);
	if (!dq->rq_noti_buf) {
		dev_err(&xib->pdev->dev, "could not allocate memory to "
					"**rq_noti_buf\n");
		goto out;
	}

	buf_size = sizeof(*buf);

	buf = dma_alloc_coherent(&xib->pdev->dev, buf_size * RX_BD_NUM,
					(dma_addr_t *)&phy_addr, GFP_KERNEL);
	if (!buf) {
		dev_err(&xib->pdev->dev, "could not allocate memory to "
					"*rq_noti_buf\n");
		goto out;
	}

	dq->rx_bd_v = dma_alloc_coherent(&xib->pdev->dev,
					  sizeof(*dq->rx_bd_v) * RX_BD_NUM,
					  (dma_addr_t *)&dq->rx_bd_p, GFP_KERNEL);
	if (!dq->rx_bd_v) {
		dev_err(&xib->pdev->dev, "could not allocate memory to "
					"rx_bd_p\n");
		goto out;
	}

	for (i = 0; i < RX_BD_NUM; i++) {
		dq->rx_bd_v[i].next = dq->rx_bd_p +
				sizeof(*dq->rx_bd_v) * ((i + 1) % RX_BD_NUM);

		dq->rq_noti_buf[i] = buf++;
		dq->rx_bd_v[i].phys = phy_addr; 
		phy_addr += buf_size;
		dq->rx_bd_v[i].cntrl = buf_size;
	}

	/* Start updating the Rx channel control register */
	cr = axidma_in32(dq, XAXIDMA_RX_CR_OFFSET);
	/* Update the interrupt coalesce count */
	cr = ((cr & ~XAXIDMA_COALESCE_MASK) |
			(XAXIDMA_DFT_RX_THRESHOLD << XAXIDMA_COALESCE_SHIFT));
	/* Update the delay timer count */
	cr = ((cr & ~XAXIDMA_DELAY_MASK) |
			(XAXIDMA_DFT_RX_WAITBOUND << XAXIDMA_DELAY_SHIFT));
	/* Enable coalesce, delay timer and error interrupts */
	cr |= XAXIDMA_IRQ_ALL_MASK;
	/* Write to the Rx channel control register */
	axidma_out32(dq, XAXIDMA_RX_CR_OFFSET, cr);

	/* Populate the tail pointer and bring the Rx Axi DMA engine out of
	 *          * halted state. This will make the Rx side ready for reception.
	 *                   */
	axidma_bdout(dq, XAXIDMA_RX_CDESC_OFFSET, dq->rx_bd_p);
	cr = axidma_in32(dq, XAXIDMA_RX_CR_OFFSET);
	axidma_out32(dq, XAXIDMA_RX_CR_OFFSET,
			cr | XAXIDMA_CR_RUNSTOP_MASK);
	axidma_bdout(dq, XAXIDMA_RX_TDESC_OFFSET, dq->rx_bd_p +
			(sizeof(*dq->rx_bd_v) * (RX_BD_NUM - 1)));

	dq->rx_bd_ci = 0;

	return 0;

out:
	axidma_bd_release(xib);
	return -ENOMEM;
}

static void axidma_rq_recv (struct xilinx_ib_dev *xib)
{
	struct axidma_q	*dq = xib->dmaq;
	struct xib_qp *qp;
	u32 size = 0;
	u32 packets = 0;
	dma_addr_t tail_p = 0;
	struct axidma_bd *cur_p;
	struct rq_noti_buffer *buf = NULL;

	/* Get relevat BD status value */
	rmb();
	cur_p = &dq->rx_bd_v[dq->rx_bd_ci];

	while (cur_p->status & XAXIDMA_BD_STS_COMPLETE_MASK) {
		tail_p = dq->rx_bd_p + sizeof(*dq->rx_bd_v) * dq->rx_bd_ci;
		buf = dq->rq_noti_buf[dq->rx_bd_ci];

		if (buf->type == SEND_INVALIDATE ||
				buf->type == SEND_IMMEDIATE) {
			qp = xib->qp_list[buf->qpid-1];
			qp->imm_inv_data[buf->rq_pi_ptr-1].data = buf->data;
			qp->imm_inv_data[buf->rq_pi_ptr-1].type = buf->type;
			qp->imm_inv_data[buf->rq_pi_ptr-1].isvalid = true;
		}

		if (buf->type == SEND_INVALIDATE)
			xrnic_unreg_mr(xib, buf->data);

		size += sizeof (*buf);
		packets++;

		cur_p->cntrl = sizeof (*buf);
		cur_p->status = 0;

		++dq->rx_bd_ci;
		dq->rx_bd_ci %= RX_BD_NUM;

		/* Get relevat BD status value */
		rmb();
		cur_p = &dq->rx_bd_v[dq->rx_bd_ci];
	}

	dq->rx_packets += packets;
	dq->rx_bytes += size;

	if (tail_p) {
		axidma_bdout(dq, XAXIDMA_RX_TDESC_OFFSET, tail_p);
	}
	return;
}

static irqreturn_t axidma_rx_irq(int irq, void *_xib)
{
	struct xilinx_ib_dev *xib = _xib;
	struct axidma_q	*dq = xib->dmaq;
	unsigned int status, cr;
	unsigned long flags;

	status = axidma_in32(dq, XAXIDMA_RX_SR_OFFSET);
	if (status & XAXIDMA_IRQ_ERROR_MASK) {
		dev_err(&xib->pdev->dev, "Rx error 0x%x\n\r", status);
		return IRQ_HANDLED;
	}

	spin_lock_irqsave(&dq->rx_lock, flags);
	if (status & (XAXIDMA_IRQ_IOC_MASK | XAXIDMA_IRQ_DELAY_MASK)) {
		axidma_out32(dq, XAXIDMA_RX_SR_OFFSET, status);                           
		/* Disable the interrupts until notifier buffer get processed */
		cr = axidma_in32(dq, XAXIDMA_RX_CR_OFFSET);
		cr &= ~(XAXIDMA_IRQ_IOC_MASK | XAXIDMA_IRQ_DELAY_MASK);
		axidma_out32(dq, XAXIDMA_RX_CR_OFFSET, cr);
		axidma_rq_recv(xib);                    
	}

	/* Enable the interrupts again */
	cr = axidma_in32(dq, XAXIDMA_RX_CR_OFFSET);
	cr |= (XAXIDMA_IRQ_IOC_MASK | XAXIDMA_IRQ_DELAY_MASK);
	axidma_out32(dq, XAXIDMA_RX_CR_OFFSET, cr);

	spin_unlock_irqrestore(&dq->rx_lock, flags);
	return IRQ_HANDLED;
}

int axidma_init(struct xilinx_ib_dev *xib, struct device_node *ernic_np)
{
	struct axidma_q	*dq = NULL;
	struct device_node *np;
	struct resource dmares;
	int ret;

	/* Find the DMA node, map the DMA registers, and decode the DMA IRQs */
	np = of_parse_phandle(ernic_np, "axidma-handle", 0);
	if (!np) {
		dev_err(&xib->pdev->dev, "could not find DMA node\n");
		xib->dmaq = NULL;
		return -ENODEV;
	}

	dq = kzalloc(sizeof(*dq), GFP_KERNEL);
	if (!dq) {
		dev_err(&xib->pdev->dev, "could not allocate memory for axidma_q\n");
		xib->dmaq = NULL;
		return -ENOMEM;
	}

	xib->dmaq = dq;
	ret = of_address_to_resource(np, 0, &dmares);
	if (ret >= 0) {
		dq->dma_regs = devm_ioremap_resource(&xib->pdev->dev,
				&dmares);
		if (IS_ERR(dq->dma_regs)) {
			dev_err(&xib->pdev->dev, "Failed to ioremap DMA node resource\n");
			return -ENODEV;
		}
	} else {
		dev_err(&xib->pdev->dev, "Failed to get DMA node resource\n");
		return -ENODEV;
	}

	dq->rx_irq = irq_of_parse_and_map(np, 0);

	of_node_put(np); /* Finished with the DMA node; drop the reference */

	if (!dq->rx_irq) {
		dev_err(&xib->pdev->dev, "could not determine irq\n");
		return -ENODEV;
	}
	
	if (axidma_bd_init(xib)) {
		dev_err(&xib->pdev->dev,
			"axidma_bd_init descriptor allocation failed\n");
	}

	spin_lock_init(&dq->rx_lock);
	ret = request_irq(dq->rx_irq, axidma_rx_irq, 0, "axidma_rx", xib);
	if (ret) {
		dq->rx_irq = 0;
		dev_err(&xib->pdev->dev, "request_irq() failed\n");
		return ret;
	}
	return 0;
}

int axidma_exit(struct xilinx_ib_dev *xib)
{
	struct axidma_q	*dq = xib->dmaq;

	if (dq) {
		if(dq->rx_irq)
			free_irq(dq->rx_irq, xib);

		axidma_bd_release(xib);
		kfree(dq);
	}
	return 0;
}
