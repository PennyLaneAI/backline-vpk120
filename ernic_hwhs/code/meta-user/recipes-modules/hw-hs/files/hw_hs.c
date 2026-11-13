// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * AMD ERNIC hardware handshake driver
 *
 * Copyright (C) 2024 AMD, Inc. All rights reserved.
 * Copyright 2026 Xanadu Quantum Technologies Inc.
 *
 * Author : Anjaneyulu Reddy Mule <anjaneyu@xilinx.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation and may be copied,
 * distributed, and modified under those terms.
 *
 * NOTE: this driver targets the different hw_handshake controller now. Not the legacy one anymore.
 *       But the HW-handshake QP setup (setup_qps, via the ERNIC reg_base) is unchanged for setting
 *       up the QP and ERNIC.
 */
#include "hw_hs.h"

#include "hw_hs_ioctl.h"
#include "rnic.h"
#include "xib_export.h"

#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/in.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/ioctl.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/mutex.h>
#include <linux/of_address.h>
#include <linux/of_platform.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/uaccess.h>
#include <linux/version.h>

#define DEV_MINOR_BASE 0
#define DEVS_CNT 1
#define DEV_NAME "xib"

struct hw_hs_dev *dev;

static int hh_dev_open(struct inode *, struct file *);
static ssize_t hh_dev_read(struct file *, char *, size_t, loff_t *);
static ssize_t hh_dev_write(struct file *, const char *, size_t, loff_t *);
static int hh_dev_close(struct inode *, struct file *);
static long hh_dev_ioctl(struct file *file, unsigned int cmd, unsigned long arg);
static int hh_dev_mmap(struct file *file, struct vm_area_struct *vma);

struct file_operations fops = {
    .write = hh_dev_write,
    .read = hh_dev_read,
    .unlocked_ioctl = hh_dev_ioctl,
    .mmap = hh_dev_mmap,
    .release = hh_dev_close,
    .open = hh_dev_open,
};

// Map the engine's 4 KB AXI4-Lite control/status window into userspace
static int hh_dev_mmap(struct file *fp, struct vm_area_struct *vma)
{
    size_t size = vma->vm_end - vma->vm_start;

    (void)fp;
    if (!dev) {
        return -ENODEV;
    }
    if (vma->vm_pgoff != 0 || size > HH_REG_WIN_SIZE) {
        return -EINVAL;
    }

    vma->vm_page_prot = pgprot_noncached(vma->vm_page_prot);
    return io_remap_pfn_range(vma, vma->vm_start, dev->cfg.base >> PAGE_SHIFT, size,
                              vma->vm_page_prot);
}

static inline void xib_iow32(u8 __iomem *base, off_t offset, u32 value)
{
    iowrite32(value, base + offset);
    wmb();
}

static inline u32 xib_ior32(u8 __iomem *base, off_t offset)
{
    u32 val;
    val = ioread32(base + offset);
    rmb();
    return val;
}

