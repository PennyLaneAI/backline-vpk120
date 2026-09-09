/*
 * AMD FPGA Xilinx ERNIC Infiniband Driver
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
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/interrupt.h>
#include <linux/of_platform.h>
#include <asm/byteorder.h>
#include <rdma/ib_addr.h>
#include <rdma/ib_smi.h>
#include <rdma/ib_user_verbs.h>
#include <rdma/ib_cache.h>
#ifndef NO_NVMF
#include <linux/xnvmf.h>
#endif
#include "rnic.h"
#include "xib.h"
#include "ib_verbs.h"
#include "xib_kmm_export.h"

#define DEBUG

/* #undef dev_dbg */
/* #undef dev_info */
/* #undef dev_warn */
/* #undef dev_notice */

/* #define dev_dbg dev_err */
/* #define dev_info dev_err */
/* #define dev_warn dev_err */
/* #define dev_notice dev_err */
/*
   max qps enabled = 30
*/

/* connection type UC */


void xrnic_send_wr(struct xib_qp *qp, struct xilinx_ib_dev *xib)
{
	struct xrnic_local *xl = xib->xl;

	if (!SQ_BASE_ALIGNED(qp->send_sgl_p))
		dev_err(&xib->ib_dev.dev, "warning send sgl is not aligned \n");

	qp->sq.sq_cmpl_db_local++;
	/* increment the SQ PI Doorbell */
	xrnic_iow(xl, XRNIC_SQ_PROD_IDX(qp->hw_qpn), qp->sq.sq_cmpl_db_local);
	wmb();
	if (qp->sq.sq_cmpl_db_local == qp->sq.max_wr) {
		qp->sq.sq_cmpl_db_local = 0;
	}
	qp->send_sgl_busy = false;
}

void xrnic_set_dest_ip(struct xib_qp *qp)
{
	struct ib_qp *ibqp = &qp->ib_qp;
	struct xilinx_ib_dev *xib = get_xilinx_dev(ibqp->device);
	struct xrnic_local *xl = xib->xl;

	xrnic_iow(xl, XRNIC_IP_DEST_ADDR_1(qp->hw_qpn), qp->qp1_hdr.ip4.daddr);
}

void config_raw_ip(struct xrnic_local *xl, u32 base, u32 *ip, bool is_ipv6)
{
	u32 val = 0, i;

	if (!is_ipv6) {
		val = cpu_to_be32(*ip);
		xrnic_iow(xl, base, val);
	} else {
		for (i = 0; i < 4; i++) {
			val = cpu_to_be32(ip[i]);
			xrnic_iow(xl, base + (3 - i) * 4, val);
		}
	}
}

void qp_set_ipv6_destination(struct xrnic_local *xl, int qpn, u8* raw_ipv6)
{
        u32 config = 0, i;
        u32 ip_base = XRNIC_IP_DEST_ADDR_1(qpn);

	config_raw_ip(xl, ip_base, (u32 *)raw_ipv6, true);
}

void xrnic_set_mac(struct xrnic_local *xl, u8 *mac)
{
	u32 val;

	val = mac[5] | (mac[4] << 8) |
		(mac[3] << 16) | (mac[2] << 24);

	xrnic_iow(xl, XRNIC_MAC_ADDR_LO, val);

  	wmb();
  	val =  mac[1] | (mac[0] << 8);
	xrnic_iow(xl, XRNIC_MAC_ADDR_HI, val);
}

void get_raw_ip(struct xrnic_local *xl, u32 base, u32 *addr, bool is_ipv6)
{
	u32 val, i;

	if (!xl || !addr)
		return;

	if (!is_ipv6) {
		val = xrnic_ior(xl, base);
		val = cpu_to_be32(val);
		*addr = val;
	} else {
		for (i = 0; i < 4; i++) {
			val  = xrnic_ior(xl, base + (3 - i) * 4);
			val = cpu_to_be32(val);
			addr[i] = val;
		}
	}
}

void xrnic_set_ip_address(struct xrnic_local *xl, u8 *addr, int len)
{
	u32 val;
	if (len == 4) {
		val  = (addr[3]<<0)|( addr[2]<<8)|
			(addr[1]<<16)|(addr[0]<<24);
		xrnic_iow(xl, XRNIC_IPV4_ADDR, val);
		wmb();
	}

	if (len == 16) {
		val  = (addr[0]<<0)|( addr[1]<<8)|
			(addr[2]<<16)|(addr[3]<<24);
		xrnic_iow(xl, XRNIC_IPV6_ADD_1, val);
		wmb();
		val  = (addr[4]<<0)|( addr[5]<<8)|
			(addr[6]<<16)|(addr[7]<<24);
		xrnic_iow(xl, XRNIC_IPV6_ADD_2, val);
		wmb();
		val  = (addr[8]<<0)|( addr[9]<<8)|
			(addr[10]<<16)|(addr[11]<<24);
		xrnic_iow(xl, XRNIC_IPV6_ADD_3, val);
		wmb();
		val  = (addr[12]<<0)|( addr[13]<<8)|
			(addr[14]<<16)|(addr[15]<<24);
		xrnic_iow(xl, XRNIC_IPV6_ADD_4, val);
		wmb();
	}
}



static void hex_dump(unsigned char *buf, int len)
{
	while (len--)
		pr_cont("%02x", *buf++);
}


