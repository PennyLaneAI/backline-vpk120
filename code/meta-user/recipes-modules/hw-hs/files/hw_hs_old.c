// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * AMD ERNIC hardware handshake driver
 *
 * Copyright (C) 2024 AMD, Inc. All rights reserved.
 *
 * Author : Anjaneyulu Reddy Mule <anjaneyu@xilinx.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation and may be copied,
 * distributed, and modified under those terms.
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/version.h>
#include <linux/moduleparam.h>
#include <linux/init.h>
#include <linux/errno.h>
#include <linux/slab.h>
#include <linux/err.h>
#include <linux/string.h>
#include <linux/in.h>
#include <linux/io.h>
#include <linux/device.h>
#include <linux/cdev.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/of_platform.h>
#include <linux/mutex.h>
#include <linux/of_address.h>
#include <linux/ioctl.h>
#include "hw_hs.h"
#include "hw_hs_ioctl.h"
#include "rnic.h"
#include "xib_export.h"

#define DEV_MINOR_BASE	0
#define DEVS_CNT	1
#define DEV_NAME	"xib"
#define BRAM_STR	"bram"
#define EXT_DDR_STR	"eddr"

struct hw_hs_dev *dev;
uint32_t config_qp_cnt = 0;
static int hh_dev_open (struct inode *, struct file * );
static ssize_t hh_dev_read (struct file *, char *, size_t , loff_t *);
static ssize_t hh_dev_write(struct file *, const char *, size_t , loff_t *);
static int hh_dev_close(struct inode *, struct file * );
static long hh_dev_ioctl(struct file *file, unsigned int cmd, unsigned long arg);

struct file_operations fops = {
	.write = hh_dev_write,
	.read = hh_dev_read,
	.unlocked_ioctl = hh_dev_ioctl,
	.release = hh_dev_close,
	.open = hh_dev_open,
};

int same_ba = 1;
module_param(same_ba, int, 0664);

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

int hw_hs_cmn_cfg(struct hw_hsk_ctrl_cfg *cfg, u8 __iomem *base, u8 __iomem *qp_base)
{
        unsigned int val = 0;

        if (!cfg)
                return -EFAULT;

        if (!cfg->queue_depth)
                cfg->queue_depth = 1;

        xib_iow32(base, HW_HS_Q_DEPTH_OFS, cfg->queue_depth);

        xib_iow32(base, HW_HS_WQE_CNT_OFS, cfg->qp_wqe_cnt);

        val = 0;
        if (cfg->burst_mode) {
                val |= (BURST_MODE_EN_VAL << HW_HS_MODE_BIT_POS);
        } else {
                val |= ((!BURST_MODE_EN_VAL) << HW_HS_MODE_BIT_POS);
        }

        if (cfg->data_pattern_en) {
                val |= (1 << DATA_PTRN_EN_BIT_POS);
        }

        if (!cfg->qp_cnt) {
                cfg->qp_cnt = 1;
	} else {
		/* QP0 is not present and QP1 is CM QP */
		if(cfg->qp_cnt > (dev->num_qp - 2)) {
			dev_err(NULL, "QP count %d is greater than"
					" the HW HS supports %d\n",
					cfg->qp_cnt, dev->num_qp - 2);
			return -EINVAL;
		}
	}

        cfg->qp_cnt &= ((1 << QP_CNT_BITS_CNT) - 1);
        val |= (cfg->qp_cnt << QPS_EN_BIT_POS);

        if (!cfg->db_burst_cnt)
                cfg->db_burst_cnt = 1;
        if (cfg->db_burst_cnt >= MAX_WQE_DEPTH)
                cfg->db_burst_cnt = (MAX_WQE_DEPTH - 1);

        cfg->db_burst_cnt &= ((1 << DB_BURST_CNT_BIT_CNT) - 1);
        val |= (cfg->db_burst_cnt << DB_BURSTS_CNT_BIT_POS);

#if 0
        val |= ((cfg->hw_hs_en & 1) << HW_HS_EN_BIT_POS);
#endif

        xib_iow32(base, HW_HS_CFG_REG_OFS, val);

	/* TODO Free this space on every reset */
	dev->qp_info = kmalloc(sizeof(*dev->qp_info) * cfg->qp_cnt, GFP_KERNEL);
	if (!dev->qp_info) {
		dev_err(NULL, "Failed to allocate space for qp info\n");
		return -ENOMEM;
	}
        return 0;
}