int setup_qps(struct xrnic_local *xl, u32 hh_state, u32 qp_base, u32 qp_cnt, u32 q_depth)
{
    u32 i, qp, val;
    u64 db_addr;
    u8 __iomem *base = xl->reg_base;

    /* 1. Enable SW override */
    val = xib_ior32(base, XRNIC_ADV_CONF);
    val |= (XRNIC_SW_OVER_RIDE_EN << XRNIC_SW_OVER_RIDE_BIT);
    xib_iow32(base, XRNIC_ADV_CONF, val);
    for (i = 0; i < qp_cnt; i++) {
        qp = qp_base + i; // physical qp number
        /* 2. Reset SQ PI & the Cur SQ Ptr */
        /* Set data QPs SQ PI to 0. The XRNIC_SQ_PROD_IDX, gives
                0x20238, when 0 is passed as arg */
        xib_iow32(base, XRNIC_SQ_PROD_IDX(qp), 0);
        xib_iow32(base, XRNIC_STAT_CUR_SQ_PTR(qp), 0);
        /* 3. Reset CQ Head Ptr */
        xib_iow32(base, XRNIC_CQ_HEAD_PTR(qp), 0);
        if (hh_state == HH_EN_STATE) {
            /* 4. Write CQ DB with QP Num */
            xib_iow32(base, XRNIC_CQ_DB_ADDR_LSB(qp), qp + 1);
            if (dev->addr_width == ADDRW_64BIT) {
                xib_iow32(base, XRNIC_CQ_DB_ADDR_MSB(qp), (qp + 1) >> 32);
                wmb();
            }
            /* 5. QP SQ depth must match the HH SQ depth */
            val = xib_ior32(base, XRNIC_QUEUE_DEPTH(qp));
            /* First 16 bits are SQ Depth */
            val &= ~(0xFFFF);
            val |= (q_depth & 0xFFFF);
            xib_iow32(base, XRNIC_QUEUE_DEPTH(qp), val);
            /* 6. Enable HW HSK*/
            val = xib_ior32(base, XRNIC_QP_CONF(qp));
            val &= ~(QP_HW_HSK_DIS);
            val &= ~(QP_CQE_EN);
            xib_iow32(base, XRNIC_QP_CONF(qp), val);
        }
        else {
            val = xib_ior32(base, XRNIC_QUEUE_DEPTH(qp));
            val &= ~(0xFFFF);
            val |= get_user_sq_depth(qp);
            xib_iow32(base, XRNIC_QUEUE_DEPTH(qp), val);

            /* reset used memory */
            db_addr = xrnic_get_sq_db_addr(xl, qp);
            /* 4. program the CQ DBs back */
            xib_iow32(base, XRNIC_CQ_DB_ADDR_LSB(qp), db_addr);
            if (dev->addr_width == ADDRW_64BIT) {
                xib_iow32(base, XRNIC_CQ_DB_ADDR_MSB(qp), db_addr >> 32);
                wmb();
            }
            val = xib_ior32(base, XRNIC_QP_CONF(qp));
            val |= QP_HW_HSK_DIS;
            xib_iow32(base, XRNIC_QP_CONF(qp), val);
        }
    }

    /* Disable SW Override */
    val = xib_ior32(base, XRNIC_ADV_CONF);
    val &= ~(XRNIC_SW_OVER_RIDE_EN << XRNIC_SW_OVER_RIDE_BIT);
    xib_iow32(base, XRNIC_ADV_CONF, val);
    return 0;
}

/* ---------------------------------------------------------------------------
 * hw_handshake programming model
 * ------------------------------------------------------------------------- */

// Commit a QP-context into the engine's QP-context table
static int hw_hs_qpctx_commit(struct hw_hs_dev *d, struct hh_qpctx_cfg *qc)
{
    u8 __iomem *b = d->reg_base;
    struct xrnic_local *xl = (struct xrnic_local *)get_xrnic_local();
    u32 depth = qc->sq_depth ? qc->sq_depth : 1;

    if (qc->ctx_idx >= HH_NUM_CTX) {
        return -EINVAL;
    }

    if (qc->flags & HH_FLAG_REPLY_POLL) {
        u32 slot_bytes;

        if (qc->reply_stride_log2 >= 32) {
            pr_err("hw_hs[qpctx] reply_stride_log2=%u out of range\n", qc->reply_stride_log2);
            return -EINVAL;
        }
        slot_bytes = 1u << qc->reply_stride_log2;
        if (qc->reply_seq_off == 0 || (qc->reply_seq_off & 3) ||
            (u64)qc->reply_seq_off + 4 > (u64)slot_bytes) {
            pr_err("hw_hs[qpctx] reply_seq_off=%u invalid for a %u-byte slot; must be seq_num's "
                   "byte offset: 4-byte aligned, 0 < off, off + 4 <= slot\n",
                   qc->reply_seq_off, slot_bytes);
            return -EINVAL;
        }
    }

    // 1. stage the QP-context set and commit it atomically into QP-context[ctx]
    xib_iow32(b, HH_REG_QP_NUM, qc->qp_num);
    xib_iow32(b, HH_REG_SQBASE_LO, (u32)qc->sq_base);
    xib_iow32(b, HH_REG_SQBASE_HI, (u32)(qc->sq_base >> 32));
    xib_iow32(b, HH_REG_SQ_DEPTH, depth);
    xib_iow32(b, HH_REG_REPLY_BUF_LO, (u32)qc->reply_buf);
    xib_iow32(b, HH_REG_REPLY_BUF_HI, (u32)(qc->reply_buf >> 32));
    xib_iow32(b, HH_REG_FLAGS, qc->flags);
    xib_iow32(b, HH_REG_REPLY_TO, qc->reply_to);
    xib_iow32(b, HH_REG_REPLY_SEQ_OFF, qc->reply_seq_off);
    xib_iow32(b, HH_REG_REPLY_RING, qc->reply_stride_log2 & 0x3F);
    xib_iow32(b, HH_REG_QPCTX_COMMIT, qc->ctx_idx | (qc->rebind ? HH_QPCTX_REBIND : 0));

    // 2. ERNIC side: point that QP's SQ ring at sq_base and enable HW-HS
    // Skip if not rebind (config-only). Otherwise, the Engine's sq_pi will desync with the ERNIC
    // QP's SQPI.
    if (xl && qc->rebind) {
        xib_iow32(xl->reg_base, XRNIC_SNDQ_BUF_BASE_LSB(qc->qp_num), (u32)qc->sq_base);
        if (d->addr_width == ADDRW_64BIT) {
            xib_iow32(xl->reg_base, XRNIC_SNDQ_BUF_BASE_MSB(qc->qp_num), (u32)(qc->sq_base >> 32));
        }
        setup_qps(xl, HH_EN_STATE, qc->qp_num, 1, depth);
    }

    d->ctx[qc->ctx_idx].qp_num = qc->qp_num;
    d->ctx[qc->ctx_idx].sq_depth = depth;
    d->ctx[qc->ctx_idx].valid = true;
    return 0;
}