int send_out_cnp(struct xib_qp *qp)
{
	#define RDMA_CNP_RSVD_DATA_BYTES 16
	#define RDMA_ECN_BYTE_OFS	(0x2E)
	#define RDMA_BECN_BIT		(BIT(6))

	struct xib_qp *qp1;
	struct xilinx_ib_dev *xib;
	int flags, ip_version = 4, size;
	u32 data;
	u16 ether_type = ETH_P_IP;
	u8 data_buf[RDMA_CNP_RSVD_DATA_BYTES];
	bool is_udp = true, is_eth = true, is_grh = false;
	u8 *buf;
	struct xrnic_wr *xwqe;

        xib = get_xilinx_dev(qp->ib_qp.device);

	/* qp_list has QPs as per their hw_qpn,
           qp1 has hw qpn of 0 */
	qp1 = xib->gsi_qp;

	/* @TODO: Get IP version and then pass it to the up header init*/
	data = xrnic_ior(xib->xl, XRNIC_MAC_DEST_ADDR_HI(qp->hw_qpn));
	*(u16 *)data_buf = (((data & 0xFF) << 8) | ((data >> 8) & 0xFF));
	data = xrnic_ior(xib->xl, XRNIC_MAC_DEST_ADDR_LO(qp->hw_qpn));
	*(u32 *)&data_buf[2] = cpu_to_be32(data);

	if (ip_version == 6)
		ether_type = ETH_P_IPV6;

	spin_lock_irqsave(&qp1->sq_lock, flags);
	ib_ud_header_init(RDMA_CNP_RSVD_DATA_BYTES - sizeof(qp->qp1_hdr.deth), !is_eth, is_eth, false, is_grh,
		ip_version, is_udp, 0, &qp1->qp1_hdr);

	/* MAC should be in BE */
	ether_addr_copy(qp1->qp1_hdr.eth.dmac_h, data_buf);
	ether_addr_copy(qp1->qp1_hdr.eth.smac_h, xib->netdev->dev_addr);
	qp1->qp1_hdr.eth.type = cpu_to_be16(ether_type);

        if (!qp->is_ipv6) {
                qp1->qp1_hdr.ip4.tos = 0;
                qp1->qp1_hdr.ip4.id = 0;
                qp1->qp1_hdr.ip4.frag_off = htons(IP_DF);
                /* qp->qp1_hdr.ip4.ttl = grh->hop_limit; */
                /* TODO check why its coming back as zero */
                qp1->qp1_hdr.ip4.ttl = 64;

		data = xrnic_ior(xib->xl, XRNIC_IPV4_ADDR);
		data = cpu_to_be32(data);
                memcpy(&qp1->qp1_hdr.ip4.saddr, (void *)&data, 4);
		data = xrnic_ior(xib->xl, XRNIC_IP_DEST_ADDR_1(qp->hw_qpn));
		config_raw_ip(xib->xl, XRNIC_IP_DEST_ADDR_1(qp->hw_qpn),
			&data, 0);
		data = cpu_to_be32(data);
                memcpy(&qp1->qp1_hdr.ip4.daddr, (void *)&data, 4);
                qp1->qp1_hdr.ip4.check = ib_ud_ip4_csum(&qp->qp1_hdr);
        } else {
		data = qp->is_ipv6? XRNIC_IPV6_ADD_1: XRNIC_IPV4_ADDR;
                /* copy the GIDs / IPV6 addresses */
                qp->qp1_hdr.grh.hop_limit = 64;
                qp->qp1_hdr.grh.traffic_class = 0;
                qp->qp1_hdr.grh.flow_label= 0;
		get_raw_ip(xib->xl, XRNIC_IP_DEST_ADDR_1(qp->hw_qpn),
			(u32 *)&qp->qp1_hdr.grh.destination_gid, qp->is_ipv6);
		get_raw_ip(xib->xl, data, (u32 *)&qp->qp1_hdr.grh.source_gid, qp->is_ipv6);
		config_raw_ip(xib->xl, XRNIC_IP_DEST_ADDR_1(qp->hw_qpn),
			(u32 *)qp->qp1_hdr.grh.source_gid.raw, 1);
	}

        if (is_udp) {
                qp1->qp1_hdr.udp.dport = htons(ROCE_V2_UDP_DPORT);
                qp1->qp1_hdr.udp.sport = htons(0x8CD1);
                qp1->qp1_hdr.udp.csum = 0;
        }

	qp1->qp1_hdr.bth.opcode = IB_OPCODE_CNP;
	qp1->qp1_hdr.bth.pad_count = 0;
	qp1->qp1_hdr.bth.pkey = cpu_to_be16(0xFFFF);
	data = xrnic_ior(xib->xl, XRNIC_DEST_QP_CONF(qp->hw_qpn));
	qp1->qp1_hdr.bth.destination_qpn = cpu_to_be32(data);
	qp1->qp1_hdr.bth.psn = 0;
	qp1->qp1_hdr.bth.solicited_event = 0;
	qp1->qp1_hdr.bth.ack_req = 0;
	qp1->qp1_hdr.bth.mig_req = 0;

	qp->qp1_hdr.deth.source_qpn = 0;
	qp->qp1_hdr.deth.qkey = 0;

	buf = (u8 *)qp1->send_sgl_v;
	size = ib_ud_header_pack(&qp1->qp1_hdr, buf);

	/* set becn @TODO Remove Hardcoded macros*/
	buf[RDMA_ECN_BYTE_OFS] = RDMA_BECN_BIT;

	buf = buf + size;
	memset(buf, 0, RDMA_CNP_RSVD_DATA_BYTES);

	/* CNP Packet expectss 16 0's after BTH, but the ib_ud_header_pack
	   API considers DETH as default header because of it's transport type UD */
	size += (RDMA_CNP_RSVD_DATA_BYTES - sizeof(qp->qp1_hdr.deth));

	/* prepare the WQE to post into SQ */
	xwqe = (struct xrnic_wr *)((unsigned long)(qp1->sq_ba_v) +
                                qp1->sq.sq_cmpl_db_local * sizeof(*xwqe));
	xwqe->wrid = 0;
	xwqe->l_addr = qp1->send_sgl_p;
	xwqe->r_offset = 0;
	xwqe->r_tag = 0;
	xwqe->length = size;
	xwqe->opcode = XRNIC_SEND_ONLY;
	xrnic_send_wr(qp1, xib);

	spin_unlock_irqrestore(&qp1->sq_lock, flags);
	return 0;
}