int hh_alloc_mem(u32 qp_num, u8 __iomem *base, struct hw_hs_info *hh_info,
				struct qp_info *qp_info, u64 data_addr, u64 sq_addr)
{
	struct xrnic_local *xl;
        u64 addr, size;
	int ret;
	struct hw_hsk_ctrl_cfg *cfg = &hh_info->ctrl_cfg;
	u32 mode = !cfg->burst_mode;

	/* Program the register */
	xib_iow32(base, HW_HS_DATA_BA_OFS_LSB(qp_num), data_addr);

	if (dev->addr_width == ADDRW_64BIT)
		xib_iow32(base, HW_HS_DATA_BA_OFS_MSB(qp_num), data_addr >> 32);

	/* write address to the ERNIC QP SQ BA, if it's in BRAM, if not the
		SQ BA in ERNIC QP would be used as is for HW HS*/
	xl = (struct xrnic_local *)get_xrnic_local();
	xib_iow32(xl->reg_base, XRNIC_SNDQ_BUF_BASE_LSB(qp_num + 1), sq_addr);
	if (dev->addr_width == ADDRW_64BIT)
		xib_iow32(xl->reg_base, XRNIC_SNDQ_BUF_BASE_MSB(qp_num + 1),\
				sq_addr >> 32);
	qp_info->sq_ba_p = addr;
	xib_iow32(base, HW_HS_QP_WQE_BA_LSB(qp_num), sq_addr);

	if (dev->addr_width == ADDRW_64BIT)
		xib_iow32(base, HW_HS_QP_WQE_BA_MSB(qp_num), sq_addr >> 32);

	config_qp_cnt++;
        return 0;
}

int hw_hs_qp_cfg(struct per_qp_hw_hsk_reg_info *cfg, u8 __iomem *base, struct hw_hs_info *hh_info,
		struct qp_info  *qp_info)
{
        int qp_num = 0, ret = 0;
        unsigned int val = 0;

        if (cfg->data_size_kb >= MIN_DATA_SIZ_IN_KB) {
                switch (cfg->data_size_kb) {
                case 4:
                        val = 0;
                        break;
                case 8:
                        val = 1;
                        break;
                case 16:
                        val = 2;
                        break;
                case 64:
                        val = 3;
                        break;
                case 256:
                        val = 4;
                        break;
                case 512:
                        val = 5;
                        break;
                case 1024:
                        val = 6;
                        break;
                case 2048:
                        val = 7;
                        break;
                case 4096:
			val = 8;
			break;
                case 8192:
			val = 9;
			break;
                default:
                        pr_err("Rx data transfer size [%d] in KB is not complying with spec\n",
                               cfg->data_size_kb);
                        pr_debug("Setting 4K as data size\n");
                        val = 0;
                        break;
                }
        } else {
                pr_debug(KERN_INFO "Data transfer size rx is < minimum data transfer size\n");
                pr_debug(KERN_INFO "Setting 4K as data size\n");
                val = 0;
        }

        val &= ((1 << VALID_DATA_TF_SIZ_BIT_CNT) - 1);
        unsigned int size_val = val;
        val = cfg->wqe_opcode & ((1 << HW_HS_OPCODE_BIT_CNT) - 1);
        val = val << VALID_DATA_TF_SIZ_BIT_CNT;
        val = val | size_val;

	qp_num = cfg->qp_num;
        xib_iow32(base, HW_HS_WQE_OPCODE_OFS(qp_num), val);

        /* program the registers */

	xib_iow32(base, HW_HS_DATA_PTRN_OFS(qp_num), cfg->data_pattern);
	xib_iow32(base, HW_HS_QP_VA_LSB(qp_num), cfg->va_lsb_val);
	xib_iow32(base, HW_HS_QP_VA_MSB(qp_num), cfg->va_msb_val);
	xib_iow32(base, HW_HS_QP_RKEY(qp_num), cfg->rkey);