// rebase the MR buffer to the BRAM_1
static int hw_hs_mr_rebase(struct hw_hs_dev *d, struct hh_mr_rebase *r)
{
    struct xrnic_local *xl = (struct xrnic_local *)get_xrnic_local();
    u32 mr_idx = r->rkey >> 8;

    if (!xl) {
        printk("hw_hs: mr_rebase: no xrnic_local\n");
        return -ENODEV;
    }
    printk("hw_hs: mr_rebase rkey=%#x mr_idx=%u bram_pa=%#llx reg_base=%px -> BUF_BASE_LO@%#lx\n",
           r->rkey, mr_idx, (unsigned long long)r->bram_pa, xl->reg_base,
           (unsigned long)XRNIC_MR_BUF_BASE_LO(mr_idx));
    xib_iow32(xl->reg_base, XRNIC_MR_BUF_BASE_LO(mr_idx), (u32)r->bram_pa);
    if (d->addr_width == ADDRW_64BIT) {
        xib_iow32(xl->reg_base, XRNIC_MR_BUF_BASE_HI(mr_idx), (u32)(r->bram_pa >> 32));
    }
    wmb();
    printk("hw_hs: mr_rebase done\n");
    return 0;
}

// program the scalar round config, then bind {qp_num, sq_base, sq_depth} into QP-context 0
// (the convenience path). The WQE itself is built separately via HH_WQE_COMMIT.
static int hw_hs_config(struct hw_hs_dev *d, struct hh_round_cfg *c)
{
    u8 __iomem *b = d->reg_base;
    struct hh_qpctx_cfg qc0;
    int ret;

    // round scalars (engine)
    xib_iow32(b, HH_REG_PAY_LEN, c->payload_len);
    xib_iow32(b, HH_REG_PATTERN, c->pattern);
    xib_iow32(b, HH_REG_DBUF_LO, (u32)c->data_buf);
    xib_iow32(b, HH_REG_DBUF_HI, (u32)(c->data_buf >> 32));
    xib_iow32(b, HH_REG_TIMER_QP_SEL, c->timer_qp_sel);
    xib_iow32(b, HH_REG_RUN_WQE_IDX, c->run_wqe_idx);

    // Soft-reset the engine
    xib_iow32(b, HH_REG_CTRL, HH_CTRL_SOFT_RESET);
    xib_iow32(b, HH_REG_CTRL, 0);

    // bind SQ<->QP into QP-context
    qc0.ctx_idx = 0;
    qc0.qp_num = c->qp_num;
    qc0.sq_base = c->sq_base;
    qc0.sq_depth = c->sq_depth;
    qc0.reply_buf = c->reply_buf; // ctx0's per-QP reply buffer
    qc0.flags = c->flags;         // engine-global + ctx0 completion bits
    qc0.reply_to = c->reply_to;
    qc0.reply_seq_off = c->reply_seq_off;
    qc0.reply_stride_log2 = c->reply_stride_log2;
    qc0.rebind = 1; // HH_CONFIG is a fresh arm
    ret = hw_hs_qpctx_commit(d, &qc0);
    if (ret) {
        return ret;
    }

    d->rcfg = *c;
    return 0;
}