void xib_cnp_handler(unsigned long data)
{
	int ret;
	/* prepapre & send  CNP packet */
	ret = send_out_cnp((struct xib_qp *)data);
	if (ret)
		pr_err("%s:%d Failed to post CNP\n", __func__, __LINE__);
}

void xib_comp_handler(unsigned long data)
{
	struct xib_qp *qp = (struct xib_qp *)data;
	struct xib_cq *cq;

        //printk("xib_comp_handler\n");
	if (!qp) {
		pr_err("%s: qp is null\n", __func__);
		return;
	}

	/* check for rq completions */
	cq = qp->rq_cq;

	if (cq->ib_cq.comp_handler)
		(*cq->ib_cq.comp_handler) (&cq->ib_cq, cq->ib_cq.cq_context);

	cq = qp->sq_cq;
	/* check for sq completions */
	if (cq->ib_cq.comp_handler)
		(*cq->ib_cq.comp_handler) (&cq->ib_cq, cq->ib_cq.cq_context);
}

void xib_fatal_handler(unsigned long data)
{
	struct xib_qp *qp = (struct xib_qp *)data;
	struct ib_qp *ibqp = &qp->ib_qp;
	struct xilinx_ib_dev *xib = get_xilinx_dev(ibqp->device);
	unsigned long timeout;
	u32 val;
	int rhost, ret;

	/* 1. Wait till SQ/OSQ are empty */
	val = xrnic_ior(xib->xl, XRNIC_STAT_QP(qp->hw_qpn));
	while(1) {
		val = xrnic_ior(xib->xl, XRNIC_STAT_QP(qp->hw_qpn));
		if ((val >> 9) & 0x3)
			break;
	}

	/* 2. Check SQ PI == CQ Head */
	timeout = jiffies;
	do {
		val = xrnic_ior(xib->xl, XRNIC_SQ_PROD_IDX(qp->hw_qpn));
		ret = xrnic_ior(xib->xl, XRNIC_CQ_HEAD_PTR(qp->hw_qpn));
		if (time_after(jiffies, (timeout + 1 * HZ)))
			break;
	} while(!(val == ret));


	/* put the qp in recovery */
	xrnic_qp_under_recovery(xib, qp->hw_qpn);

#if 0 /* NVME Code */
	if (qp->io_qp) {
		rhost = xnvmf_get_rhost(qp->hw_qpn + 1);
		xnvmf_flush_rhost(rhost);
	}
#endif
}

int is_ipv4_addr(char *buf)
{
	/*TODO: Current design posts all 0's as the first 20B if it's IPV4 header,
		Request for v6 mapped v4 address */
	uint32_t *bufp = (uint32_t *)buf, i, val = 0, lim = (IB_GRH_BYTES - IB_IP4_BYTES) / sizeof(*bufp);

	for (i = 0; i < lim; i++)
		val |= buf[i];
	return val;
}