        ret = hh_alloc_mem(qp_num, base, hh_info, qp_info, cfg->data_addr, cfg->sq_addr);
	return ret;
}

int setup_qps(struct xrnic_local *xl, u32 hh_state, u32 qp_cnt, u32 q_depth)
{
        u32 i, val;
        u64 db_addr;
	u8 __iomem *base = xl->reg_base;

        /* 1. Enable SW override */
        val = xib_ior32(base, XRNIC_ADV_CONF);
        val |= (XRNIC_SW_OVER_RIDE_EN << XRNIC_SW_OVER_RIDE_BIT);
        xib_iow32(base, XRNIC_ADV_CONF, val);
        for (i = 0; i < qp_cnt; i++) {
                /* 2. Reset SQ PI & the Cur SQ Ptr */
                /* Set data QPs SQ PI to 0. The XRNIC_SQ_PROD_IDX, gives
                        0x20238, when 0 is passed as arg */
                xib_iow32(base, XRNIC_SQ_PROD_IDX(i + 1), 0);
                xib_iow32(base, XRNIC_STAT_CUR_SQ_PTR(i + 1), 0);
                /* 3. Reset CQ Head Ptr */
                xib_iow32(base, XRNIC_CQ_HEAD_PTR(i + 1), 0);
                if (hh_state == HH_EN_STATE) {
                        /* 4. Write CQ DB with QP Num */
                        xib_iow32(base, XRNIC_CQ_DB_ADDR_LSB(i + 1), i + 2);
			if (dev->addr_width == ADDRW_64BIT) {
	                        xib_iow32(base, XRNIC_CQ_DB_ADDR_MSB(i + 1),
						(i + 2) >> 32);
				wmb();
			}
                        /* 5. QP SQ depth must be same as to the HH SQ depth */
                        val = xib_ior32(base, XRNIC_QUEUE_DEPTH(i + 1));
                        /* First 16 bits are SQ Depth */
			val &= ~(0xFFFF);
                        val |= (q_depth & 0xFFFF);

                        xib_iow32(base, XRNIC_QUEUE_DEPTH(i + 1), val);
                        /* 6. Enable HW HSK*/
                        val = xib_ior32(base, XRNIC_QP_CONF(i + 1));
                        val &= ~(QP_HW_HSK_DIS);
                        val &= ~(QP_CQE_EN);
                        xib_iow32(base, XRNIC_QP_CONF(i + 1), val);
                } else {
			val = xib_ior32(base, XRNIC_QUEUE_DEPTH(i + 1));
			val &= ~(0xFFFF);
			val |= get_user_sq_depth(i + 1);
			xib_iow32(base, XRNIC_QUEUE_DEPTH(i + 1), val);

			/* reset used memory */
                        db_addr = xrnic_get_sq_db_addr(xl, i + 1);
                        /* 4. program the CQ DBs back */
                        xib_iow32(base, XRNIC_CQ_DB_ADDR_LSB(i + 1), db_addr);
			if (dev->addr_width == ADDRW_64BIT) {
				xib_iow32(base, XRNIC_CQ_DB_ADDR_MSB(i + 1),
					db_addr >> 32);
				wmb();
			}
                        val = xib_ior32(base, XRNIC_QP_CONF(i + 1));
                        val |= QP_HW_HSK_DIS;
                        xib_iow32(base, XRNIC_QP_CONF(i + 1), val);
                }
        }

        /* Disable SW Override */
        val = xib_ior32(base, XRNIC_ADV_CONF);
        val &= ~(XRNIC_SW_OVER_RIDE_EN << XRNIC_SW_OVER_RIDE_BIT);
        xib_iow32(base, XRNIC_ADV_CONF, val);
        return 0;
}

int enable_hw_hsk(u32 state, u32 qp_cnt, u8 __iomem *base, u32 q_depth)
{
	struct xrnic_local *xl = (struct xrnic_local *)get_xrnic_local();
	int ret;
	u32 val;

	ret = setup_qps(xl, state, qp_cnt, q_depth);

	val = xib_ior32(base, HW_HS_CFG_REG_OFS);
	val |= (1 << HW_HS_EN_BIT_POS);
	xib_iow32(base, HW_HS_CFG_REG_OFS, val);
	return 0;
}