// Assemble one WQE from fields and store it at slot idx
static int hw_hs_wqe_commit(struct hw_hs_dev *d, struct hh_wqe_cfg *w)
{
    u8 __iomem *b = d->reg_base;

    xib_iow32(b, HH_REG_WQE_OPCODE, w->opcode & 0x3);
    xib_iow32(b, HH_REG_WQE_XFER_LEN, w->xfer_len);
    xib_iow32(b, HH_REG_WQE_RKEY, w->rkey);
    xib_iow32(b, HH_REG_WQE_VA_LO, w->va_lsb);
    xib_iow32(b, HH_REG_WQE_VA_HI, w->va_msb);
    xib_iow32(b, HH_REG_WQE_OFFS_LO, (u32)w->local_offset);
    xib_iow32(b, HH_REG_WQE_OFFS_HI, (u32)(w->local_offset >> 32));
    xib_iow32(b, HH_REG_WQE_WRID, w->wrid & 0xFFFF);

    // WQE ring config
    {
        u32 ring_cfg = (w->ring_en ? HH_RING_EN : 0) | ((w->stride_local_log2 & 0x3F) << 8) |
                       ((w->stride_remote_log2 & 0x3F) << 16);
        xib_iow32(b, HH_REG_WQE_QP_REF, w->qp_ref);
        xib_iow32(b, HH_REG_WQE_RING_CFG, ring_cfg);
        xib_iow32(b, HH_REG_WQE_RING_LEN, w->ring_len & 0xFFFF);
    }

    xib_iow32(b, HH_REG_WQE_COMMIT, w->idx); // strobes assemble -> RAM[idx]
    return 0;
}

// kick the controller for one round
static int hh_start(struct hw_hs_dev *d)
{
    xib_iow32(d->reg_base, HH_REG_CTRL, HH_CTRL_START); // W1P to start
    return 0;
}

static int hh_abort(struct hw_hs_dev *d)
{
    xib_iow32(d->reg_base, HH_REG_CTRL, HH_CTRL_ABORT);
    return 0;
}

// DEBUG purpose: dump the ERNIC per-QP SQ/CQ state
// Only use it when you find something wrong
static void hh_dump_qp_stat(struct hw_hs_dev *d)
{
    struct xrnic_local *xl = (struct xrnic_local *)get_xrnic_local();
    u8 __iomem *rb;
    u32 qp = d->rcfg.qp_num ? d->rcfg.qp_num : 1;
    u32 q;

    if (!xl) {
        return;
    }
    rb = xl->reg_base;
    (void)qp;
    for (q = 0; q <= 4; q++) {
        pr_err("hw_hs[qp%u] QP_CONF=0x%08x SQ_PI=%u CUR=%u DEST_QP=0x%08x SQ_PSN=0x%08x "
               "IPDST=0x%08x SNDQ=0x%08x_%08x STAT_WQE=0x%08x\n",
               q, xib_ior32(rb, XRNIC_QP_CONF(q)), xib_ior32(rb, XRNIC_SQ_PROD_IDX(q)),
               xib_ior32(rb, XRNIC_STAT_CUR_SQ_PTR(q)), xib_ior32(rb, XRNIC_DEST_QP_CONF(q)),
               xib_ior32(rb, XRNIC_SNDQ_PSN(q)), xib_ior32(rb, XRNIC_IP_DEST_ADDR_1(q)),
               xib_ior32(rb, XRNIC_SNDQ_BUF_BASE_MSB(q)), xib_ior32(rb, XRNIC_SNDQ_BUF_BASE_LSB(q)),
               xib_ior32(rb, XRNIC_STAT_WQE(q)));
    }
    pr_err("hw_hs[cfg] qp_num=%u sq_base=0x%llx data_buf=0x%llx addr_width=%u\n", d->rcfg.qp_num,
           (unsigned long long)d->rcfg.sq_base, (unsigned long long)d->rcfg.data_buf,
           d->addr_width);
    {
        u32 aq = d->rcfg.qp_num ? d->rcfg.qp_num : 1;
        pr_err("hw_hs[qp%u-l2] DEST_QP=%u(0x%x) MAC=%04x%08x\n", aq,
               xib_ior32(rb, XRNIC_DEST_QP_CONF(aq)), xib_ior32(rb, XRNIC_DEST_QP_CONF(aq)),
               xib_ior32(rb, XRNIC_MAC_DEST_ADDR_HI(aq)) & 0xffff,
               xib_ior32(rb, XRNIC_MAC_DEST_ADDR_LO(aq)));
    }
    pr_err("hw_hs[glb] INT_STAT=0x%08x INCG_NAK=%u OUTG_NAK=%u RETRY=0x%08x RESP_HDLR=0x%08x\n",
           xib_ior32(rb, XRNIC_INT_STAT), xib_ior32(rb, XRNIC_INCG_NAK_PKT_CNT),
           xib_ior32(rb, XRNIC_OUTG_NAK_PKT_CNT), xib_ior32(rb, XRNIC_RETRY_CNT_STAT),
           xib_ior32(rb, XRNIC_RESP_HDLR_STAT));
    pr_err("hw_hs[pkt] OUT_IO=0x%08x LST_OUT=0x%08x IN_ACKMAD=0x%08x LST_IN=0x%08x "
           "IN_ALLDROP=0x%08x\n",
           xib_ior32(rb, XRNIC_OUTG_SND_RDWR_PKT_CNT), xib_ior32(rb, XRNIC_LST_OUTG_PKT),
           xib_ior32(rb, XRNIC_INCG_ACK_MAD_PKT_CNT), xib_ior32(rb, XRNIC_LST_INCG_PKT),
           xib_ior32(rb, XRNIC_INCG_ALL_DRP_PKT_CNT));
}