void xib_gsi_comp_handler(unsigned long data)
{
	#define	IPV4_PAYLOAD_DATA_OFS	(20)
	struct xilinx_ib_dev *xib = (struct xilinx_ib_dev *)data;
	struct xrnic_local *xl = xib->xl;
	struct xib_qp *qp = xib->gsi_qp;
	struct xib_rq *rq = &qp->rq;
	volatile u32 *rq_db_addr;
	u32 rq_wr_current;
	struct ethhdr *eth_hdr;
	struct iphdr *iph;
	struct ipv6hdr *ip6h;
	struct xib_cq *cq;
	struct xib_rqe *rqe;
	int i, received, offset = 0, ret = 0;
	int mad_len, rq_pkt_num;
	u8 *buf;
	u8 *sgl_va;
	int ip_version = 4, temp = 0;

        //printk("xib_gsi_comp_handler\n");
	/* check for rq completions */
	rq_db_addr = (volatile u32 *)xl->qp1_rq_db_v;
	rq_wr_current = *rq_db_addr;

	dev_dbg(&xib->ib_dev.dev, "rq_wr_current: %d, rq_wrptr_db_local: %d\n", rq_wr_current,
			rq->rq_wrptr_db_local);

	received = xib_get_rq_recd(rq, rq_wr_current);

	dev_dbg(&xib->ib_dev.dev, "received %d frames on RQ\n", received);

	for (i = 0; i < received; i++) {
		rq_pkt_num = rq->rq_wrptr_db_local;
		if (rq_pkt_num >= rq->max_wr) {
			rq_pkt_num = rq_pkt_num - rq->max_wr;
		}

		buf = (u8 *)(rq->rq_ba_v) + (rq_pkt_num * qp->rq_buf_size);
		ret = is_ipv4_addr(buf);
		if (ret) {
			ip6h = (struct ipv6hdr *)buf;
			mad_len = IB_GRH_BYTES + ntohs(ip6h->payload_len);
			ip_version = 6;
		} else {
			/* Set ipv4 header ptr to buf + 20 which actually is the
				start of ipv4 header */
			iph = (struct iphdr *)(buf + IB_GRH_BYTES - IB_IP4_BYTES);
			mad_len = ntohs(iph->tot_len);
			ip_version = 4;
		}

		rqe = &(rq->rqe_list[rq->gsi_cons]);

		/* TODO sg_list.addr is hw (dma) address
		 * this works on uBlaze as VA = PA */
		/* first copy iphdr */
		/* TODO copy 40 bytes for ipv6 */
		sgl_va = (u8 *)phys_to_virt((unsigned long)(rqe->sg_list[0].addr));

		rqe->ip_version = ip_version;
		memcpy((void *)sgl_va, buf, IB_GRH_BYTES);
		offset = IB_GRH_BYTES;
		buf += (IB_GRH_BYTES + IB_UDP_BYTES + IB_BTH_BYTES + IB_DETH_BYTES); /* skip ip, udp, ibh, deth */

		temp = IB_UDP_BYTES + IB_BTH_BYTES + IB_DETH_BYTES;
		if (ip_version == 4)
			temp += IB_IP4_BYTES;
		else
			temp += IB_GRH_BYTES;
		memcpy((void *)(sgl_va + offset), buf, mad_len - temp);

		rqe->sg_list[0].length = mad_len - 8;

		xib_inc_sw_gsi_cons(rq);

		rq->rq_wrptr_db_local++;
		/* tell hw we consumed */
		xrnic_iow(xl, XRNIC_RQ_CONS_IDX(qp->hw_qpn),
				rq->rq_wrptr_db_local);
		wmb();

		if(rq->rq_wrptr_db_local == rq->max_wr)
			rq->rq_wrptr_db_local = 0;

		cq = qp->rq_cq;
		if (cq->ib_cq.comp_handler)
			(*cq->ib_cq.comp_handler) (&cq->ib_cq, cq->ib_cq.cq_context);
	}

	cq = qp->sq_cq;
	/* check for sq completions */
	if (cq->ib_cq.comp_handler)
		(*cq->ib_cq.comp_handler) (&cq->ib_cq, cq->ib_cq.cq_context);
}

void xib_irq_event(struct xilinx_ib_dev *xib, u32 status)
{
	struct xrnic_local *xl = xib->xl;
	u32 cq_ints, base;
	struct xib_qp *qp = NULL;
	uint32_t i, qpn, qp_num;

	if (status & XRNIC_INT_WQE_COMP)
		base = XRNIC_COMPQ_INT_STAT_0_31;
	else if(status & XRNIC_INT_RQ_PKT_RCVD)
		base = XRNIC_RCVQ_INT_STAT_0_31;
	else if (status & XRNIC_INT_CNP)
		base = XRNIC_CNP_INT_STAT_0_31;
	else
		return;

	for (i = 0; i < xl->num_intr_banks; i++) {
		cq_ints = xrnic_ior(xl, (base + (i << 2)));

		/* clear cq int*/
		xrnic_iow(xl, (base + (i << 2)), cq_ints);

		qpn = ffs(cq_ints);
		while (qpn) {
			//dev_dbg(&xib->ib_dev.dev, "wqe complete on qpn: %d\n", qpn);
			qp_num = (i << 5) + qpn;
			qp = xib->qp_list[qp_num - 2];

			/* the Bottom half invokes kernel user app's functions registered
				for RQ & CQ completions which are not invoked when it's a
				user QP */
			if (!qp) {
				pr_err("qp doesnt exist : qpn is %d \n", qp_num);
			}

			if (status & XRNIC_INT_CNP)
				tasklet_schedule(&qp->cnp_task);
			else if (qp->qp_type != XIB_QP_TYPE_USER)
				tasklet_schedule(&qp->comp_task);

			cq_ints &= ~(1 << (qpn - 1));
			qpn = ffs(cq_ints);
		}
	}
}