int reset_hw_hsk(struct hw_hs_info *hh_info, struct qp_info *qp_info, u8 __iomem *base)
{
        u64 size;
	struct hw_hsk_ctrl_cfg *cfg = &hh_info->ctrl_cfg;
        u32 val, i, mode = !cfg->burst_mode;
        void *xib = get_xib_ptr();
	struct xrnic_local *xl = (struct xrnic_local *)get_xrnic_local();

	hh_info->data_ba_rx = 0;
	hh_info->mem_info.used_mem = 0;

	setup_qps(xl, HH_DIS_STATE, config_qp_cnt, cfg->queue_depth);
	xib_iow32(base, HW_HS_TEST_STAT_OFS, HH_CLR_STAT_REG_VAL);
	config_qp_cnt = 0;
        return 0;
}

int read_perf_counters(struct hw_hs_dev *dev, struct perf_read_str *arg, u32 max_qp)
{
        int ret = 0;
	u32 data[2], cq_cnt;

	data[0] = xrnic_ior32(dev->qp_cfg_base, HW_HS_QP_PERF_CNT_LSB(arg->qp_num));
	data[1] = xrnic_ior32(dev->qp_cfg_base,
					HW_HS_QP_PERF_CNT_MSB(arg->qp_num));

	cq_cnt = xrnic_ior32(dev->qp_cfg_base, HW_HS_QP_PERF_BW(arg->qp_num));
	ret = copy_to_user((void __user *)&arg->clk_cnt, (void *)data,
				sizeof(data));
	if (ret) {
		pr_err("Failed to copy to user buffer\n");
		return -EFAULT;
	}
	ret = copy_to_user((void __user *)&arg->cq_cnt, (void *)&cq_cnt,
				sizeof(cq_cnt));
	if (ret) {
		pr_err("Failed to copy to user buffer\n");
		return -EFAULT;
	}
        return ret;
}

int read_hw_hsk_stat(void __iomem *base, void __user *buf)
{
        int ret = 0;
	u32 stat;

	stat = xib_ior32(base, HW_HS_TEST_STAT_OFS);
        ret = copy_to_user(buf, (const void *)&stat,
                           sizeof(stat));
        if (ret)
		dev_err(NULL, "Failed to copy to user buffer %d\n", __LINE__);
        u32 roi[4];

        roi[0] = xib_ior32(base, 0x0);
        roi[1] = xib_ior32(base, 0x1C);
        roi[2] = xib_ior32(base, 0x20);
        roi[3] = xib_ior32(base, 0x24);

        //printk("registers 0x0 [%x] 0x1C [%x] 0x20 [%x] 0x24 [%x]\n", roi[0], roi[1], roi[2], roi[3]);

        return ret;
}