// soft-reset the controller and restore the ERNIC QPs out of HW-HS
static int hh_reset(struct hw_hs_dev *d)
{
    struct xrnic_local *xl = (struct xrnic_local *)get_xrnic_local();
    u32 i = 0;
    u32 any = 0;

    hh_dump_qp_stat(d);
    xib_iow32(d->reg_base, HH_REG_CTRL, HH_CTRL_SOFT_RESET);

    // Take every committed QP context out of HW-HS
    for (i = 0; i < HH_NUM_CTX; i++) {
        if (d->ctx[i].valid) {
            setup_qps(xl, HH_DIS_STATE, d->ctx[i].qp_num, 1,
                      d->ctx[i].sq_depth ? d->ctx[i].sq_depth : 1);
            d->ctx[i].valid = false;
            any = 1;
        }
    }
    if (!any) {
        u32 qp_cnt = d->rcfg.qp_cnt ? d->rcfg.qp_cnt : 1;
        u32 depth = d->rcfg.sq_depth ? d->rcfg.sq_depth : 1;
        u32 qp_base = d->rcfg.qp_num ? d->rcfg.qp_num : 1;
        setup_qps(xl, HH_DIS_STATE, qp_base, qp_cnt, depth);
    }
    return 0;
}

// Read status
static int hh_read_status(struct hw_hs_dev *d, void __user *ubuf)
{
    struct hh_status_rd s;
    u8 __iomem *b = d->reg_base;
    static unsigned long __maybe_unused dbg_last;

    memset(&s, 0, sizeof(s));

    s.status = xib_ior32(b, HH_REG_STATUS);
    s.round_cnt = xib_ior32(b, HH_REG_ROUND_CNT);
    s.rtt = ((u64)xib_ior32(b, HH_REG_RTT_HI) << 32) | xib_ior32(b, HH_REG_RTT_LO);
    s.last_rd = xib_ior32(b, HH_REG_LAST_RD);

    // uncomment if you find something wrong
    // if (!dbg_last || time_after(jiffies, dbg_last + HZ)) {
    //     dbg_last = jiffies;
    //     pr_err("hw_hs: STATUS=0x%03x round=%u rtt=%llu\n", s.status, s.round_cnt,
    //            (unsigned long long)s.rtt);
    //     hh_dump_qp_stat(d);
    // }

    if (copy_to_user(ubuf, &s, sizeof(s))) {
        dev_err(NULL, "hw_hs: failed to copy status to user\n");
        return -EFAULT;
    }
    return 0;
}

static int hh_set_pattern(struct hw_hs_dev *d, unsigned long arg)
{
    u32 pat;
    if (copy_from_user(&pat, (const void __user *)arg, sizeof(pat))) {
        return -EFAULT;
    }
    xib_iow32(d->reg_base, HH_REG_PATTERN, pat);
    d->rcfg.pattern = pat;
    return 0;
}