void xib_fatal_event(struct xilinx_ib_dev *xib)
{
	u32 in_pkt_err_db;
	struct xrnic_local *xl = xib->xl;
	u32 entry, qp_num, val;
	int i, received;
	struct xib_qp *qp = NULL;

	in_pkt_err_db = xrnic_ior(xl, XRNIC_INCG_PKT_ERRQ_WPTR);

	if (in_pkt_err_db == xl->in_pkt_err_db_local) {
		return;
	}

	if (xl->in_pkt_err_db_local > in_pkt_err_db)
		received = (in_pkt_err_db + xib->dev_attr.max_qp) -
				xl->in_pkt_err_db_local;
	else
		received = in_pkt_err_db - xl->in_pkt_err_db_local;

	for (i = 0; i < received; i++) {
		if (xl->in_pkt_err_db_local == xib->dev_attr.max_qp)
			xl->in_pkt_err_db_local = 0;
		entry = xl->in_pkt_err_db_local;

		val = *(volatile u32 *)(xl->in_pkt_err_va + (8 * entry));
		qp_num = (val & 0xffff0000) >> 16;
		printk("Fatal error on qp_num: %d\n", qp_num);
		printk("Specific error: %x\n", (val & 0x0000ffff));
		val = xrnic_ior(xib->xl, XRNIC_QP_CONF(qp_num - 1));
		val &= ~(QP_ENABLE);
		val |= QP_UNDER_RECOVERY;
		xrnic_iow(xib->xl, XRNIC_QP_CONF(qp_num - 1), val);

		qp = xib->qp_list[qp_num - 1];
		if (qp)
			tasklet_schedule(&qp->fatal_hdlr_task);
		xl->in_pkt_err_db_local++;
	}

	/* clear fatal interrupt */
	val = xrnic_ior(xib->xl, XRNIC_INT_STAT);
	val |= XRNIC_INT_FATAL_ERR_RCVD;
	xrnic_iow(xib->xl, XRNIC_INT_STAT, val);
}
/*
 *
 */
irqreturn_t xib_irq(int irq, void *ptr)
{
	struct xilinx_ib_dev *xib = (struct xilinx_ib_dev *)ptr;
	struct xrnic_local *xl = xib->xl;
	u32 status, cq_ints;
	struct xib_qp *qp;
	int qpn;

	status = xrnic_ior(xl, XRNIC_INT_STAT);
	rmb();

        if(!(status & 0x1))
        {
            //printk("%s status : %d <---------- \n", __func__, status);
	dev_dbg(&xib->ib_dev.dev, "%s status : %d <---------- \n", __func__, status);
        }

	/* clear the interrupt */
	xrnic_iow(xl, XRNIC_INT_STAT, status);
	wmb();

	if (status & XRNIC_INT_WQE_COMP)
		xib_irq_event(xib, XRNIC_INT_WQE_COMP);

	if (status & XRNIC_INT_RQ_PKT_RCVD)
		/* @TODO This is to  test CNP. Remove this once the testing is done */
		xib_irq_event(xib, XRNIC_INT_RQ_PKT_RCVD);

	if (status & XRNIC_INT_CNP)
		xib_irq_event(xib, XRNIC_INT_CNP);

	if (status & XRNIC_INT_INC_MAD_PKT) {
		qp = xib->gsi_qp;
		tasklet_schedule(&qp->comp_task);
	}

	if (status & XRNIC_INT_FATAL_ERR_RCVD) {
		pr_err("xib_irq: qp fatal error!\n");
		xib_fatal_event(xib);
	}


	return IRQ_HANDLED;
}

int xrnic_qp_under_recovery(struct xilinx_ib_dev *xib, int hw_qpn)
{
	int val = 0;

	/* Disable the QP */
	val = xrnic_ior(xib->xl, XRNIC_QP_CONF(hw_qpn));
	val &= ~(QP_ENABLE);
	xrnic_iow(xib->xl, XRNIC_QP_CONF(hw_qpn), val);

	/* put QP under recovery */
	xrnic_iow(xib->xl, XRNIC_QP_CONF(hw_qpn), val);
	val |= QP_UNDER_RECOVERY;
	xrnic_iow(xib->xl, XRNIC_QP_CONF(hw_qpn), val);

	return 0;
}

int xrnic_qp_disable(struct xib_qp *qp)
{
	struct ib_qp *ibqp = &qp->ib_qp;
	struct xilinx_ib_dev *xib = get_xilinx_dev(ibqp->device);
	struct xrnic_local *xl = xib->xl;

	xrnic_iow(xl, XRNIC_QP_CONF(qp->hw_qpn), 0);
	wmb();

	return 0;
}