static long hh_dev_ioctl(struct file *fp, unsigned int cmd, unsigned long arg)
{
	int err = 0;
	struct hw_hs_dev *dev = fp->private_data;
	struct per_qp_hw_hsk_reg_info qp_cfg;
	unsigned int rx_qp_cnt;


	if (_IOC_TYPE(cmd) != HH_MAGIC_NUM)
		return -ENOTTY;

	if (_IOC_NR(cmd) > MAX_HW_HSK_FNS)
		return -ENOTTY;

	if (_IOC_DIR(cmd) & _IOC_READ)
		err = !access_ok((void __user *)arg, _IOC_SIZE(cmd));
	else if(_IOC_DIR(cmd) & _IOC_WRITE)
		err = !access_ok((void __user *)arg, _IOC_SIZE(cmd));

	if (err)
		return -EFAULT;

	switch (cmd) {
	case HH_WRITE_CMN_CFG:
		err = copy_from_user(&dev->hh_info.ctrl_cfg, (const void __user *)arg,
				sizeof(dev->hh_info.ctrl_cfg));
		if (err) {
			printk("Couldn't copy %d bytes of %d bytes\n", err, sizeof(dev->hh_info.ctrl_cfg));
			return -EFAULT;
		}
		/* do hw hs common cfg reg writes */
		err = hw_hs_cmn_cfg(&dev->hh_info.ctrl_cfg, dev->cmn_cfg_base, dev->qp_cfg_base);
	break;
	case HH_WRITE_QP_CFG:
		err = copy_from_user(&qp_cfg, (const void __user *)arg,
				sizeof (qp_cfg));
		if (err) {
			printk("Couldn't copy %d bytes of %d bytes\n", err, sizeof(qp_cfg));
			return -EFAULT;
		}
		if (qp_cfg.qp_num > dev->hh_info.ctrl_cfg.qp_cnt) {
			dev_err(NULL, "Rx QP num [%d] is > max configured QPs[%d]\n",
				qp_cfg.qp_num, dev->hh_info.ctrl_cfg.qp_cnt);
			return -EFAULT;
		}
		if (!dev->qp_info) {
			dev_err(NULL, "qp list not allocated\n");
			return -EFAULT;
		}
		err = hw_hs_qp_cfg(&qp_cfg, dev->qp_cfg_base, &dev->hh_info,
				&dev->qp_info[qp_cfg.qp_num]);
	break;
	case HH_EN_OP:
		err = enable_hw_hsk(HH_EN_STATE, dev->hh_info.ctrl_cfg.qp_cnt, dev->cmn_cfg_base,
					dev->hh_info.ctrl_cfg.queue_depth);
	break;
	case HH_RESET_OP:
		err = reset_hw_hsk(&dev->hh_info, dev->qp_info, dev->cmn_cfg_base);
	break;
	case PERF_READ_OP:
		err = copy_from_user(&rx_qp_cnt, (const void __user *)arg, sizeof(rx_qp_cnt));
		if (err) {
			printk("Couldn't copy %d bytes of %d bytes\n", err, sizeof(rx_qp_cnt));
			return -EFAULT;
		}

		if (rx_qp_cnt > dev->hh_info.ctrl_cfg.qp_cnt) {
			dev_err(NULL, "Rx QP num [%d] is > max configured QPs[%d]\n",
				rx_qp_cnt, dev->hh_info.ctrl_cfg.qp_cnt);
			return -EFAULT;
		}
		err = read_perf_counters(dev, (struct perf_read_str *)arg,
					dev->hh_info.ctrl_cfg.qp_cnt);
	break;
	case TEST_DONE_READ_OP:
		err = read_hw_hsk_stat(dev->cmn_cfg_base, (void __user*)arg);
	break;
	default:
		printk("Rx invalid opcode %#x\n", cmd);
	break;
	}

	return err;
}

int hh_dev_close(struct inode *inode, struct file *fp)
{
	int err = reset_hw_hsk(&dev->hh_info, dev->qp_info, dev->cmn_cfg_base);
	if (mutex_is_locked(&dev->mutx))
		mutex_unlock(&dev->mutx);
	if (dev->qp_info)
		kfree(dev->qp_info);
	return err;
}

static int hh_dev_open(struct inode *inode, struct file *fp)
{
	if (!mutex_trylock(&dev->mutx))
		return -EBUSY;

	config_qp_cnt = 0;
	fp->private_data = (void *)dev;
	return 0;
}

static ssize_t hh_dev_read (struct file *fp, char *buf, size_t len, loff_t *ofs)
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
	if (dev->dev)
		device_destroy(dev->cl, dev->dev_num);
	if (dev->cl)
		class_destroy(dev->cl);
	if (dev->cdevice)
		cdev_del(dev->cdevice);
	unregister_chrdev_region(dev->dev_num, DEVS_CNT);
	kfree(dev);
	return;
}