static int hh_set_wqe_idx(struct hw_hs_dev *d, unsigned long arg)
{
    u32 idx;
    if (copy_from_user(&idx, (const void __user *)arg, sizeof(idx))) {
        return -EFAULT;
    }
    xib_iow32(d->reg_base, HH_REG_RUN_WQE_IDX, idx);
    d->rcfg.run_wqe_idx = idx;
    return 0;
}

// ---------------------------------------------------------------------------
// transport demo
//
// FREQ/CMD_CNT/DEPTH are sampled continuously by transport_demo,
// so they must be settled before the first DEMO_CTRL[0] write
// ---------------------------------------------------------------------------
static int hh_demo_configure(struct hw_hs_dev *d, const struct hh_demo_cfg *c)
{
    u8 __iomem *b = d->reg_base;

    if (!c->syn_depth || (c->syn_depth & 63u)) {
        dev_err(d->dev, "hw_hs: demo syn_depth %u must be a non-zero multiple of 64\n",
                c->syn_depth);
        return -EINVAL;
    }
    if (!c->cmd_cnt) {
        dev_err(d->dev, "hw_hs: demo cmd_cnt must be non-zero\n");
        return -EINVAL;
    }

    if (c->freq_span & (c->freq_span + 1u)) {
        dev_err(d->dev, "hw_hs: demo freq_span 0x%x must be 2^N-1 (all ones)\n", c->freq_span);
        return -EINVAL;
    }

    xib_iow32(b, HH_REG_FREQ_LO, (u32)(c->freq_num & 0xFFFFFFFFull));
    xib_iow32(b, HH_REG_FREQ_HI, (u32)(c->freq_num >> 32));
    xib_iow32(b, HH_REG_CMD_CNT, c->cmd_cnt);
    xib_iow32(b, HH_REG_DEMO_DEPTH, c->syn_depth);
    xib_iow32(b, HH_REG_FREQ_SPAN, c->freq_span);
    xib_iow32(b, HH_REG_LFSR_SEED, c->lfsr_seed);

    // reset
    xib_iow32(b, HH_REG_DEMO_CTRL, HH_DEMO_RESET);
    xib_iow32(b, HH_REG_DEMO_CTRL, 0);
    return 0;
}

// W1P: arms one round
static int hh_demo_start(struct hw_hs_dev *d)
{
    xib_iow32(d->reg_base, HH_REG_DEMO_CTRL, HH_DEMO_START);
    return 0;
}

static int hh_demo_stop(struct hw_hs_dev *d)
{
    xib_iow32(d->reg_base, HH_REG_DEMO_CTRL, HH_DEMO_RESET);
    xib_iow32(d->reg_base, HH_REG_DEMO_CTRL, 0);
    return 0;
}

// Clear the trace and arm it
static int hh_trace_arm(struct hw_hs_dev *d)
{
    u8 __iomem *b = d->reg_base;

    xib_iow32(b, HH_REG_TRACE_CTRL, HH_TRACE_CLEAR);
    xib_iow32(b, HH_REG_TRACE_CTRL, 0);
    return 0;
}

static int hh_trace_status(struct hw_hs_dev *d, void __user *ubuf)
{
    struct hh_trace_status s;
    u8 __iomem *b = d->reg_base;
    u32 sts;

    memset(&s, 0, sizeof(s));
    s.cnt = xib_ior32(b, HH_REG_TRACE_CNT);
    sts = xib_ior32(b, HH_REG_TRACE_STS);
    s.full = !!(sts & HH_TRACE_STS_FULL);
    s.sat = !!(sts & HH_TRACE_STS_SAT);

    if (copy_to_user(ubuf, &s, sizeof(s))) {
        return -EFAULT;
    }
    return 0;
}

static int hh_demo_status(struct hw_hs_dev *d, void __user *ubuf)
{
    struct hh_demo_status s;
    u8 __iomem *b = d->reg_base;

    memset(&s, 0, sizeof(s));
    s.done = xib_ior32(b, HH_REG_DEMO_STS) & HH_DEMO_STS_DONE;
    s.err_cnt = xib_ior32(b, HH_REG_DEMO_ERR);
    s.cmd_cnt = xib_ior32(b, HH_REG_CMD_CNT);
    s.syn_depth = xib_ior32(b, HH_REG_DEMO_DEPTH);
    s.freq_num = ((u64)xib_ior32(b, HH_REG_FREQ_HI) << 32) | xib_ior32(b, HH_REG_FREQ_LO);

    if (copy_to_user(ubuf, &s, sizeof(s))) {
        return -EFAULT;
    }
    return 0;
}