int xrnic_qp_modify(struct xib_qp *qp, struct xib_qp_modify_params *params)
{
	struct ib_qp *ibqp = &qp->ib_qp;
	struct xilinx_ib_dev *xib = get_xilinx_dev(ibqp->device);
	struct xrnic_local *xl = xib->xl;
	u32 config = 0;

	dev_dbg(&xl->pdev->dev, "%s : <---------- \n", __func__);

	if (params->flags & XIB_MODIFY_QP_DEST_QP) {
		dev_dbg(&xib->ib_dev.dev, "setting dest_qp: %d\n", params->dest_qp);
		xrnic_iow(xl, XRNIC_DEST_QP_CONF(qp->hw_qpn), params->dest_qp);
	}

	if (params->flags & XIB_MODIFY_QP_DEST_MAC) {
		u32 val;

		dev_dbg(&xib->ib_dev.dev, "setting dest mac\n");
		val = (params->dmac[5]) | (params->dmac[4] << 8) |
			(params->dmac[3] << 16) | (params->dmac[2] << 24);
		xrnic_iow(xl, XRNIC_MAC_DEST_ADDR_LO(qp->hw_qpn), val);
		wmb();
		val = params->dmac[1] | (params->dmac[0] << 8);
		xrnic_iow(xl, XRNIC_MAC_DEST_ADDR_HI(qp->hw_qpn), val);
		wmb();
	}

	if (params->flags & XIB_MODIFY_QP_AV) {
		u32 adv_conf;
		u32 pkey = 0xffff;
		qp->is_ipv6 = (params->ip_version == 6);
		if (qp->is_ipv6) {
			qp_set_ipv6_destination(xl, qp->hw_qpn, params->ipv6_addr);
		} else
			xrnic_iow(xl, XRNIC_IP_DEST_ADDR_1(qp->hw_qpn),
					ntohl(params->ip4_daddr));
		wmb();

		/* Update the IP address type in the QP */
		config = xrnic_ior(xl, XRNIC_QP_CONF(qp->hw_qpn));
		if (params->ip_version == 6)
			config |= QP_CONF_IPV6;
		else
			config &= ~(QP_CONF_IPV6);

		xrnic_iow(xl, XRNIC_QP_CONF(qp->hw_qpn), config);

		adv_conf = xrnic_ior(xl, XRNIC_QP_ADV_CONF(qp->hw_qpn));

		adv_conf &= ~(QP_ADV_TTL_MASK << QP_ADV_TTL_SHIFT);
		adv_conf |= params->hop_limit << QP_ADV_TTL_SHIFT;

		adv_conf &= ~QP_ADV_TC_MASK;
		adv_conf |= params->traffic_class;
		adv_conf |= (pkey << QP_ADV_PKEY_SHIFT);
		xrnic_iow(xl, XRNIC_QP_ADV_CONF(qp->hw_qpn),
				adv_conf);
		wmb();
	}

	if (params->flags & XIB_MODIFY_QP_TIMEOUT) {
		extern unsigned int retry_time;
		u32 cfg = 0;

		cfg |= retry_time; /* time out */
		cfg |= (params->retry_cnt << 8);
		cfg |= (params->rnr_retry_cnt << 11);
		cfg |= 0x10 << 16; /* nack retry timeout*/

		xrnic_iow(xl, XRNIC_TIMEOUT_CONF(qp->hw_qpn),
				cfg);
		wmb();
	}

	if (params->flags & XIB_MODIFY_QP_SQ_PSN) {
		dev_dbg(&xib->ib_dev.dev, "setting sq psn: %x\n", (params->sq_psn)&0xffffff);
		xrnic_iow(xl, XRNIC_SNDQ_PSN(qp->hw_qpn), (params->sq_psn) &
				0xffffff);
		wmb();
	}
	if (params->flags & XIB_MODIFY_QP_RQ_PSN) {
		u32 val = (params->rq_psn - 1) & 0xffffff;
		val |= (XRNIC_SEND_ONLY << 24);
		dev_dbg(&xib->ib_dev.dev, "setting rq psn: %x\n", (params->rq_psn) & 0xffffff);
		xrnic_iow(xl, XRNIC_LAST_RQ_PSN(qp->hw_qpn), val);
		wmb();
	}

	if ((params->flags & XIB_MODIFY_QP_STATE)) {
		if (params->qp_state == XIB_QP_STATE_INIT) {
			dev_dbg(&xib->ib_dev.dev, "setting state\n");
			config = (QP_ENABLE | QP_HW_HSK_DIS | QP_CQE_EN);

			if (qp->hw_qpn == 0)
				config |= QP_CQ_IRQ_EN;
			else {
				/* For user QPs, the libibverbs does a polling for
					pointers, so interrupts are not required for them */
				if (qp->qp_type == XIB_QP_TYPE_USER)
					config &= ~(QP_RQ_IRQ_EN | QP_CQ_IRQ_EN);
				else {
					config |= QP_CQ_IRQ_EN | QP_RQ_IRQ_EN;
				}
			}
			config |= (xib->mtu << QP_PMTU_SHIFT);
			/* rq buf size in multiple of 256 */
			config |= ((qp->rq_buf_size >> 8) << QP_RQ_BUF_SZ_SHIFT);
			xrnic_iow(xl, XRNIC_QP_CONF(qp->hw_qpn), config);
			wmb();
		}

		/* nothing special needed for other states(RTR/RTS) in HW? */
		qp->state = params->qp_state;
	}

	return 0;
}

/*
 *
 */
dma_addr_t xrnic_buf_alloc(struct xrnic_local *xl, u32 size, u32 count)
{
	int order;
	void *buf;
	dma_addr_t addr;

	order = get_order(PAGE_ALIGN(size * count));
	buf = (void *)__get_free_pages(GFP_KERNEL, order);
	if (!buf) {
		dev_err(&xl->pdev->dev, "failed to alloc xrnic buffers, order\
				:%d\n", order);
		return 0;
	}

	addr = dma_map_single(&xl->pdev->dev, buf,
				PAGE_ALIGN(size * count),
				DMA_FROM_DEVICE);

	if (dma_mapping_error(&xl->pdev->dev, addr)) {
		dev_err(&xl->pdev->dev, "failed to alloc xrnic buffers, order\
				:%d\n", order);
		return 0;
	}
	return addr;
}


/*
 *
 */