static int __init hh_dev_init(void)
{
	struct device_node *rnic_node, *hh_node;
	struct hw_hs_info *hh_info;
	int err;
	struct resource resource;
	dev_t dev_num;
	struct class *cl;
	struct cdev *cdevice = NULL;

	rnic_node = of_find_node_by_name(NULL, "ernic");
	if (!rnic_node) {
		dev_err(NULL, "Couldn't find rnic node\n");
		return -EFAULT;
	}

	hh_node = of_find_node_by_name(NULL, "hw_handshake");
	if (!hh_node) {
		dev_err(NULL, "Couldn't hw hs node\n");
		return -EFAULT;
	}

	dev = kmalloc(sizeof *dev, GFP_KERNEL);
	if (!dev) {
		dev_err(NULL, "Failed to allocate memory for dev\n");
		return -ENOMEM;
	}
	dev->cl = NULL;
	hh_info = &dev->hh_info;
	memset(dev, 0, sizeof *dev);

	err = of_property_read_u32(rnic_node, "xlnx,addr-width", &dev->addr_width);
	if (err < 0) {
		dev_err(NULL, "Couldn't find address width\n");
		goto err1;
	}
	if(!(dev->addr_width == ADDRW_32BIT || dev->addr_width == ADDRW_64BIT)) {
		dev_err(NULL, "Address width is neither 32 nor 64\n");
		goto err1;
	}

	err = of_address_to_resource(hh_node, 0, &resource);
	if (err < 0) {
		dev_err(NULL, "HW handshake reg resources doesn't exist\n");
		goto err1;
	}

	hh_info->cfg.size = resource_size(&resource);
	hh_info->cfg.base = resource.start;
	hh_info->mem_info.used_mem = 0;

	err = of_property_read_u32(hh_node, "xlnx,num-qp", &dev->num_qp);
	if (err < 0) {
		dev_err(NULL, "Couldn't find number of QP HW handshake aupports\n");
		goto err1;
	}
	pr_info("HW HS num-qp %d\n", dev->num_qp);

	/* Remap
		1. HW HS global config space
		2. HW HS per QP config space
	*/

	dev->cmn_cfg_base = ioremap(hh_info->cfg.base, HS_HS_COMMON_CFG_SIZE);
	if (!dev->cmn_cfg_base) {
		dev_err(NULL, "Failed to remap hw handshake common config space\n");
		goto err2;
	}

	dev->qp_cfg_base = ioremap(hh_info->cfg.base + HW_HNDSHK_QP_CFG_BASE,
				dev->num_qp * HW_HS_QP_CFG_SIZE);
	if (!dev->qp_cfg_base) {
		dev_err(NULL, "Failed to remap hw handshake per QP config space\n");
		goto err3;
	}

	/* char device */
	err = alloc_chrdev_region(&dev_num, DEV_MINOR_BASE, DEVS_CNT, DEV_NAME);
	if (err) {
		dev_err(NULL, "Failed to allocate device numbers\n");
		goto err3;
	}

	cdevice = cdev_alloc();
	if (!cdevice) {
		dev_err(NULL, "Failed to allocate cdev\n");
		unregister_chrdev_region(dev_num, DEVS_CNT);
		goto err4;
	}

	dev->cdevice = cdevice;
	cdev_init(cdevice, &fops);
	cdevice->owner = THIS_MODULE;
	err = cdev_add(cdevice, dev_num, DEVS_CNT);
	if (err) {
		dev_err(NULL, "Failed to add cdev to VFS\n");
		unregister_chrdev_region(dev_num, DEVS_CNT);
		goto err4;
	}

	/* trigger udev */
	dev->cl = class_create("xib");
	if (!dev->cl) {
		dev_err(NULL, "Failed to create a dev class\n");
		cdev_del(cdevice);
		unregister_chrdev_region(dev_num, DEVS_CNT);
		goto err4;
	}

	dev->dev = NULL;
	dev->dev = device_create(dev->cl, NULL, dev_num, NULL, "%s%d", DEV_NAME, MINOR(dev_num));
	if (!dev->dev) {
		dev_err(NULL, "Failed to create device\n");
		class_destroy(dev->cl);
		cdev_del(cdevice);
		unregister_chrdev_region(dev_num, DEVS_CNT);
	}

	dev->dev_num = dev_num;
	dev->hh_info.data_ba_rx = 0;
	mutex_init(&dev->mutx);
	return 0;
err4:
	iounmap(dev->qp_cfg_base);
err3:
	iounmap(dev->cmn_cfg_base);
err2:
err1:
	kfree(dev);
	return -EFAULT;
}

module_init(hh_dev_init);
module_exit(hh_dev_exit);
MODULE_LICENSE("GPL");