static long hh_dev_ioctl(struct file *fp, unsigned int cmd, unsigned long arg)
{
    int err = 0;
    struct hw_hs_dev *d = fp->private_data;
    struct hh_round_cfg rcfg;
    struct hh_wqe_cfg wcfg;
    struct hh_qpctx_cfg qcfg;
    struct hh_mr_rebase mrb;
    struct hh_demo_cfg dcfg;

    if (_IOC_TYPE(cmd) != HH_MAGIC_NUM) {
        return -ENOTTY;
    }

    if (_IOC_NR(cmd) >= MAX_HW_HSK_FNS) {
        return -ENOTTY;
    }

    if (_IOC_DIR(cmd) & (_IOC_READ | _IOC_WRITE)) {
        if (!access_ok((void __user *)arg, _IOC_SIZE(cmd))) {
            return -EFAULT;
        }
    }

    switch (cmd) {
    case HH_CONFIG:
        if (copy_from_user(&rcfg, (const void __user *)arg, sizeof(rcfg))) {
            return -EFAULT;
        }
        err = hw_hs_config(d, &rcfg);
        break;
    case HH_WQE_COMMIT:
        if (copy_from_user(&wcfg, (const void __user *)arg, sizeof(wcfg))) {
            return -EFAULT;
        }
        err = hw_hs_wqe_commit(d, &wcfg);
        break;
    case HH_START:
        err = hh_start(d);
        break;
    case HH_ABORT:
        err = hh_abort(d);
        break;
    case HH_RESET:
        err = hh_reset(d);
        break;
    case HH_READ_STATUS:
        err = hh_read_status(d, (void __user *)arg);
        break;
    case HH_SET_PATTERN:
        err = hh_set_pattern(d, arg);
        break;
    case HH_SET_WQE_IDX:
        err = hh_set_wqe_idx(d, arg);
        break;
    case HH_QPCTX_COMMIT:
        if (copy_from_user(&qcfg, (const void __user *)arg, sizeof(qcfg))) {
            return -EFAULT;
        }
        err = hw_hs_qpctx_commit(d, &qcfg);
        break;
    case HH_MR_REBASE:
        if (copy_from_user(&mrb, (const void __user *)arg, sizeof(mrb))) {
            return -EFAULT;
        }
        err = hw_hs_mr_rebase(d, &mrb);
        break;
    case HH_DEMO_CFG:
        if (copy_from_user(&dcfg, (const void __user *)arg, sizeof(dcfg))) {
            return -EFAULT;
        }
        err = hh_demo_configure(d, &dcfg);
        break;
    case HH_DEMO_START_RUN:
        err = hh_demo_start(d);
        break;
    case HH_DEMO_STOP_RUN:
        err = hh_demo_stop(d);
        break;
    case HH_TRACE_ARM:
        err = hh_trace_arm(d);
        break;
    case HH_TRACE_STATUS:
        err = hh_trace_status(d, (void __user *)arg);
        break;
    case HH_DEMO_STATUS:
        err = hh_demo_status(d, (void __user *)arg);
        break;
    default:
        printk("hw_hs: invalid opcode %#x\n", cmd);
        err = -ENOTTY;
        break;
    }
    return err;
}

int hh_dev_close(struct inode *inode, struct file *fp)
{
    int err = hh_reset(dev);
    if (mutex_is_locked(&dev->mutx)) {
        mutex_unlock(&dev->mutx);
    }
    return err;
}

static int hh_dev_open(struct inode *inode, struct file *fp)
{
    if (!mutex_trylock(&dev->mutx)) {
        return -EBUSY;
    }
    fp->private_data = (void *)dev;
    return 0;
}

static ssize_t hh_dev_read(struct file *fp, char *buf, size_t len, loff_t *ofs)
{
    printk("Not implemented\n");
    return 0;
}

static ssize_t hh_dev_write(struct file *fp, const char *buf, size_t len, loff_t *ofs)
{
    printk("Not implemented\n");
    return 0;
}