static void xrnic_set_bufs(struct platform_device *pdev, struct xrnic_local *xl)
{
	dma_addr_t addr;
	u32 val;

	addr = xrnic_buf_alloc(xl, XRNIC_SIZE_OF_ERROR_BUF, XRNIC_NUM_OF_ERROR_BUF);
	if (!addr) {
		dev_err(&pdev->dev, "xrnic_set_bufs: Failed to allocate err bufs\n");
		return;
	}
	xrnic_iow(xl, XRNIC_ERR_BUF_BASE_LSB, addr);
	xrnic_iow(xl, XRNIC_ERR_BUF_BASE_MSB, UPPER_32_BITS(addr));
	wmb();
	val = XRNIC_NUM_OF_ERROR_BUF | ( XRNIC_SIZE_OF_ERROR_BUF << 16);
	xrnic_iow(xl, XRNIC_ERR_BUF_SZ, val);
	wmb();

	addr = xrnic_buf_alloc(xl, PAGE_SIZE, 4);
	if (!addr) {
		dev_err(&pdev->dev, "xrnic_set_bufs: Failed to allocate incg pkt err bufs\n");
		return;
	}
	xrnic_iow(xl, XRNIC_INCG_PKT_ERRQ_BASE_LSB, addr);
	xrnic_iow(xl, XRNIC_INCG_PKT_ERRQ_BASE_MSB, UPPER_32_BITS(addr));
	wmb();
	xrnic_iow(xl, XRNIC_INCG_PKT_ERRQ_SZ, XRNIC_IN_ERRST_Q_NUM_ENTRIES);
	wmb();

	addr = xrnic_buf_alloc(xl, XRNIC_RESP_ERR_BUF_SIZE, XRNIC_RESP_ERR_BUF_DEPTH);
	xrnic_iow(xl, XRNIC_RSP_ERR_BUF_BA_LSB, addr);
	xrnic_iow(xl, XRNIC_RSP_ERR_BUF_BA_MSB, UPPER_32_BITS(addr));
	wmb();
	xrnic_iow(xl, XRNIC_RSP_ERR_BUF_DEPTH, (XRNIC_RESP_ERR_BUF_DEPTH << 16 | XRNIC_RESP_ERR_BUF_SIZE));
	wmb();
}

/*
 *
 */
int xrnic_start(struct xrnic_local *xl)
{
	u32 val;

#ifdef ERNIC_V4
	val = ( XRNIC_EN | (1 << 5) |
		((xl->udp_sport & XRNIC_UDP_SPORT_MASK) <<
		XRNIC_UDP_SPORT_SHIFT) );
	xrnic_iow(xl, XRNIC_CONF, val);
	val = xl->qps_enabled & XRNIC_NUM_QP_MASK ;
	xrnic_iow(xl, XRNIC_CONF_QP_EN, val);

#else
	val = ( XRNIC_EN | (1 << 5) |
		((xl->qps_enabled & XRNIC_NUM_QP_MASK) << XRNIC_NUM_QP_SHIFT) |
		((xl->udp_sport & XRNIC_UDP_SPORT_MASK) <<
		XRNIC_UDP_SPORT_SHIFT) );
	xrnic_iow(xl, XRNIC_CONF, val);
#endif
	return 0;
}

/*
 *
 */
u64 xrnic_get_sq_db_addr(struct xrnic_local *xl, int hw_qpn)
{
	/* reset the counter */
	*(xl->qp1_sq_db_v + hw_qpn) = 0;
	return (xl->qp1_sq_db_p + (hw_qpn * 4));
}
EXPORT_SYMBOL(xrnic_get_sq_db_addr);
/*
 *
 */
u64 xrnic_get_rq_db_addr(struct xrnic_local *xl, int hw_qpn)
{
	*(xl->qp1_rq_db_v + hw_qpn) = 0;
	return (xl->qp1_rq_db_p + (hw_qpn * 4));
}

/*
 *
 */
struct xrnic_local *xrnic_hw_init(struct platform_device *pdev, struct xilinx_ib_dev *xib)
{
	struct xrnic_local *xl;
	struct resource *res;
	u32 *db_buf, size;
	u64 db_buf_pa;
	int err;

	xl = kzalloc(sizeof(*xl), GFP_KERNEL);
	if (!xl) {
		dev_err(&pdev->dev, "memory alloc failed\n");
		return NULL;
	}

	xl->pdev = pdev;
	xl->retry_buf_va = NULL;
	xl->in_pkt_err_va = NULL;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res) {
		dev_err(&pdev->dev, "memory resource not found\n");
		goto fail;
	}
	xl->reg_base = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(xl->reg_base)) {
		dev_err(&pdev->dev, "cant ioreamp resource\n");
		goto fail;
	}

	/* store pa of reg for db access */
#ifdef ERNIC_V4
	xl->db_pa = (res->start + XRNIC_DB_PA_OFFSET);
	xl->db_size = (res->end - (res->start + XRNIC_DB_PA_OFFSET) + 1);
#else
	xl->db_pa = (res->start + XRNIC_DB_PA_OFFSET);
	xl->db_size = (res->end - (res->start + XRNIC_DB_PA_OFFSET) + 1);
