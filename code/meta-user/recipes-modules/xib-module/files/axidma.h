// SPDX-License-Identifier: GPL-2.0
/*
 * axidma.h - This is the header file for axi dma stream implementation
 * to get the Immediate and Rkey data
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

#ifndef _AXIDMA_H_
#define _AXIDMA_H_

#define XAXIDMA_RX_CR_OFFSET		0x00000030 /* Channel control */
#define XAXIDMA_RX_SR_OFFSET		0x00000034 /* Status */
#define XAXIDMA_RX_CDESC_OFFSET		0x00000038 /* Current descriptor pointer */
#define XAXIDMA_RX_TDESC_OFFSET		0x00000040 /* Tail descriptor pointer */

#define XAXIDMA_CR_RUNSTOP_MASK		0x00000001 /* Start/stop DMA channel */
#define XAXIDMA_CR_RESET_MASK		0x00000004 /* Reset DMA engine */

#define XAXIDMA_DELAY_MASK              0xFF000000 /* Delay timeout counter */
#define XAXIDMA_COALESCE_MASK           0x00FF0000 /* Coalesce counter */

#define XAXIDMA_DELAY_SHIFT             24
#define XAXIDMA_COALESCE_SHIFT          16

#define XAXIDMA_IRQ_IOC_MASK            0x00001000 /* Completion intr */
#define XAXIDMA_IRQ_DELAY_MASK          0x00002000 /* Delay interrupt */
#define XAXIDMA_IRQ_ERROR_MASK          0x00004000 /* Error interrupt */
#define XAXIDMA_IRQ_ALL_MASK            0x00007000 /* All interrupts */

#define XAXIDMA_DFT_RX_THRESHOLD        1
#define XAXIDMA_DFT_RX_WAITBOUND        254

#define XAXIDMA_BD_STS_COMPLETE_MASK    0x80000000 /* Completed */

struct axidma_bd {
	phys_addr_t next;       /* Physical address of next buffer descriptor */
#ifndef CONFIG_PHYS_ADDR_T_64BIT
	u32 reserved1;
#endif
	phys_addr_t phys;
#ifndef CONFIG_PHYS_ADDR_T_64BIT
	u32 reserved2;
#endif
	u32 reserved3;
	u32 reserved4;
	u32 cntrl;
	u32 status;
} __aligned(128);

struct rq_noti_buffer {
    u32     data;   /*Immediate or Rkey*/
#ifdef ERNIC_V4
    u16      type:5;
    u16      qpid:11;
#else
    u8     type;
    u8     qpid;
#endif
    u16	rq_pi_ptr;
};

struct axidma_q {
	void __iomem		*dma_regs;
	int			rx_irq;
	spinlock_t		rx_lock;             /* rx lock */

	/* Buffer descriptors */
	struct axidma_bd	*rx_bd_v;
	u64			rx_bd_p;
	u32			rx_bd_ci;
	struct rq_noti_buffer	**rq_noti_buf;
	unsigned long		rx_packets;
	unsigned long		rx_bytes;
};
#endif /*_AXIDMA_H_*/