static void __exit hh_dev_exit(void)
{
    if (dev->dev) {
        device_destroy(dev->cl, dev->dev_num);
    }
    if (dev->cl) {
        class_destroy(dev->cl);
    }
    if (dev->cdevice) {
        cdev_del(dev->cdevice);
    }
    unregister_chrdev_region(dev->dev_num, DEVS_CNT);
    if (dev->reg_base) {
        iounmap(dev->reg_base);
    }
    kfree(dev);
    return;
}

static int __init hh_dev_init(void)
{
    struct device_node *rnic_node, *hh_node;
    int err;
    struct resource resource;
    dev_t dev_num;
    struct cdev *cdevice = NULL;

    rnic_node = of_find_node_by_name(NULL, "ernic");
    if (!rnic_node) {
        dev_err(NULL, "Couldn't find rnic node\n");
        return -EFAULT;
    }

    hh_node = of_find_node_by_name(NULL, "hw_handshake");
    if (!hh_node) {
        dev_err(NULL, "Couldn't find hw hs node\n");
        return -EFAULT;
    }

    dev = kmalloc(sizeof(*dev), GFP_KERNEL);
    if (!dev) {
        dev_err(NULL, "Failed to allocate memory for dev\n");
        return -ENOMEM;
    }
    memset(dev, 0, sizeof(*dev));
    dev->cl = NULL;

    err = of_property_read_u32(rnic_node, "xlnx,addr-width", &dev->addr_width);
    if (err < 0) {
        dev_err(NULL, "Couldn't find address width\n");
        goto err1;
    }
    if (!(dev->addr_width == ADDRW_32BIT || dev->addr_width == ADDRW_64BIT)) {
        dev_err(NULL, "Address width is neither 32 nor 64\n");
        goto err1;
    }

    err = of_address_to_resource(hh_node, 0, &resource);
    if (err < 0) {
        dev_err(NULL, "HW handshake reg resources doesn't exist\n");
        goto err1;
    }
    dev->cfg.size = resource_size(&resource);
    dev->cfg.base = resource.start;

    err = of_property_read_u32(hh_node, "xlnx,num-qp", &dev->num_qp);
    if (err < 0) {
        dev_err(NULL, "Couldn't find number of QP HW handshake supports\n");
        goto err1;
    }
    pr_info("HW HS num-qp %d\n", dev->num_qp);

    // control/status window for the controller
    dev->reg_base = ioremap(dev->cfg.base, HH_REG_WIN_SIZE);
    if (!dev->reg_base) {
        dev_err(NULL, "Failed to remap hw handshake register window\n");
        goto err1;
    }

    err = alloc_chrdev_region(&dev_num, DEV_MINOR_BASE, DEVS_CNT, DEV_NAME);
    if (err) {
        dev_err(NULL, "Failed to allocate device numbers\n");
        goto err2;
    }

    cdevice = cdev_alloc();
    if (!cdevice) {
        dev_err(NULL, "Failed to allocate cdev\n");
        unregister_chrdev_region(dev_num, DEVS_CNT);
        goto err2;
    }
    dev->cdevice = cdevice;
    cdev_init(cdevice, &fops);
    cdevice->owner = THIS_MODULE;
    err = cdev_add(cdevice, dev_num, DEVS_CNT);
    if (err) {
        dev_err(NULL, "Failed to add cdev to VFS\n");
        unregister_chrdev_region(dev_num, DEVS_CNT);
        goto err2;
    }

    /* trigger udev */
    dev->cl = class_create("xib");
    if (!dev->cl) {
        dev_err(NULL, "Failed to create a dev class\n");
        cdev_del(cdevice);
        unregister_chrdev_region(dev_num, DEVS_CNT);
        goto err2;
    }

    dev->dev = device_create(dev->cl, NULL, dev_num, NULL, "%s%d", DEV_NAME, MINOR(dev_num));
    if (!dev->dev) {
        dev_err(NULL, "Failed to create device\n");
        class_destroy(dev->cl);
        cdev_del(cdevice);
        unregister_chrdev_region(dev_num, DEVS_CNT);
        goto err2;
    }

    dev->dev_num = dev_num;
    mutex_init(&dev->mutx);
    return 0;
err2:
    iounmap(dev->reg_base);
err1:
    kfree(dev);
    return -EFAULT;
}

module_init(hh_dev_init);
module_exit(hh_dev_exit);
MODULE_LICENSE("GPL");