#endif

	dev_dbg(&pdev->dev, "xl->reg_base: %lx\n", xl->reg_base);
	printk("xl->reg_base: %lx\n", xl->reg_base);

	xl->irq = platform_get_irq_byname(pdev, "xrnic_intr0");
	if (xl->irq <= 0) {
		dev_err(&pdev->dev, "platform get of irq failed!\n");
		goto fail;
	}

	/* set default hw config */
	xrnic_set_bufs(pdev, xl);

	/* enable all interrupts */
	xrnic_iow(xl, XRNIC_INT_EN, 0xff);

	/* allocate memory for rq and sq doorbells for all QPs */
	/* allocate 4 * qpn */
	size = ((xib->dev_attr.max_qp * 4) + (PAGE_SIZE - 1)) / PAGE_SIZE;
	size = size * PAGE_SIZE;
#ifdef CONFIG_MICROBLAZE
	db_buf = xib_alloc_coherent("ps", xib,
				size, &db_buf_pa, GFP_KERNEL);
#else
	db_buf = xib_alloc_coherent("pl", xl->xib,
				size, (dma_addr_t *)&db_buf_pa,
				GFP_KERNEL);
#endif
	if (!db_buf) {
		printk("failed to alloc db mem %s:%d\n",
							__func__, __LINE__);
		goto fail;
	}

	xl->qp1_rq_db_p = db_buf_pa;
	xl->qp1_rq_db_v = db_buf;

	db_buf = dma_alloc_coherent(&pdev->dev, size, (dma_addr_t *)&db_buf_pa,
			GFP_KERNEL);
	if (!db_buf) {
		dev_err(&pdev->dev, "failed to alloc db mem\n");
		goto fail;
	}

	xl->cq_rq_db_size = size;
	/* 512 bytes for all DBs for all QPs? TODO */
	xl->qp1_sq_db_v = db_buf;
	xl->qp1_sq_db_p = db_buf_pa;


	return xl;
fail:
	kfree(xl);
	return NULL;

}

int xrnic_reg_mr(struct xilinx_ib_dev *xib, u64 va, u64 len,
		u64 *pbl_tbl, int umem_pgs, int pdn, u32 mr_idx, u8 rkey)
{
	struct xrnic_local *xl = xib->xl;
	int i;
	u64 pa = pbl_tbl[0];
	u32 value = 0;

//	dev_dbg(&xib->ib_dev.dev, "%s: pbl_tbl: %px \n", __func__, pbl_tbl);

	for(i = 0; i < umem_pgs; i++)
		dev_dbg(&xib->ib_dev.dev, "pbl_tbl[%d]: %lx \n", i, pbl_tbl[i]);
	xrnic_iow(xl, XRNIC_MR_PDNUM(mr_idx), pdn);
	xrnic_iow(xl, XRNIC_MR_VA_LO(mr_idx), (va & 0xffffffff));
	xrnic_iow(xl, XRNIC_MR_VA_HI(mr_idx), (va >> 32) & 0xFFFFFFFF);
	wmb();

	xrnic_iow(xl, XRNIC_MR_BUF_BASE_LO(mr_idx), pa & 0xffffffff);
	xrnic_iow(xl, XRNIC_MR_BUF_BASE_HI(mr_idx), ((pa >> 32) & 0xFFFFFFFF));
	wmb();
	xrnic_iow(xl, XRNIC_MR_BUF_RKEY(mr_idx), rkey & XRNIC_BUF_RKEY_MASK);
	xrnic_iow(xl, XRNIC_MR_WRRD_BUF_LEN(mr_idx), (len & 0xffffffff));

	value = XRNIC_MR_ACC_DESC_RD_WR;
	value |= (len >> 32) << 16;
	xrnic_iow(xl, XRNIC_MR_ACC_DESC(mr_idx), value);
	wmb();

	return 0;
}

int xrnic_unreg_mr(struct xilinx_ib_dev *xib, u32 data)
{
	struct xrnic_local *xl = xib->xl;
	u32 mr_idx = data >> 8;

	xrnic_iow(xl, XRNIC_MR_BUF_RKEY(mr_idx), 0x0);
	wmb();

	xrnic_iow(xl, XRNIC_MR_ACC_DESC(mr_idx), 0xF);
	wmb();

	xib_bmap_release_id(&xib->mr_map, mr_idx);
	return 0;
}
/*
 *
 */
int xrnic_qp_set_pd(struct xilinx_ib_dev *xib, int qpn, int pdn)
{
	struct xrnic_local *xl = xib->xl;

	xrnic_iow(xl, XRNIC_QP_PD_NUM(qpn), pdn);

	return 0;
}

/*
 *
 */
int xrnic_set_pd(struct xilinx_ib_dev *xib,  int pdn)
{
	struct xrnic_local *xl = xib->xl;

	xrnic_iow(xl, XRNIC_MR_PDNUM(pdn), pdn);

	return 0;
}

/*
 *
 */
void xrnic_hw_deinit(struct xilinx_ib_dev *xib)
{
	struct xrnic_local *xl = xib->xl;

	xib_free_coherent(xib, PAGE_SIZE, xl->qp1_sq_db_v,
			xl->qp1_sq_db_p);
	xib_free_coherent(xib, PAGE_SIZE, xl->qp1_rq_db_v,
			xl->qp1_rq_db_p);
	free_irq(xl->irq, xib);
	kfree(xl);
}
