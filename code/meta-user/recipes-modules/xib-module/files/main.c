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
#include <linux/of_net.h>
#include <linux/module.h>
#include <linux/mm.h>
#include <rdma/ib_addr.h>
#include <rdma/ib_smi.h>
#include <rdma/ib_user_verbs.h>
#include <rdma/ib_cache.h>
#include <rdma/ib_umem.h>
#include <rdma/xib-abi.h>
#include <rdma/ib_verbs.h>
#include <rdma/uverbs_ioctl.h>
#include <net/addrconf.h>
#include <linux/of_address.h>
#include <linux/jiffies.h>
#include "xib.h"
#include "xib_kmm_export.h"
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
//#define DEBUG_IPV6

struct xilinx_ib_dev *ibdev;

static int handle_netdev_notifier(struct notifier_block *notifier,
                               unsigned long event, void *ptr);
struct notifier_block cmac_netdev_notifier = {
	.notifier_call = handle_netdev_notifier
};

static int cmac_inetaddr_event(struct notifier_block *notifier,
                               unsigned long event, void *data);
static int cmac_inet6addr_event(struct notifier_block *notifier,
                               unsigned long event, void *data);
struct notifier_block cmac_inetaddr_notifier = {
	.notifier_call = cmac_inetaddr_event
};

struct notifier_block cmac_inet6addr_notifier = {
        .notifier_call = cmac_inet6addr_event
};

char sq_mem[8] = "pl";
module_param_string(sq_mem, sq_mem, 8, 0644);
MODULE_PARM_DESC(rsq, "rdma_sq buffers from: \n"
		"bram\n"
		"pl\n"
		"ps\n");

char rq_mem[8] = "pl";
module_param_string(rq_mem, rq_mem, 8, 0644);
MODULE_PARM_DESC(rrq, "rdma_rq buffers from: \n"
		"bram\n"
		"pl\n"
		"ps\n");

static char cq_mem[8] = "pl";
module_param_string(cq_mem, cq_mem, 8, 0644);
MODULE_PARM_DESC(rcq, "rdma_cq buffers from: \n"
		"bram\n"
		"pl\n"
		"ps\n");

static char rtr_mem[8] = "pl";
module_param_string(rtr_mem, rtr_mem, 8, 0644);
MODULE_PARM_DESC(rtr_buf, "rdma_rtr_buf buffers from: \n"
		"bram\n"
		"pl\n"
		"ps\n");

unsigned int app_qp_cnt = 10;
module_param_named(max_app_qp, app_qp_cnt, int, 0644);
unsigned int retry_time = 0xD;
module_param_named(rtime, retry_time, int, 0644);

unsigned int app_qp_depth = 16;
module_param_named(max_q_depth, app_qp_depth, int, 0644);

unsigned int max_rq_sge = 16;
module_param(max_rq_sge, int, 0644);

static int check_qp_depths(unsigned int qp_depth)
{
	return (qp_depth && (!(qp_depth & (qp_depth-1)))) && (qp_depth >=2);
}

static ssize_t show_roce_pfc_enable(struct kobject *kobj,
		        struct kobj_attribute *attr, char *buf)
{
	if(xrnic_ior(ibdev->xl, XRNIC_PAUSE_CONF) &
			(1U << XRNIC_ROCE_PFC_EN_BIT))
		return sprintf(buf, "1\n");
	else
		return sprintf(buf, "0\n");
}

static ssize_t show_non_roce_pfc_enable(struct kobject *kobj,
		        struct kobj_attribute *attr, char *buf)
{
	if(xrnic_ior(ibdev->xl, XRNIC_PAUSE_CONF) &
			(1U << XRNIC_NON_ROCE_PFC_EN_BIT))
		return sprintf(buf, "1\n");
	else
		return sprintf(buf, "0\n");
}

static ssize_t store_roce_pfc_enable(struct kobject *kobj,
		        struct kobj_attribute *attr,
			const char *buf, size_t count)
{
	u32 val, en;
	val = xrnic_ior(ibdev->xl, XRNIC_PAUSE_CONF);
	en = simple_strtol(buf, NULL, 10);

	if (!en) {
		val &= ~(1U << XRNIC_ROCE_PFC_EN_BIT);
		xrnic_iow(ibdev->xl, XRNIC_PAUSE_CONF, val);
	} else if (en == 1) {
		val |= (1U << XRNIC_ROCE_PFC_EN_BIT);
		xrnic_iow(ibdev->xl, XRNIC_PAUSE_CONF, val);
	} else {
		pr_err("Error: Write 1 or 0 to enable/disable PFC.\n", val);
	}
	return count;
}

static ssize_t store_non_roce_pfc_enable(struct kobject *kobj,
		        struct kobj_attribute *attr,
			const char *buf, size_t count)
{
	u32 val, en;
	val = xrnic_ior(ibdev->xl, XRNIC_PAUSE_CONF);
	en = simple_strtol(buf, NULL, 10);

	if (!en) {
		val &= ~(1U << XRNIC_NON_ROCE_PFC_EN_BIT);
		xrnic_iow(ibdev->xl, XRNIC_PAUSE_CONF, val);
	} else if (en == 1) {
		val |= (1U << XRNIC_NON_ROCE_PFC_EN_BIT);
		xrnic_iow(ibdev->xl, XRNIC_PAUSE_CONF, val);
	} else {
		pr_err("Error: Write 1 or 0 to enable/disable PFC.\n", val);
	}
	return count;
}

static ssize_t show_roce_pfc_priority(struct kobject *kobj,
		        struct kobj_attribute *attr, char *buf)
{
	u32 val;
	val = xrnic_ior(ibdev->xl, XRNIC_PAUSE_CONF);

	return sprintf(buf, "%u\n", (val >> XRNIC_ROCE_PFC_PRIO_BIT) &
				XRNIC_PFC_PRIO_BIT_MASK);
}

static ssize_t show_non_roce_pfc_priority(struct kobject *kobj,
		        struct kobj_attribute *attr, char *buf)
{
	u32 val;
	val = xrnic_ior(ibdev->xl, XRNIC_PAUSE_CONF);

	return sprintf(buf, "%u\n", (val >> XRNIC_NON_ROCE_PFC_PRIO_BIT) &
				XRNIC_PFC_PRIO_BIT_MASK);
}

static ssize_t store_roce_pfc_priority(struct kobject *kobj,
			struct kobj_attribute *attr,
			const char *buf, size_t count)
{
	u32 val, prio;
	val = xrnic_ior(ibdev->xl, XRNIC_PAUSE_CONF);
	prio = simple_strtol(buf, NULL, 10);

	if (prio >= 0 && prio <= XRNIC_PFC_GLOBAL_PRIOIRTY) {
		val &= ~(XRNIC_PFC_PRIO_BIT_MASK << XRNIC_ROCE_PFC_PRIO_BIT);
		val |= (prio << XRNIC_ROCE_PFC_PRIO_BIT);
		xrnic_iow(ibdev->xl, XRNIC_PAUSE_CONF, val);
	} else {
		pr_err("Error: Priority value must be 0 to 8.\n");
	}
	return count;
}


static ssize_t store_non_roce_pfc_priority(struct kobject *kobj,
			struct kobj_attribute *attr,
			const char *buf, size_t count)
{
	u32 val, prio;
	val = xrnic_ior(ibdev->xl, XRNIC_PAUSE_CONF);
	prio = simple_strtol(buf, NULL, 10);

	if (prio >= 0 && prio <= XRNIC_PFC_GLOBAL_PRIOIRTY) {
		val &= ~(XRNIC_PFC_PRIO_BIT_MASK << XRNIC_NON_ROCE_PFC_PRIO_BIT);
		val |= (prio << XRNIC_NON_ROCE_PFC_PRIO_BIT);
		xrnic_iow(ibdev->xl, XRNIC_PAUSE_CONF, val);
	} else {
		pr_err("Error: Priority value must be 0 to 8.\n");
	}
	return count;
}

static ssize_t show_roce_pfc_xon(struct kobject *kobj,
		        struct kobj_attribute *attr, char *buf)
{
	u32 val;
	val = xrnic_ior(ibdev->xl, XRNIC_ROCE_PAUSE_OFFSET);
	return sprintf(buf, "%u\n", val & 0xFFFF);
}

static ssize_t show_non_roce_pfc_xon(struct kobject *kobj,
		        struct kobj_attribute *attr, char *buf)
{
	u32 val;
	val = xrnic_ior(ibdev->xl, XRNIC_NON_ROCE_PAUSE_OFFSET);
	return sprintf(buf, "%u\n", val & 0xFFFF);
}

static ssize_t store_roce_pfc_xon(struct kobject *kobj,
			struct kobj_attribute *attr,
			const char *buf, size_t count)
{
	u32 val, xon;
	val = xrnic_ior(ibdev->xl, XRNIC_ROCE_PAUSE_OFFSET);
	xon = simple_strtol(buf, NULL, 10);

	if (xon >= PFC_XON_XOFF_MIN && xon <= PFC_XON_XOFF_MAX) {
		xrnic_iow(ibdev->xl, XRNIC_ROCE_PAUSE_OFFSET, xon | (val & 0xFFFF0000));
	} else {
		pr_err("Error: XON threshold must be 0 to 512.\n", val);
	}
	return count;
}

static ssize_t store_non_roce_pfc_xon(struct kobject *kobj,
			struct kobj_attribute *attr,
			const char *buf, size_t count)
{
	u32 val, xon;
	val = xrnic_ior(ibdev->xl, XRNIC_NON_ROCE_PAUSE_OFFSET);
	xon = simple_strtol(buf, NULL, 10);

	if (xon >= PFC_XON_XOFF_MIN && xon <= PFC_XON_XOFF_MAX) {
		xrnic_iow(ibdev->xl, XRNIC_NON_ROCE_PAUSE_OFFSET, xon | (val & 0xFFFF0000));
	} else {
		pr_err("Error: XON threshold must be 0 to 512.\n", val);
	}
	return count;
}

static ssize_t show_non_roce_pfc_xoff(struct kobject *kobj,
		        struct kobj_attribute *attr, char *buf)
{
	u32 val;
	val = xrnic_ior(ibdev->xl, XRNIC_NON_ROCE_PAUSE_OFFSET);
	return sprintf(buf, "%u\n", val >> 16);
}

static ssize_t show_roce_pfc_xoff(struct kobject *kobj,
		        struct kobj_attribute *attr, char *buf)
{
	u32 val;
	val = xrnic_ior(ibdev->xl, XRNIC_ROCE_PAUSE_OFFSET);
	return sprintf(buf, "%u\n", val >> 16);
}

static ssize_t store_roce_pfc_xoff(struct kobject *kobj,
			struct kobj_attribute *attr,
			const char *buf, size_t count)
{
	u32 val, xoff;
	val = xrnic_ior(ibdev->xl, XRNIC_ROCE_PAUSE_OFFSET);
	xoff = simple_strtol(buf, NULL, 10);
	if (xoff >= PFC_XON_XOFF_MIN && xoff <= PFC_XON_XOFF_MAX) {
		xrnic_iow(ibdev->xl, XRNIC_ROCE_PAUSE_OFFSET, (xoff << 16) | (val & 0xFFFF));
	} else {
		pr_err("Error: XOFF threshold must be 0 to 512.\n", val);
	}
	return count;
}


static ssize_t store_non_roce_pfc_xoff(struct kobject *kobj,
			struct kobj_attribute *attr,
			const char *buf, size_t count)
{
	u32 val, xoff;
	val = xrnic_ior(ibdev->xl, XRNIC_NON_ROCE_PAUSE_OFFSET);
	xoff = simple_strtol(buf, NULL, 10);
	if (xoff >= PFC_XON_XOFF_MIN && xoff <= PFC_XON_XOFF_MAX) {
		xrnic_iow(ibdev->xl, XRNIC_NON_ROCE_PAUSE_OFFSET, (xoff << 16) | (val & 0xFFFF));
	} else {
		pr_err("Error: XOFF threshold must be 0 to 512.\n", val);
	}
	return count;
}

static ssize_t show_pfc_priority_check(struct kobject *kobj,
		        struct kobj_attribute *attr, char *buf)
{
	u32 val;

	val = xrnic_ior(ibdev->xl, XRNIC_PAUSE_CONF);
	return sprintf(buf, "%u\n", (val >> XRNIC_DIS_PRIO_CHECK_BIT) & 1);
}

static ssize_t store_pfc_priority_check(struct kobject *kobj,
			struct kobj_attribute *attr,
			const char *buf, size_t count)
{
	u32 val, temp;

	val = xrnic_ior(ibdev->xl, XRNIC_PAUSE_CONF);
	temp = simple_strtol(buf, NULL, 10);

	if (temp == 1) {
		val |= (1 << XRNIC_DIS_PRIO_CHECK_BIT);
		xrnic_iow(ibdev->xl, XRNIC_PAUSE_CONF, val);
	} else if (!temp) {
		val &= ~(1 << XRNIC_DIS_PRIO_CHECK_BIT);
		xrnic_iow(ibdev->xl, XRNIC_PAUSE_CONF, val);
	} else {
		pr_err("Error: value must be either a 1 or 0\n");
	}
	return count;
}

static struct kobj_attribute disable_priory_check_attr =
__ATTR(dis_prioirty_check, 0660, show_pfc_priority_check, store_pfc_priority_check);

/* Non-RoCE PFC configuration */
static struct kobj_attribute non_roce_pfc_enable_attr =
__ATTR(en_non_roce_pfc, 0660, show_non_roce_pfc_enable, store_non_roce_pfc_enable);

static struct kobj_attribute non_roce_pfc_priority_attr =
__ATTR(non_roce_pfc_priority, 0660, show_non_roce_pfc_priority, store_non_roce_pfc_priority);

static struct kobj_attribute non_roce_pfc_xon_attr =
__ATTR(non_roce_xon_threshold, 0660, show_non_roce_pfc_xon, store_non_roce_pfc_xon);

static struct kobj_attribute non_roce_pfc_xoff_attr =
__ATTR(non_roce_xoff_threshold, 0660, show_non_roce_pfc_xoff, store_non_roce_pfc_xoff);


/* RoCE PFC configuration */
static struct kobj_attribute roce_pfc_enable_attr =
__ATTR(en_roce_pfc, 0660, show_roce_pfc_enable, store_roce_pfc_enable);

static struct kobj_attribute roce_pfc_priority_attr =
__ATTR(roce_pfc_priority, 0660, show_roce_pfc_priority, store_roce_pfc_priority);

static struct kobj_attribute roce_pfc_xon_attr =
__ATTR(roce_xon_threshold, 0660, show_roce_pfc_xon, store_roce_pfc_xon);

static struct kobj_attribute roce_pfc_xoff_attr =
__ATTR(roce_xoff_threshold, 0660, show_roce_pfc_xoff, store_roce_pfc_xoff);


static struct attribute *pfc_attrs[] = {
	&roce_pfc_enable_attr.attr,
	&roce_pfc_priority_attr.attr,
	&roce_pfc_xon_attr.attr,
	&roce_pfc_xoff_attr.attr,

	&non_roce_pfc_enable_attr.attr,
	&non_roce_pfc_priority_attr.attr,
	&non_roce_pfc_xon_attr.attr,
	&non_roce_pfc_xoff_attr.attr,
	&disable_priory_check_attr.attr,
	NULL,
};

struct attribute_group pfc_attr_group = {
	.attrs = pfc_attrs,
};

static int pfc_create_sysfs_entries(const char *name,
				struct kobject *parent,
				const struct attribute_group *grp,
				struct kobject **kobj)
{
	int ret = 0;

	*kobj = kobject_create_and_add(name, parent);

	if (!*kobj) {
		pr_err("pfc %s sysfs create failed", name);
		return -ENOMEM;
	}

	ret = sysfs_create_group(*kobj, grp);
	if (ret < 0) {
		pr_err("%s unable to create pfc %s sysfs entries\n",
				__func__, name);
		return ret;
	}

	return ret;
}

char *xib_get_rq_mem(void)
{
	return rq_mem;
}

char *xib_get_sq_mem(void)
{
	return sq_mem;
}

int update_mtu(struct net_device *dev)
{
	u32 mtu;

	switch (dev->mtu) {
	case 340:
		mtu = QP_PMTU_256;
		break;
	case 592:
		mtu = QP_PMTU_512;
		break;
	case 1500:
		mtu = QP_PMTU_1024;
		break;
	case 2200:
		mtu = QP_PMTU_2048;
		break;
	case 4200:
		mtu = QP_PMTU_4096;
		break;
	default:
		mtu = QP_PMTU_4096;
		break;
	}

	/* update ib dev structure with mtu */
	pr_debug("Updating MTU to %d\n", mtu);
	ibdev->mtu = mtu;
	return 0;
}

void *xib_alloc_mem(char *from, size_t len, u64 *pa)
{
	struct xilinx_ib_dev *xib = ibdev;
	void *va;

	va = xib_alloc_coherent(from, xib, len, pa, GFP_KERNEL);
	return va;
}
EXPORT_SYMBOL(xib_alloc_mem);

u32 get_user_sq_depth(u32 hw_qpn)
{
	return ibdev->qp_list[hw_qpn]->sq.max_wr;
}
EXPORT_SYMBOL(get_user_sq_depth);

void *get_xrnic_local(void)
{
	return ibdev->xl;
}
EXPORT_SYMBOL(get_xrnic_local);

void *get_xib_ptr(void)
{
	return (void *)ibdev;
}
EXPORT_SYMBOL(get_xib_ptr);

dma_addr_t get_qp_sq_ba(u32 qp_num)
{
	return ibdev->qp_list[qp_num]->sq_ba_p;
}
EXPORT_SYMBOL(get_qp_sq_ba);


int xib_dealloc_mem(u64 len, void *va, u64 pa)
{
	struct xilinx_ib_dev *xib = ibdev;

	xib_free_coherent(xib, len, va, pa);

	return 0;
}
EXPORT_SYMBOL(xib_dealloc_mem);

struct ib_mr *xib_reg_mr(struct ib_qp *ibqp, u64 va, u64 len, u64 *pa,
		struct ib_pd *ibpd, int umem_pgs)
{
        struct xib_qp *qp;// = get_xib_qp(ibqp);
        struct xilinx_ib_dev *xib = ibdev;
	u64 vaddr = va;
	u64 length = len;
	int ret;
	struct xib_mr *mr = NULL;
	struct xib_pd *pd = get_xib_pd(ibpd);
	u32 mr_idx;
	u8 rkey;

#ifndef CONFIG_64BIT
	if (*pa < 0xFFFFFFFF) {
		vaddr = (u32)(uintptr_t)va;
	}
#endif

	mr = kzalloc(sizeof(*mr), GFP_KERNEL);
	if (!mr) {
		pr_err("Failed to allocate memory for mr\n");
		return NULL;
	}

	spin_lock_bh(&xib->lock);
	ret = xib_bmap_alloc_id(&xib->mr_map, &mr_idx);
	spin_unlock_bh(&xib->lock);
	if (ret < 0)
		goto fail;

	/*rkey - 8bits random number*/
	get_random_bytes(&rkey, sizeof(rkey));

	/* Alloc mr pointer */
	mr->ib_mr.lkey = (mr_idx << 8) | rkey;
	mr->ib_mr.rkey = (mr_idx << 8) | rkey;
	mr->mr_idx = mr_idx;
	mr->rkey = rkey;
	mr->ib_mr.device = &xib->ib_dev;

	mr->type = XIB_MR_USER;

	ret = xrnic_reg_mr(xib, vaddr, length, pa, umem_pgs, pd->pdn, mr_idx, rkey);
	if (ret) {
		pr_err("%s:Failed to register MR\n", __func__);
		goto fail2;
	}

	return &mr->ib_mr;
fail2:
	spin_lock_bh(&xib->lock);
	xib_bmap_release_id(&xib->mr_map, mr_idx);
	spin_unlock_bh(&xib->lock);
fail:
	kfree(mr);
	return NULL;
}
EXPORT_SYMBOL(xib_reg_mr);

int set_ip_address(struct net_device *dev, u32 is_ipv4)
{
	char ip_addr[16];
	int ret = 0;

	if (!dev) {
		pr_err("Dev is null\n");
		return 0;
	}
	if (is_ipv4) {
		u32 ipv4_addr = 0;
		struct in_device *inet_dev = (struct in_device *)dev->ip_ptr;

		if (!inet_dev) {
			pr_err("inet dev is null\n");
			return -EFAULT;
		}
		if (inet_dev->ifa_list) {
			ipv4_addr = inet_dev->ifa_list->ifa_address;
			if (!ipv4_addr) {
				pr_err("ifa_address not available\n");
				return -EINVAL;
			}

			if (!ibdev)
				return -EFAULT;

			if (!ibdev->xl)
				return -EFAULT;

			config_raw_ip(ibdev->xl, XRNIC_IPV4_ADDR, (u32 *)&ipv4_addr, 0);
			snprintf(ip_addr, 16, "%pI4", &ipv4_addr);
			pr_info("IP address is :%s\n", ip_addr);
		} else {
			pr_info("IP address not available at present\n");
			return -EFAULT;
		}
	} else {
		struct inet6_dev *idev;
		struct inet6_ifaddr *ifp, *tmp;
		u32 i, ip_avail = 0;

		idev = __in6_dev_get(dev);
		if (!idev) {
			pr_err("ipv6 inet device not found\n");
			return -EFAULT;
		}

		list_for_each_entry_safe(ifp, tmp, &idev->addr_list, if_list) {
			pr_info("IP=%pI6, MAC=%pM\n", &ifp->addr, dev->dev_addr);
			for (i = 0; i < 16; i++) {
				pr_info("IP=%x\n", ifp->addr.s6_addr[i]);
				ip_addr[15 - i] = ifp->addr.s6_addr[i];
			}
			ip_avail = 1;
			config_raw_ip(ibdev->xl, XRNIC_IPV6_ADD_1, (u32 *)ifp->addr.s6_addr, 1);
		}

		if (!ip_avail) {
			pr_info("IPv6 address not available at present\n");
			return 0;
		}
	}
	ret = update_mtu(dev);
	return ret;
}

static int handle_netdev_notifier(struct notifier_block *notifier,
				       unsigned long event, void *ptr)
{
	struct net_device *dev;

	dev = netdev_notifier_info_to_dev(ptr);
	if (!dev) {
		pr_err("Failed to get the net device");
		return -EINVAL;
	}

	switch (event) {
		case NETDEV_CHANGEADDR:
			pr_info("%s mac changed\n", ibdev->netdev->name);
			xrnic_set_mac(ibdev->xl, dev->dev_addr);
			break;
		case NETDEV_CHANGEMTU:
			pr_info("MTU changed\n");
			update_mtu(dev);
			break;
	}
	return 0;
}


static int handle_inetaddr_notification(struct notifier_block *notifier,
				       unsigned long event, void *data, u32 is_ipv4)
{
	struct in_ifaddr *ifa = data;
	struct net_device *event_netdev;
	struct net_device *dev = __dev_get_by_name(&init_net, ibdev->netdev->name);
	int ret = 0;

	if (!ifa) {
		pr_err("ifaddr is NULL\n");
		return -EINVAL;
	}

	if (!dev) {
		pr_err("Failed to get the net device");
		return -EINVAL;
	}

	if (is_ipv4) {
		event_netdev = ifa->ifa_dev->dev;

		/* Check whether notification is for ernic-ether or not*/
		if (event_netdev != dev)
			return 0;
	}

	switch (event) {
	case NETDEV_DOWN:
		pr_info("%s link down\n", ibdev->netdev->name);
		break;
	case NETDEV_UP:
		pr_info("%s link up\n", ibdev->netdev->name);
		ret = set_ip_address(dev, is_ipv4);
		break;
	}
	return 0;
}


static int cmac_inetaddr_event(struct notifier_block *notifier,
			       unsigned long event, void *data)
{
	handle_inetaddr_notification(notifier, event, data, 1);
	return 0;
}

static int cmac_inet6addr_event(struct notifier_block *notifier,
			       unsigned long event, void *data)
{
	handle_inetaddr_notification(notifier, event, data, 0);
	return 0;
}

static void xib_get_guid(u8 *dev_addr, u8 *guid)
{
	u8 mac[ETH_ALEN];

	/* MAC-48 to EUI-64 mapping */
	memcpy(mac, dev_addr, ETH_ALEN);
	guid[0] = mac[0] ^ 2;
	guid[1] = mac[1];
	guid[2] = mac[2];
	guid[3] = 0xff;
	guid[4] = 0xfe;
	guid[5] = mac[3];
	guid[6] = mac[4];
	guid[7] = mac[5];
}

int xib_alloc_ucontext(struct ib_ucontext *uctx, struct ib_udata *udata)
{
	struct ib_device *ib_dev = uctx->device;
	struct xilinx_ib_dev *xib = get_xilinx_dev(ib_dev);
	struct xrnic_local *xl = xib->xl;
	struct xib_ib_alloc_ucontext_resp resp;
	size_t min_len;
	int ret = 0;

	dev_dbg(&xib->ib_dev.dev, "%s : <---------- \n", __func__);

	resp.qp_tab_size = xib->dev_attr.max_qp;

	resp.db_pa = (u64)xl->db_pa;
	resp.db_size = xl->db_size;
	resp.cq_ci_db_pa = (u64)xl->qp1_sq_db_p;
	resp.rq_pi_db_pa = (u64)xl->qp1_rq_db_p;

	resp.cq_ci_db_size = xl->cq_rq_db_size;
	resp.rq_pi_db_size = xl->cq_rq_db_size;

	dev_dbg(&xib->ib_dev.dev, "%s: db_pa: %llx db_size: %x\n", __func__, resp.db_pa,
			resp.db_size);

	min_len = min_t(size_t, sizeof(struct xib_ib_alloc_ucontext_resp),
			udata->outlen);
	ret = ib_copy_to_udata(udata, &resp, min_len);
	if (ret)
		return ret;

	return 0;
}

static void xib_dealloc_ucontext(struct ib_ucontext *ibucontext)
{
	struct xib_ucontext *context = get_xib_ucontext(ibucontext);

	dev_dbg(&ibucontext->device->dev, "%s : <---------- \n", __func__);

}

static int xib_mmap(struct ib_ucontext *ibucontext,
			 struct vm_area_struct *vma)
{
	struct xilinx_ib_dev *xib = get_xilinx_dev(ibucontext->device);
	struct xrnic_local *xl = xib->xl;
	u64 pfn;
	ssize_t length;

	if (((vma->vm_end - vma->vm_start) % PAGE_SIZE) != 0)
		return -EINVAL;

	length = vma->vm_end - vma->vm_start;

	dev_dbg(&xib->ib_dev.dev, "%s : pg_off: %x length: %x \n", __func__, vma->vm_pgoff, length);
	vma->vm_page_prot = pgprot_noncached(vma->vm_page_prot);
	if (io_remap_pfn_range(vma,
				vma->vm_start,
				vma->vm_pgoff,
			       length,
			       vma->vm_page_prot)) {
		dev_err(&xib->ib_dev.dev, "Failed to map device memory");
		return -EAGAIN;
	}
	return 0;
}


/* ernic hw automatically reposts consumed receive buffers
 * no need for app to post receive wr
 */
int xib_post_recv(struct ib_qp *ibqp, const struct ib_recv_wr *wr,
		const struct ib_recv_wr **bad_wr)
{
	return xib_kernel_qp_post_recv(ibqp, wr, bad_wr);
}

int xib_map_mr_sge(struct ib_mr *ibmr, struct scatterlist *sg, int sg_nents,
			 unsigned int *sg_offset)

{
        struct xilinx_ib_dev *xib = ibdev;
	struct xib_mr *mr = get_xib_mr(ibmr);
	u64 pa = sg->dma_address;
	u64 va = sg->offset;
	u64 vaddr = va;
	u64 length = sg_dma_len(sg);
	int ret;
#ifndef CONFIG_64BIT
	if (pa < 0xFFFFFFFF) {
		vaddr = (u32)(uintptr_t)va;
	}
#endif
	if (sg == NULL) {
		pr_err("%s: scatterlist is NULL\n", __func__);
		goto fail;
	}

	if(sg_nents > 1) {
		pr_err("%s: SGE entries cannot be more than 1\n", __func__);
		goto fail;
	}
	ret = xrnic_reg_mr(xib, vaddr, length, &pa, sg_nents, mr->pd, mr->mr_idx, mr->rkey);
	if (ret) {
		pr_err("%s:Failed to register MR\n", __func__);
		goto fail;
	}
	return ret;
fail:
	spin_lock_bh(&xib->lock);
	xib_bmap_release_id(&xib->mr_map, mr->mr_idx);
	spin_unlock_bh(&xib->lock);
	kfree(mr);
	return -1;
}

struct ib_mr *xib_alloc_mr(struct ib_pd *ibpd,
				enum ib_mr_type mr_type,
				u32 max_num_sg)
{
        struct xilinx_ib_dev *xib = ibdev;
	struct xib_mr *mr = NULL;
	struct xib_pd *pd = get_xib_pd(ibpd);
	u32 mr_idx;
	u8 rkey;
	int ret;

	mr = kzalloc(sizeof(*mr), GFP_KERNEL);
	if (!mr) {
		pr_err("Failed to allocate memory for mr\n");
		return NULL;
	}

	spin_lock_bh(&xib->lock);
	ret = xib_bmap_alloc_id(&xib->mr_map, &mr_idx);
	spin_unlock_bh(&xib->lock);
	if (ret < 0)
		goto fail;

	get_random_bytes(&rkey, sizeof(rkey));
	/* Alloc mr pointer */
	mr->ib_mr.lkey = (mr_idx << 8) | rkey;
	mr->ib_mr.rkey = (mr_idx << 8) | rkey;
	mr->mr_idx = mr_idx;
	mr->rkey = rkey;
	mr->pd = pd->pdn;
	mr->ib_mr.device = &xib->ib_dev;

	mr->type = XIB_MR_USER;
	return &mr->ib_mr;
fail:
	kfree(mr);
	return NULL;
}

static int xib_alloc_pd(struct ib_pd *ibpd,
			struct ib_udata *udata)
{
	struct ib_device *ibdev = ibpd->device;
	struct xilinx_ib_dev *xib = get_xilinx_dev(ibdev);
	struct xib_pd *pd = get_xib_pd(ibpd);
	int ret;
	struct xib_ucontext *context = rdma_udata_to_drv_context(
				udata, struct xib_ucontext, ib_uc);

	/* TODO we need to lock to protect allocations */
	spin_lock_bh(&xib->lock);
	ret = xib_bmap_alloc_id(&xib->pd_map, &pd->pdn);
	spin_unlock_bh(&xib->lock);
	if (ret < 0) {
		printk("%s ERROR\n", __func__);
		return ret;
	}

	dev_dbg(&xib->ib_dev.dev, "%s : pd: %d \n", __func__, pd->pdn);
	if (udata && context) {
		struct xib_ib_alloc_pd_resp uresp;

		uresp.pdn = pd->pdn;
		/* TODO check udata->outlen ? */
                size_t min_len = min_t(size_t, sizeof(struct xib_ib_alloc_pd_resp), udata->outlen);

		ret = ib_copy_to_udata(udata, &uresp, min_len);
		if (ret)
                {
		printk("%s COPY ERROR\n", __func__);
			goto err;
                }
	}

	return 0;
err:
	spin_lock_bh(&xib->lock);
	xib_bmap_release_id(&xib->pd_map, pd->pdn);
	spin_unlock_bh(&xib->lock);
	return ret;
}

static int xib_dealloc_pd(struct ib_pd *ibpd, struct ib_udata * udata)
{
	struct xilinx_ib_dev *xib = get_xilinx_dev(ibpd->device);
	struct xib_pd *pd = get_xib_pd(ibpd);

	dev_dbg(&xib->ib_dev.dev, "%s : <---------- \n", __func__);

	xib_bmap_release_id(&xib->pd_map, pd->pdn);

	/* TODO tell hw about dealloc? */
	return 0;
}


static enum rdma_link_layer xib_get_link_layer(struct ib_device *device,
						    u32 port_num)
{
	return IB_LINK_LAYER_ETHERNET;
}

/* Device */
struct net_device *xib_get_netdev(struct ib_device *ibdev, u32 port_num)
{
	struct xilinx_ib_dev *xib = get_xilinx_dev(ibdev);
	struct net_device *netdev = NULL;

	dev_dbg(&ibdev->dev, "%s : <---------- \n", __func__);

	rcu_read_lock();
	if (xib)
		netdev = xib->netdev;
	/* dev_put shall be called by whoever calls get_netdev */
	if (netdev)
		dev_hold(netdev);

	rcu_read_unlock();
	return netdev;
}

static int xib_query_device(struct ib_device *ibdev,
				struct ib_device_attr *props,
				struct ib_udata *uhw)
{
	struct xilinx_ib_dev *xib = get_xilinx_dev(ibdev);

	dev_dbg(&xib->ib_dev.dev, "%s : <---------- \n", __func__);

	memset(props, 0, sizeof(*props));
#if 1
	props->max_qp		= app_qp_cnt;
#else
	props->max_qp		= xib->dev_attr.max_qp;
#endif
	props->max_send_sge	= xib->dev_attr.max_send_sge;
	props->max_sge_rd	= xib->dev_attr.max_send_sge;
#if 1
	props->max_qp_wr	= app_qp_depth;
	props->max_recv_sge	= max_rq_sge;
#else
	props->max_qp_wr	= 32;
#endif
	props->max_pd		= xib->dev_attr.max_pd;
	/* TODO ernic doesnt support scatter list
	 * in mr, restrict mr to 1 page
	 */
	props->max_mr		= xib->dev_attr.max_mr;
	props->atomic_cap	= IB_ATOMIC_NONE;

	props->device_cap_flags    = IB_DEVICE_CHANGE_PHY_PORT |
				IB_DEVICE_PORT_ACTIVE_EVENT |
				IB_DEVICE_RC_RNR_NAK_GEN;

#if 0
	props->max_cq		= app_qp_depth;
#else
	props->max_cq		= xib->dev_attr.max_qp - 1;
#endif
	props->max_cqe		= xib->dev_attr.max_cq_wqes;
	props->max_pkeys	= 1;
	props->max_qp_rd_atom   = 0x10; /* TODO how to arrive at these */
	props->max_qp_init_rd_atom = 0x10;

	return 0;
}

/* struct ib_qp *xib_create_qp(struct ib_pd *pd, */
/* 				struct ib_qp_init_attr *init_attr, */
/* 				struct ib_udata *udata) */
/* { */
/* 	struct xilinx_ib_dev *xib = get_xilinx_dev(pd->device); */
/* 	struct xrnic_local *xl = xib->xl; */
/* 	struct ib_qp *ibqp; */
/* 	struct xib_qp *qp; */
/* 	u32 val; */

/* 	dev_dbg(&pd->device->dev, "%s : <---------- \n", __func__); */

/* 	if (init_attr->srq) */
/* 		return ERR_PTR(-EINVAL); */

/* 	switch (init_attr->qp_type) { */
/* 	case IB_QPT_GSI: */
/* 		return xib_gsi_create_qp(pd, init_attr); */
/* 	case IB_QPT_RC: */
/* 		if (!check_qp_depths(init_attr->cap.max_send_wr)) { */
/* 			dev_err(&pd->device->dev, "qp depth should be a power of 2\n"); */
/* 			return ERR_PTR(-EINVAL); */
/* 		} */
/* 		if (!check_qp_depths(init_attr->cap.max_recv_wr)) { */
/* 			dev_err(&pd->device->dev, "qp depth should be a power of 2\n"); */
/* 			return ERR_PTR(-EINVAL); */
/* 		} */
/* 		if (udata)  */
/* 			ibqp = xib_create_user_qp(pd, init_attr, udata); */
/* 		else */
/* 			ibqp = xib_create_kernel_qp(pd, init_attr); */
/* /\*  */
/*  * AR# 75247: Initialize STAT_QPN.curr_rnr_retry_cnt and curr_retry_cnt */
/*  *\/ */
/* 		qp = get_xib_qp(ibqp); */
/* 		val = xrnic_ior(xl, XRNIC_STAT_QP(qp->hw_qpn)); */
/* 		if ((val >> 24) != 0x77) { */
/* 			xrnic_iow(xl, XRNIC_STAT_QP(qp->hw_qpn), val | (0x77 << 24)); */
/* 			wmb(); */
/* 		} */
/* 		return ibqp; */
/* 	default: */
/* 		dev_err(&pd->device->dev, "unsupported qp type %d\n", */
/* 			    init_attr->qp_type); */
/* 		/\* Don't support raw QPs *\/ */
/* 		return ERR_PTR(-EINVAL); */
/* 	} */
/* } */

int xib_create_qp(struct ib_qp *kqp,
				struct ib_qp_init_attr *init_attr,
				struct ib_udata *udata)
{
        struct ib_pd *pd = kqp->pd;
	struct xilinx_ib_dev *xib = get_xilinx_dev(pd->device);
	struct xrnic_local *xl = xib->xl;
	struct ib_qp *ibqp;
	struct xib_qp *qp;
        u32 val;
        u32 ret = 0;

	dev_dbg(&pd->device->dev, "%s : <---------- \n", __func__);

	if (init_attr->srq)
		return (-EINVAL);

	switch (init_attr->qp_type) {
	case IB_QPT_GSI:
            dev_dbg(&pd->device->dev, "%s : xib_gsi_create_qp\n", __func__);
            return xib_gsi_create_qp(kqp, pd, init_attr);
	case IB_QPT_RC:

                dev_dbg(&pd->device->dev, "%s : IB_QPT_RC\n", __func__);
		if (!check_qp_depths(init_attr->cap.max_send_wr)) {
			dev_err(&pd->device->dev, "qp depth should be a power of 2\n");
			return (-EINVAL);
		}
                dev_dbg(&pd->device->dev, "%s : check_qp_depths\n", __func__);
		if (!check_qp_depths(init_attr->cap.max_recv_wr)) {
			dev_err(&pd->device->dev, "qp depth should be a power of 2\n");
			return (-EINVAL);
		}
		if (udata)
                {
                    dev_dbg(&pd->device->dev, "%s : create_user_qp\n", __func__);
                    ret = xib_create_user_qp(kqp, pd, init_attr, udata);
                }
		else
                {
                    dev_dbg(&pd->device->dev, "%s : create_kernel_qp\n", __func__);
                    ret = xib_create_kernel_qp(kqp, pd, init_attr);
                }
/*
 * AR# 75247: Initialize STAT_QPN.curr_rnr_retry_cnt and curr_retry_cnt
 */
                dev_dbg(&pd->device->dev, "%s : get_xib_qp\n", __func__);
		qp = get_xib_qp(kqp);
		val = xrnic_ior(xl, XRNIC_STAT_QP(qp->hw_qpn));
		if ((val >> 24) != 0x77) {
			xrnic_iow(xl, XRNIC_STAT_QP(qp->hw_qpn), val | (0x77 << 24));
			wmb();
		}
		return ret;
	default:
		dev_err(&pd->device->dev, "unsupported qp type %d\n",
			    init_attr->qp_type);
		/* Don't support raw QPs */
		return (-EINVAL);
	}
}

int xib_query_port(struct ib_device *ibdev, u32 port,
		       struct ib_port_attr *props)
{
	struct xilinx_ib_dev *xib = get_xilinx_dev(ibdev);

	dev_dbg(&ibdev->dev, "%s : port: %d <---------- \n", __func__, port);

	memset(props, 0, sizeof(struct ib_port_attr));

	if (netif_running(xib->netdev) && netif_carrier_ok(xib->netdev)) {
		props->state = IB_PORT_ACTIVE;
		props->phys_state = 5;
	} else {
		props->state = IB_PORT_DOWN;
		props->phys_state = 3;
	}

	props->gid_tbl_len = 128; /* TODO */
	props->max_mtu = IB_MTU_4096;
	props->lid = 0;
	props->lmc = 0;
	props->sm_lid = 0;
	props->sm_sl = 0;
	props->active_mtu = iboe_get_mtu(xib->netdev->mtu);
	props->port_cap_flags = IB_PORT_CM_SUP | IB_PORT_REINIT_SUP |
				IB_PORT_DEVICE_MGMT_SUP |
				IB_PORT_VENDOR_CLASS_SUP;

	props->active_speed = xib->active_speed;
	props->active_width = xib->active_width;
	props->max_msg_sz = 0x80000000;
	props->bad_pkey_cntr = 0;
	props->qkey_viol_cntr = 0;
	props->subnet_timeout = 0;
	props->init_type_reply = 0;
	props->pkey_tbl_len = 1; /* TODO is it 1? */

	return 0;
}

#define PKEY_ID	0xffff
static int xib_query_pkey(struct ib_device *ibdev, u32 port, u16 index,
			      u16 *pkey)
{
	dev_dbg(&ibdev->dev, "%s : <---------- \n", __func__);
	*pkey = PKEY_ID; /* TODO */
	return 0;
}

#define XIB_MAX_PORT	1
int xib_add_gid(const struct ib_gid_attr *attr, void **context)
{
	if (!rdma_cap_roce_gid_table(attr->device, attr->port_num))
		return -EINVAL;

	if (attr->port_num > XIB_MAX_PORT)
		return -EINVAL;

	if (!context)
		return -EINVAL;

	return 0;
}

int xib_del_gid(const struct ib_gid_attr *attr, void **context)
{
	if (!rdma_cap_roce_gid_table(attr->device, attr->port_num))
		return -EINVAL;

	if (attr->port_num > XIB_MAX_PORT)
		return -EINVAL;

	if (!context)
		return -EINVAL;

	return 0;
}


int xib_create_ah(struct ib_ah *ibah, struct rdma_ah_init_attr *ah_attr,
				struct ib_udata *udata)
{
	struct xib_ah *ah = get_xib_ah(ibah);

	ah->attr = *ah_attr;
	return 0;
}

int xib_destroy_ah(struct ib_ah *ib_ah, uint32_t flags)
{
	struct xib_ah *ah = get_xib_ah(ib_ah);

	dev_dbg(&ib_ah->device->dev, "%s : <---------- \n", __func__);
	return 0;

}


static enum ib_qp_state xib_get_ibqp_state(enum xib_qp_state qp_state)
{
	switch (qp_state) {
	case XIB_QP_STATE_RESET:
		return IB_QPS_RESET;
	case XIB_QP_STATE_INIT:
		return IB_QPS_INIT;
	case XIB_QP_STATE_RTR:
		return IB_QPS_RTR;
	case XIB_QP_STATE_RTS:
		return IB_QPS_RTS;
	case XIB_QP_STATE_SQD:
		return IB_QPS_SQD;
	case XIB_QP_STATE_ERR:
		return IB_QPS_ERR;
	case XIB_QP_STATE_SQE:
		return IB_QPS_SQE;
	}
	return IB_QPS_ERR;
}

static enum xib_qp_state xib_get_state_from_ibqp(
					enum ib_qp_state qp_state)
{
	switch (qp_state) {
	case IB_QPS_RESET:
		return XIB_QP_STATE_RESET;
	case IB_QPS_INIT:
		return XIB_QP_STATE_INIT;
	case IB_QPS_RTR:
		return XIB_QP_STATE_RTR;
	case IB_QPS_RTS:
		return XIB_QP_STATE_RTS;
	case IB_QPS_SQD:
		return XIB_QP_STATE_SQD;
	case IB_QPS_ERR:
		return XIB_QP_STATE_ERR;
	default:
		return XIB_QP_STATE_ERR;
	}
}


int get_rq_pending_cnt(struct ib_qp *ibqp)
{
	struct xilinx_ib_dev *xib = get_xilinx_dev(ibqp->device);
	struct xib_qp *qp = get_xib_qp(ibqp);
	struct xrnic_local *xl = xib->xl;
	int ret;
	u32 rq_ci, rq_pi;

	rq_pi = *(volatile u32 *)(xl->qp1_rq_db_v + qp->hw_qpn);
	rq_ci = xrnic_ior(xl, XRNIC_RQ_CONS_IDX(qp->hw_qpn));

	if (rq_pi == rq_ci)
		ret = 0;
	else if (rq_pi > rq_ci)
		ret = (rq_pi - rq_ci);
	else
		ret = (qp->rq.max_wr - rq_ci + rq_pi);

	return ret;
}

/*
	rem_rx_pkts: If the data QP has done few transactions and received
	incoming SENDs which are not processed yet, but need to enable the
	HW acceleration, then the first 16 bits of STAT RQ PI must have
	number of Rx pkts that are not processed
*/
int xib_enable_hw_accl(struct ib_qp *ibqp)
{
	struct xilinx_ib_dev *xib = get_xilinx_dev(ibqp->device);
	struct xib_qp *qp = get_xib_qp(ibqp);
	struct xrnic_local *xl = xib->xl;
	u32 val;
	dma_addr_t db_addr;

	/* Enable SW override enable */
	val = xrnic_ior(xl, XRNIC_ADV_CONF);
	val |= (XRNIC_SW_OVER_RIDE_EN << XRNIC_SW_OVER_RIDE_BIT);
	xrnic_iow(xl, XRNIC_ADV_CONF, val);

	xrnic_iow(xl, XRNIC_CQ_HEAD_PTR(qp->hw_qpn), 0);
	xrnic_iow(xl, XRNIC_SQ_PROD_IDX(qp->hw_qpn), 0);
	xrnic_iow(xl, XRNIC_STAT_CUR_SQ_PTR(qp->hw_qpn), 0);
	wmb();

	db_addr = (dma_addr_t)xrnic_get_rq_db_addr(xl, qp->hw_qpn);
	xrnic_iow(xl, XRNIC_RCVQ_WP_DB_ADDR_LSB(qp->hw_qpn), db_addr);
	wmb();

	xrnic_iow(xl, XRNIC_RCVQ_WP_DB_ADDR_MSB(qp->hw_qpn),
			 UPPER_32_BITS(db_addr));
	wmb();

	db_addr = (dma_addr_t)xrnic_get_sq_db_addr(xl, qp->hw_qpn);
	xrnic_iow(xl, XRNIC_CQ_DB_ADDR_LSB(qp->hw_qpn), db_addr);
	wmb();

	xrnic_iow(xl, XRNIC_CQ_DB_ADDR_MSB(qp->hw_qpn),
				UPPER_32_BITS(db_addr));
	wmb();

	val = xrnic_ior(xl, XRNIC_STAT_RQ_PROD_IDX(qp->hw_qpn));
	/* get pending RQs to be processed */
	val = (val & 0xFFFF) << 16 | get_rq_pending_cnt(ibqp);
	xrnic_iow(xl, XRNIC_STAT_RQ_PROD_IDX(qp->hw_qpn), val);

	val = xrnic_ior(xl, XRNIC_QP_CONF(qp->hw_qpn));
	val &= ~(QP_HW_HSK_DIS);
	xrnic_iow(xl, XRNIC_QP_CONF(qp->hw_qpn), val);

	val = xrnic_ior(xl, XRNIC_ADV_CONF);
	val &= ~(XRNIC_SW_OVER_RIDE_EN << XRNIC_SW_OVER_RIDE_BIT);
	xrnic_iow(xl, XRNIC_ADV_CONF, val);
	return 0;
}

int xib_modify_qp(struct ib_qp *ibqp, struct ib_qp_attr *attr,
		      int attr_mask, struct ib_udata *udata)
{
	struct xilinx_ib_dev *xib = get_xilinx_dev(ibqp->device);
	struct xib_qp *qp = get_xib_qp(ibqp);
	struct xib_qp_modify_params qp_params = { 0, };
	const struct ib_global_route *grh = rdma_ah_read_grh(&attr->ah_attr);
	enum xib_qp_state curr_qp_state, new_qp_state;
        struct xib_sq *sq = &qp->sq;

	dev_dbg(&xib->ib_dev.dev, "%s : <---------- \n", __func__);

	dev_dbg(&xib->ib_dev.dev, "modify qp: qp %px attr_mask=0x%x, state=%d\n", ibqp, attr_mask,
		 attr->qp_state);

	if (attr_mask & IB_ENABLE_QP_HW_ACCL)
		return xib_enable_hw_accl(ibqp);
	if (attr_mask & IB_QP_RST_RQ)
		return xib_rst_rq(qp);

	if (attr_mask & IB_QP_RST_SQ_CQ)
		return xib_rst_cq_sq(qp, attr->nvmf_rhost);
	curr_qp_state = xib_get_ibqp_state(qp->state);
	if (attr_mask & IB_QP_STATE)
		new_qp_state = attr->qp_state;
	else
		new_qp_state = curr_qp_state;

	if (!ib_modify_qp_is_ok
		(curr_qp_state, new_qp_state, ibqp->qp_type, attr_mask)) {
		dev_err(&xib->ib_dev.dev,
			"modify qp: invalid attr mask: 0x%x specified for\n"
			"qpn=0x%x of type=0x%x curr_qp_state=0x%x,\n"
			"new_qp_state=0x%x\n", attr_mask, qp->hw_qpn+1,
			ibqp->qp_type, curr_qp_state, new_qp_state);
		return -EINVAL;
	}

	if (attr_mask & (IB_QP_AV | IB_QP_PATH_MTU)) {
		dev_dbg(&xib->ib_dev.dev, "mtu: %d traffic class: %d hop limit: %d\n",
				attr->path_mtu, grh->traffic_class,
				grh->hop_limit);

		qp_params.traffic_class = grh->traffic_class;
		qp_params.hop_limit = grh->hop_limit;

#ifdef DEBUG_IPV6
		memcpy(&qp_params.ip4_daddr, grh->dgid.raw + 12, 4);
		qp_params.ip_version = 4;
#else
		/* if IPV4 mapped Ipv6 address then first 10B are 0's, next 2B 's are all 1's
			The following macro checks the avoce condition */
		if (ipv6_addr_v4mapped((struct in6_addr *)&grh->sgid_attr->gid)) {
			memcpy(&qp_params.ip4_daddr, grh->dgid.raw + 12, 4);
			qp_params.ip_version = 4;
		} else {
			memcpy(qp_params.ipv6_addr, grh->dgid.raw, 16);
			qp_params.ip_version = 6;
		}
#endif

		ether_addr_copy(qp_params.dmac,
				attr->ah_attr.roce.dmac);
		dev_dbg(&xib->ib_dev.dev, "dmac: %x:%x:%x:%x:%x:%x\n", qp_params.dmac[0],
				qp_params.dmac[1], qp_params.dmac[2],
				qp_params.dmac[3], qp_params.dmac[4],
				qp_params.dmac[5]);
		qp_params.flags |= XIB_MODIFY_QP_DEST_MAC |
			XIB_MODIFY_QP_AV;
	}
	if (attr_mask & IB_QP_STATE) {
		qp_params.flags |= XIB_MODIFY_QP_STATE;
		qp_params.qp_state = xib_get_state_from_ibqp(attr->qp_state);
	}

	if (attr_mask & (IB_QP_RETRY_CNT | IB_QP_RNR_RETRY)) {
		qp_params.flags |= XIB_MODIFY_QP_TIMEOUT;
		qp_params.retry_cnt = attr->retry_cnt;
		if (attr->rnr_retry == 0)
			attr->rnr_retry = 5;
		qp_params.rnr_retry_cnt = attr->rnr_retry;
	}

	if (attr_mask & IB_QP_SQ_PSN) {
		qp_params.flags |= XIB_MODIFY_QP_SQ_PSN;
		qp_params.sq_psn = attr->sq_psn;
	}
	if (attr_mask & IB_QP_RQ_PSN) {
		qp_params.flags |= XIB_MODIFY_QP_RQ_PSN;
		qp_params.rq_psn = attr->rq_psn;
	}
	if (attr_mask & IB_QP_DEST_QPN) {
		qp_params.flags |= XIB_MODIFY_QP_DEST_QP;
		qp_params.dest_qp = attr->dest_qp_num;
	}

	xrnic_qp_modify(qp, &qp_params);

	return 0;
}

#define ROCE_REQ_MAX_INLINE_DATA_SIZE (256)
int xib_query_qp(struct ib_qp *ibqp, struct ib_qp_attr *qp_attr,
		int attr_mask, struct ib_qp_init_attr *qp_init_attr)
{
	struct xib_qp *qp = get_xib_qp(ibqp);

	dev_dbg(&ibqp->device->dev, "%s : <---------- \n", __func__);

	memset(qp_attr, 0, sizeof(*qp_attr));

	qp_attr->qp_state = xib_get_ibqp_state(qp->state);

	qp_attr->cap.max_send_wr = qp->sq.max_wr;
	qp_attr->cap.max_recv_wr = qp->rq.max_wr;
	qp_attr->cap.max_inline_data = ROCE_REQ_MAX_INLINE_DATA_SIZE;
	qp_attr->cap.max_send_sge = XIB_MAX_SQE_SGE; /* TODO get from device cap */
	qp_attr->cap.max_recv_sge = XIB_MAX_RQE_SGE; /* TODO get from device cap */

	qp_attr->ah_attr.type = RDMA_AH_ATTR_TYPE_ROCE;
	qp_attr->port_num = 1;
	qp_attr->alt_pkey_index = 0;
	qp_attr->alt_port_num = 0;
	qp_attr->alt_timeout = 0;
	/* for xnvmf use case */
	qp_attr->sq_ba_p = qp->sq_ba_p;
	qp_attr->rq_ba_p = qp->rq.rq_ba_p;

	return 0;
}

int xrnic_reset_user_qp(struct xib_qp *qp)
{
	#define QP_UNDER_RECOVERY       (1 << 6)
	struct xilinx_ib_dev *xib = get_xilinx_dev(qp->ib_qp.device);
	int val = 0, ret;
	unsigned long timeout;
	dma_addr_t db_addr;

	if (!xib) {
		dev_dbg(&xib->ib_dev.dev, "xib dev not found\n");
		return -EFAULT;
	}

	/* 1. Wait till SQ/OSQ are empty */
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

	/* 3. Wait till STAT_RQ_PI_DB == RQ_CI_DB */
	timeout = jiffies;
	do {
		val = xrnic_ior(xib->xl, XRNIC_RQ_CONS_IDX(qp->hw_qpn));
		ret = xrnic_ior(xib->xl, XRNIC_STAT_RQ_PROD_IDX(qp->hw_qpn));
		if (time_after(jiffies, (timeout + 1 * HZ)))
			break;
	} while(!(val == ret));

	/* 4. Disable HW handshake */
	val = xrnic_ior(xib->xl, XRNIC_QP_CONF(qp->hw_qpn));
	val |= QP_HW_HSK_DIS;
	xrnic_iow(xib->xl, XRNIC_QP_CONF(qp->hw_qpn), val);

	/* 5. write the RQ wr ptr DB address back
	(if in case other app changes it) */
	db_addr = xrnic_get_rq_db_addr(xib->xl, qp->hw_qpn);
	xrnic_iow(xib->xl, XRNIC_RCVQ_WP_DB_ADDR_LSB(qp->hw_qpn), db_addr);
	xrnic_iow(xib->xl, XRNIC_RCVQ_WP_DB_ADDR_MSB(qp->hw_qpn),
					UPPER_32_BITS(db_addr));

	/* 6. Write SQ completion DB addr */
	db_addr = xrnic_get_sq_db_addr(xib->xl, qp->hw_qpn);
	xrnic_iow(xib->xl, XRNIC_CQ_DB_ADDR_LSB(qp->hw_qpn), db_addr);
	xrnic_iow(xib->xl, XRNIC_CQ_DB_ADDR_MSB(qp->hw_qpn),
					UPPER_32_BITS(db_addr));

	/* 7. Disable the QP & Enable the SW override */
	val = xrnic_ior(xib->xl, XRNIC_QP_CONF(qp->hw_qpn));
	val &= ~(QP_ENABLE);
	xrnic_iow(xib->xl, XRNIC_QP_CONF(qp->hw_qpn), val);

	/* 8. Reset QP pointers to 0s */
	/* En SW override */
	val = xrnic_ior(xib->xl, XRNIC_ADV_CONF);
	val |= (XRNIC_SW_OVER_RIDE_EN << XRNIC_SW_OVER_RIDE_BIT);
	xrnic_iow(xib->xl, XRNIC_ADV_CONF, val);

	xrnic_iow(xib->xl, XRNIC_STAT_RQ_PROD_IDX(qp->hw_qpn), 0);
	xrnic_iow(xib->xl, XRNIC_RQ_CONS_IDX(qp->hw_qpn), 0);
	xrnic_iow(xib->xl, XRNIC_STAT_CUR_SQ_PTR(qp->hw_qpn), 0);
	xrnic_iow(xib->xl, XRNIC_SQ_PROD_IDX(qp->hw_qpn), 0);
	xrnic_iow(xib->xl, XRNIC_CQ_HEAD_PTR(qp->hw_qpn), 0);
	xrnic_iow(xib->xl, XRNIC_SNDQ_PSN(qp->hw_qpn), 0);
	xrnic_iow(xib->xl, XRNIC_LAST_RQ_PSN(qp->hw_qpn), 0);
	xrnic_iow(xib->xl, XRNIC_STAT_MSG_SQN(qp->hw_qpn), 0);

	/* 9. put QP under recovery */
	val = xrnic_ior(xib->xl, XRNIC_QP_CONF(qp->hw_qpn));
	val |= QP_ENABLE;
	xrnic_iow(xib->xl, XRNIC_QP_CONF(qp->hw_qpn), val);
	val &= ~(QP_UNDER_RECOVERY);
	xrnic_iow(xib->xl, XRNIC_QP_CONF(qp->hw_qpn), val);

	/* 10. Disable SW override */
	val = xrnic_ior(xib->xl, XRNIC_ADV_CONF);
	val &= ~(XRNIC_SW_OVER_RIDE_EN << XRNIC_SW_OVER_RIDE_BIT);
	xrnic_iow(xib->xl, XRNIC_ADV_CONF, val);
	return 0;
}


int xib_destroy_qp(struct ib_qp *ibqp, struct ib_udata *udata)
{
	struct xilinx_ib_dev *xib = get_xilinx_dev(ibqp->device);
	struct xib_qp *qp = get_xib_qp(ibqp);

        //printk("xib_destroy_qp num %d\n", ibqp->qp_num);
	switch (qp->qp_type) {
	case XIB_QP_TYPE_KERNEL:
		dev_dbg(&xib->ib_dev.dev, "%s : kernel\n", __func__);
		/* qp rst same for kernel/user qp */
		xrnic_reset_user_qp(qp);
		xrnic_qp_disable(qp);
		xib_dealloc_qp_buffers(ibqp->device, qp);
		kfree(qp->sq.wr_id_array);
		kfree(qp->sq.pl_buf_list);
		kfree(qp->rq.rqe_list);
		kfree(qp->imm_inv_data);
		xib->qp_list[qp->hw_qpn] = NULL;
		xib_bmap_release_id(&xib->qp_map, qp->hw_qpn);
		break;

	case XIB_QP_TYPE_USER:
		dev_dbg(&xib->ib_dev.dev, "%s : user\n", __func__);
		xrnic_reset_user_qp(qp);
		xrnic_qp_disable(qp);
		xib_dealloc_user_qp_buffers(ibqp->device, qp);
		xib_bmap_release_id(&xib->qp_map, qp->hw_qpn);
		break;

	case XIB_QP_TYPE_GSI:
		dev_dbg(&xib->ib_dev.dev, "%s : gsi\n", __func__);
		xrnic_reset_user_qp(qp);
		xrnic_qp_disable(qp);
		xib_dealloc_gsi_qp_buffers(ibqp->device, qp);
		kfree(qp->sq.wr_id_array);
		kfree(qp->sq.pl_buf_list);
		kfree(qp->rq.rqe_list);
		kfree(qp->imm_inv_data);
		xib->gsi_qp = NULL;
                tasklet_kill(&qp->comp_task);
                printk("killed gsi tasklet\n");
		break;
	default:
		dev_dbg(&xib->ib_dev.dev, "%s : unknown qp_type\n", __func__);
		break;
	}

	//kfree(qp); core now handling alloc and free of qp
	return 0;
}

static void hex_dump(unsigned char *buf, int len)
{
	while (len--)
		pr_cont("%02x", *buf++);
}

int xib_log(struct ib_send_wr *wr)
{
	u8 *in_data;
	u32 i, sge_len;
	void *sge_addr;

	for (i = 0; i < wr->num_sge; i++) {
		sge_addr = (void *)(unsigned long)
				wr->sg_list[i].addr;
		sge_len = wr->sg_list[i].length;
		memcpy(in_data, sge_addr, sge_len);
		hex_dump(in_data, sge_len);
	}
	return 0;
}


int xib_get_payload_size(struct ib_sge *sg_list, int num_sge)
{
	u32 i, total = 0;

	for (i = 0; i < num_sge; i++) {
		total += sg_list[i].length;
	}

	return total;
}


int xib_wr_code(int opcode)
{
	switch (opcode) {
	case IB_WR_SEND:
		return XRNIC_SEND_ONLY;
	case IB_WR_SEND_WITH_INV:
		/* TODO ernic doesnt support send_with_inv
		 * use send instead;
		 */
		return XRNIC_SEND_WITH_INV;
	case IB_WR_RDMA_READ:
		return XRNIC_RDMA_READ;
	case IB_WR_RDMA_WRITE:
		return XRNIC_RDMA_WRITE;
	case IB_WR_RDMA_WRITE_WITH_IMM:
		return XRNIC_RDMA_WRITE_WITH_IMM;
	case IB_WR_SEND_WITH_IMM:
		return XRNIC_SEND_WITH_IMM;
	default:
		return XRNIC_INVALID_OPC;
	}
}

int xib_prepare_sgl(u8 *buf, const struct ib_send_wr *wr)
{
	int i, size = 0;
	void *sgl_va;

	for (i = 0; i < wr->num_sge; i++) {
		if(size > XRNIC_SEND_SGL_SIZE) {
			pr_err("%s: send wr len size exceeds max\n",
					__func__);
			return -1;
		}
		sgl_va = (void *)phys_to_virt((unsigned long)wr->sg_list[i].addr);

		memcpy(buf, sgl_va, wr->sg_list[i].length);
		buf += wr->sg_list[i].length;
		size += wr->sg_list[i].length;
	}
	return size;
}

static int xib_gsi_post_send(struct ib_qp *ibqp, const struct ib_send_wr *wr,
		      const struct ib_send_wr **bad_wr)
{
	struct xib_qp *qp = get_xib_qp(ibqp);
	struct xilinx_ib_dev *xib = get_xilinx_dev(ibqp->device);
	struct xrnic_wr *xwqe;
	int ret = 0;
	unsigned long flags;
	int size;
	bool is_udp, is_vlan = 0;
	u8 ip_version;
        struct xib_sqd *temp;

	if (wr->opcode != IB_WR_SEND) {
		dev_err(&ibqp->device->dev,
			"opcode: %d not supported\n", wr->opcode);
		ret = -EINVAL;
		goto fail;
	}

	if (qp->send_sgl_busy) {
		dev_err(&ibqp->device->dev, "send sgl is busy");
		ret = -ENOMEM;
		goto fail;
	}

	spin_lock_irqsave(&qp->sq_lock, flags);
	if (qp->state == XIB_QP_STATE_SQD) {
		while (wr && (qp->sq.sqd_length < qp->sq.max_wr)) {
                        if(!(qp->sq.sqd_wr_list)) {
				qp->sq.sqd_wr_list = kzalloc(sizeof(struct xib_sqd), GFP_KERNEL);
                                if(!(qp->sq.sqd_wr_list))
                                        return -ENOMEM;
                                temp = qp->sq.sqd_wr_list;
                        } else {
                                temp = qp->sq.sqd_wr_list;
                                while(temp->next)
                                        temp = temp->next;
				temp->next = kzalloc(sizeof(struct xib_sqd), GFP_KERNEL);
                                if(!(temp->next))
                                        return -ENOMEM;
		                temp = temp->next;
                        }
                	temp->wr_id = wr->wr_id;
                	temp->next = NULL;
                	wr = wr->next;
                        qp->sq.sqd_length++;
		}
                if(qp->sq.sqd_length > qp->sq.max_wr) {
			pr_err("%s: Number of work requests in SQD exceeded maximum limit \n", __func__);
                	return -ENOMEM;
		}
        } else {
		while (wr) {
			u8 *buf = (u8 *)qp->send_sgl_v;

			xwqe = (struct xrnic_wr *)((unsigned long)(qp->sq_ba_v) +
					qp->sq.sq_cmpl_db_local * sizeof(*xwqe));

			xwqe->wrid = (wr->wr_id) & XRNIC_WR_ID_MASK; /* TODO wrid is 64b
								       but xrnic wrid is
								       2 bytes*/
			qp->sq.wr_id_array[qp->sq.sq_cmpl_db_local].wr_id = wr->wr_id;
			qp->sq.wr_id_array[qp->sq.sq_cmpl_db_local].signaled =
							!!(wr->send_flags & IB_SEND_SIGNALED);

			ret = xib_build_qp1_send_v2(ibqp, wr,
					xib_get_payload_size(wr->sg_list, wr->num_sge),
					&is_udp,
					&ip_version);
			if (ret < 0) {
				dev_dbg(&ibqp->device->dev, "%s: xib_build_qp1_send_v2 failed\n",
						__func__);
				spin_unlock_irqrestore(&qp->sq_lock, flags);
				goto fail;
			}
			/* set dest ip address */
			if (ip_version == 6)
				config_raw_ip(xib->xl, XRNIC_IPV6_ADD_1, (u32 *)&qp->qp1_hdr.grh.source_gid,
						1);
			else
				config_raw_ip(xib->xl, XRNIC_IPV4_ADDR,
						(u32 *)&qp->qp1_hdr.ip4.saddr, 0);

			/* for UD header is needed by HW */
			size = ib_ud_header_pack(&qp->qp1_hdr, buf);
			buf = buf + size;

			size += xib_prepare_sgl(buf, wr);
			if (size < 0 ) {
				spin_unlock_irqrestore(&qp->sq_lock, flags);
				ret = -ENOMEM;
				goto fail;
			}

			#if 0 /* TODO handle vlan ? */
			if (is_udp && ip_version == 4)
				size -= 20;
			if (!is_udp)
				size -= 8;
			if(!is_vlan)
				size -= 4;
			#endif

			xwqe->l_addr = qp->send_sgl_p;
			xwqe->r_offset = 0;
			xwqe->r_tag = 0;
			xwqe->length = size;
			xwqe->opcode = XRNIC_SEND_ONLY;
			xrnic_send_wr(qp, xib);

			wr = wr->next;
		}
	}

	spin_unlock_irqrestore(&qp->sq_lock, flags);
	return 0;
fail:
	*bad_wr = wr;
	return ret;
}

static int __xib_post_send(struct ib_qp *ibqp, const struct ib_send_wr *wr,
		      const struct ib_send_wr **bad_wr)
{
	struct xib_qp *qp = get_xib_qp(ibqp);
	struct xrnic_wr xwqe;
	void *dest;
	struct xilinx_ib_dev *xib = get_xilinx_dev(ibqp->device);
	int ret = 0, j = 0;
	char *from;
	unsigned long flags;
	int size;
        struct xib_sqd *temp;

	qp->post_send_count++;
	spin_lock_irqsave(&qp->sq_lock, flags);
	if (qp->state == XIB_QP_STATE_SQD) {
		while (wr && (qp->sq.sqd_length < qp->sq.max_wr)) {
                        if(!(qp->sq.sqd_wr_list)) {
				qp->sq.sqd_wr_list = kzalloc(sizeof(struct xib_sqd), GFP_KERNEL);
                                if(!(qp->sq.sqd_wr_list))
                                        return -ENOMEM;
                                temp = qp->sq.sqd_wr_list;
                        } else {
                                temp = qp->sq.sqd_wr_list;
                                while(temp->next)
                                        temp = temp->next;
				temp->next = kzalloc(sizeof(struct xib_sqd), GFP_KERNEL);
                                if(!(temp->next))
                                        return -ENOMEM;
		                temp = temp->next;
                        }
                	temp->wr_id = wr->wr_id;
                	temp->next = NULL;
                	wr = wr->next;
                        qp->sq.sqd_length++;
		}
                if(qp->sq.sqd_length > qp->sq.max_wr) {
			pr_err("%s: Number of work requests in SQD exceeded maximum limit \n", __func__);
                	return -ENOMEM;
		}
        } else {
		while (wr) {
			xwqe.wrid = (wr->wr_id) & XRNIC_WR_ID_MASK; /* TODO wrid is 64b
								       but xrnic wrid is
								       2 bytes*/
			qp->sq.wr_id_array[qp->sq.sq_cmpl_db_local].wr_id = wr->wr_id;
			qp->sq.wr_id_array[qp->sq.sq_cmpl_db_local].signaled =
							!!(wr->send_flags & IB_SEND_SIGNALED);

			if ((wr->opcode == IB_WR_SEND) || (wr->opcode == IB_WR_SEND_WITH_INV) ||
					(wr->opcode == IB_WR_SEND_WITH_IMM)) {
				u8 *buf;
				struct xib_pl_buf *plb;

				plb = &qp->sq.pl_buf_list[qp->sq.sq_cmpl_db_local];

				plb->len = xib_get_payload_size(wr->sg_list, wr->num_sge);

				if (xib_pl_present())
					from = "pl";
				else
					from = "ps";

				plb->va = xib_alloc_coherent(from, xib,
							plb->len,
							&plb->pa,
							GFP_KERNEL);
				if (!plb->va) {
					spin_unlock_irqrestore(&qp->sq_lock, flags);
					dev_err(&ibqp->device->dev, "failed to alloc rdma rd/wr mem\n");
					ret = -ENOMEM;
					goto fail;
				}

				buf = (u8 *)plb->va;

				size = xib_prepare_sgl(buf, wr);
				if (size < 0 ) {
					ret = -EINVAL;
					spin_unlock_irqrestore(&qp->sq_lock, flags);
					goto fail;
				}
				if (size <= XRNIC_MAX_SDATA) {  /* inline data */
					memcpy(xwqe.sdata, buf, size);
					size = XRNIC_MAX_SDATA;
				}

#ifdef ARCH_HAS_	PS
				if (strcasecmp(from, "ps") == 0) {
					xwqe.l_addr = plb->pa;
				} else {
					/* program only the lower 32
					* as hw assumes the upper
					*/
					xwqe.l_addr = lower_32_bits(plb->pa);
				}
#else
				xwqe.l_addr = plb->pa;
#endif
				xwqe.r_offset = 0;
				xwqe.r_tag = 0;
			} else {
#ifdef ARCH_HAS_	PS
				if (strcasecmp(sq_mem, "ps") == 0) {
					xwqe.l_addr = wr->sg_list[0].addr;
				} else {
					/* even if sq_mem is requested from bram
					* for these admin path we dont really
					* need bram
					*/
					struct xib_pl_buf *plb;
					void *sgl_va;

					plb = &qp->sq.pl_buf_list[qp->sq.sq_cmpl_db_local];

					BUG_ON(!xib_pl_present());

					plb->va = xib_alloc_coherent("pl", xib,
							wr->sg_list[0].length,
							&plb->pa,
							GFP_KERNEL);
					if (!plb->va) {
						spin_unlock_irqrestore(&qp->sq_lock, flags);
						dev_err(&ibqp->device->dev, "failed to alloc rdma rd/wr mem\n");
						ret = -ENOMEM;
						goto fail;
					}
					/* program only the lower 32
					* as hw assumes the upper
					*/
					xwqe.l_addr = lower_32_bits(plb->pa);
					plb->sgl_addr = wr->sg_list[0].addr;
					plb->len = wr->sg_list[0].length;
					if (wr->opcode == IB_WR_RDMA_WRITE) {
						sgl_va = (void *)phys_to_virt(
								(unsigned long) plb->sgl_addr);
						memcpy(plb->va, sgl_va, plb->len);
					}
				}
#else
				xwqe.l_addr = lower_32_bits(wr->sg_list[0].addr);
#endif
				size = wr->sg_list[0].length;
			}

			xwqe.length = size;

			if (wr->opcode == IB_WR_RDMA_WRITE ||
					wr->opcode == IB_WR_RDMA_READ) {
				xwqe.r_offset = rdma_wr(wr)->remote_addr;
				xwqe.r_tag = rdma_wr(wr)->rkey;
			}

			xwqe.opcode = xib_wr_code(wr->opcode);
			if (xwqe.opcode < 0) {
				dev_err(&ibqp->device->dev,
						"opcode: %d not supported\n", wr->opcode);
				ret = -EINVAL;
				spin_unlock_irqrestore(&qp->sq_lock, flags);
				goto fail;
			}
			dest = (void *)(qp->sq_ba_v +
					qp->sq.sq_cmpl_db_local * sizeof(struct xrnic_wr));

			if (qp->io_qp && strcasecmp(sq_mem, "bram") == 0) {
				printk("doing memcpy_toio\n");
				memcpy_toio(dest, &xwqe, sizeof(struct xrnic_wr));
			} else
				memcpy(dest, &xwqe, sizeof(struct xrnic_wr));

			if ((wr->opcode == IB_WR_SEND_WITH_IMM) | (wr->opcode == IB_WR_RDMA_WRITE_WITH_IMM))
				/* xwqe.imm_data */
				xwqe.imm_data = be32_to_cpu(wr->ex.imm_data);

			if (wr->opcode == IB_WR_SEND_WITH_INV)
				xwqe.r_tag = wr->ex.invalidate_rkey;

			xrnic_send_wr(qp, xib);

			wmb();

			wr = wr->next;
		}
	}

	spin_unlock_irqrestore(&qp->sq_lock, flags);
	return 0;
fail:
	*bad_wr = wr;
	return ret;
}

/*
 * the operations supported by ernic on send queue:
 * 1. Send
 * 2. RDMA Write
 * 3. RDMA Read
 *   Atomic, Bind memory Window, Local invalidate
 *  Fast Register Physical MR are not supported.
 */
int xib_post_send(struct ib_qp *ibqp, const struct ib_send_wr *wr,
		      const struct ib_send_wr **bad_wr)
{
	struct ib_send_wr *twr = wr;
	dev_dbg(&ibqp->device->dev, "%s : <---------- \n", __func__);

	if(ibqp->qp_type == IB_QPT_GSI)
		return xib_gsi_post_send(ibqp, wr, bad_wr);
	else
		return __xib_post_send(ibqp, wr, bad_wr);
}


/*
 *
 */
int xib_create_cq(struct ib_cq *ibcq,
			const struct ib_cq_init_attr *attr,
			struct ib_udata *udata)
{
	struct ib_device *ibdev = ibcq->device;
	struct xilinx_ib_dev *xib = get_xilinx_dev(ibdev);
	struct xilinx_ib_dev_attr *dev_attr = &xib->dev_attr;
	struct xib_cq *cq = get_xib_cq(ibcq);
	int cqe = attr->cqe;
	int entries;

	dev_dbg(&xib->ib_dev.dev, "%s : <---------- \n", __func__);

	dev_dbg(&xib->ib_dev.dev, "cqe : %d, dev_attr->max_cq_wqes: %d\n", cqe,
			dev_attr->max_cq_wqes);

	if (cqe < 1 || cqe > dev_attr->max_cq_wqes) {
		dev_err(&xib->ib_dev.dev, "Failed to create CQ -max exceeded");
		return ERR_PTR(-EINVAL);
	}

	cq->ib_cq.cqe = cqe;
	entries = roundup_pow_of_two(cqe + 1);
	if (entries > dev_attr->max_cq_wqes + 1)
		entries = dev_attr->max_cq_wqes + 1;

	if (udata) {
		dev_dbg(&xib->ib_dev.dev, "user mode cq\n");
		cq->cq_type = XIB_CQ_TYPE_USER;
	} else {
		dev_dbg(&xib->ib_dev.dev, "kernel mode cq\n");
		cq->cq_type = XIB_CQ_TYPE_KERNEL;
		cq->buf_v = xib_zalloc_coherent(cq_mem, xib,
				cqe * sizeof(struct xrnic_cqe),
				&cq->buf_p, GFP_KERNEL);
		if (!cq->buf_v) {
			dev_err(&ibdev->dev, "failed to alloc sq dma mem\n");
			goto fail;
		}
		dev_dbg(&xib->ib_dev.dev, "%s: cq->buf_v: %px cq->buf_p : %x ", __func__, cq->buf_v,
			cq->buf_p);
		spin_lock_init(&cq->cq_lock);
	}

	return 0;
fail:
	return -EFAULT;
}

int xib_modify_cq(struct ib_cq *cq, u16 cq_count, u16 cq_period)
{
	dev_dbg(&cq->device->dev, "%s : <---------- \n", __func__);

	return 0;

}

int xib_poll_cq(struct ib_cq *ibcq, int num_entries, struct ib_wc *wc)
{
	struct xib_cq *cq = get_xib_cq(ibcq);

	if (cq->cq_type == XIB_CQ_TYPE_GSI) {
		return xib_gsi_poll_cq(ibcq, num_entries, wc);
	}
	else if(cq->cq_type == XIB_CQ_TYPE_KERNEL) {
		return xib_poll_kernel_cq(ibcq, num_entries, wc);
	}

	return 0;
}

int xib_arm_cq(struct ib_cq *ibcq, enum ib_cq_notify_flags flags)
{
	dev_dbg(&ibcq->device->dev, "%s : <---------- \n", __func__);
	return 0;
}

struct ib_mr *xib_get_dma_mr(struct ib_pd *pd, int acc)
{
	struct xib_mr *mr;

	dev_dbg(&pd->device->dev, "%s : <---------- \n", __func__);

	mr = kzalloc(sizeof(*mr), GFP_KERNEL);
	if (!mr)
		return ERR_PTR(-ENOMEM);

	mr->type = XIB_MR_DMA;
	mr->ib_mr.rkey = mr->ib_mr.lkey = 0x101;
	return &mr->ib_mr;
}

int xib_kdereg_mr(struct ib_mr *ibmr)
{
	struct xib_mr *mr = get_xib_mr(ibmr);
	struct xilinx_ib_dev *xib = get_xilinx_dev(ibmr->device);

	spin_lock_bh(&xib->lock);
	xib_bmap_release_id(&xib->mr_map, mr->mr_idx);
	spin_unlock_bh(&xib->lock);

	return 0;
}
EXPORT_SYMBOL(xib_kdereg_mr);

int xib_dereg_mr(struct ib_mr *ibmr, struct ib_udata *udata)
{
	struct xib_mr *mr = get_xib_mr(ibmr);
	struct xilinx_ib_dev *xib = get_xilinx_dev(ibmr->device);
#undef	KERNEL_MR_ALLOC_IMPL

	dev_dbg(&xib->ib_dev.dev, "%s : <---------- \n", __func__);

	/*TODO: even the kernel apps uses a wrapper around the
		reg_user_mr implementaion thus using the mr bit.
		Until there is an implementation for alloc_mr
		this should be in place */

#ifdef KERNEL_MR_ALLOC_IMPL
	if (mr->type == XIB_MR_USER) {
#endif
		spin_lock_bh(&xib->lock);
		xib_bmap_release_id(&xib->mr_map, mr->mr_idx);
		spin_unlock_bh(&xib->lock);
#ifdef KERNEL_MR_ALLOC_IMPL
	}
	if (mr->umem)
		ib_umem_release(mr->umem);
#endif
	return 0;
}

static int xib_port_immutable(struct ib_device *ibdev, u32 port_num,
			       struct ib_port_immutable *immutable)
{
	dev_dbg(&ibdev->dev, "%s : port_num: %d <---------- \n", __func__, port_num);

	immutable->pkey_tbl_len = 1;
	immutable->gid_tbl_len = 128; /* TODO */
	//immutable->core_cap_flags = RDMA_CORE_PORT_IBA_ROCE;
	immutable->core_cap_flags = RDMA_CORE_PORT_IBA_ROCE_UDP_ENCAP;
	immutable->max_mad_size = IB_MGMT_MAD_SIZE;
	return 0;
}

int xib_destroy_cq(struct ib_cq *ibcq, struct ib_udata *udata)
{
	struct xilinx_ib_dev *xib = get_xilinx_dev(ibcq->device);
	struct xib_cq *cq = get_xib_cq(ibcq);

	dev_dbg(&ibcq->device->dev, "%s : <---------- \n", __func__);

	if (cq->cq_type == XIB_CQ_TYPE_KERNEL) {
		xib_free_coherent(xib,
				ibcq->cqe * sizeof(struct xrnic_cqe),
				cq->buf_v, cq->buf_p);
	}
	return 0;
}

#define QP_ERR_INFO_SIZE (8)
static int xlnx_ernic_remove (struct platform_device *pdev)
{
	struct xilinx_ib_dev *xib = platform_get_drvdata(pdev);
	unsigned int rtr_count = 1024;
	struct xrnic_local *xl;

	dev_dbg(&xib->ib_dev.dev, "%s : <---------- \n", __func__);

	unregister_netdevice_notifier(&cmac_netdev_notifier);
	unregister_inetaddr_notifier(&cmac_inetaddr_notifier);
	unregister_inet6addr_notifier(&cmac_inet6addr_notifier);
	kobject_put(ibdev->pfc_kobj);
	axidma_exit(xib);
#ifdef CONFIG_MICROBLAZE
	axitimer_exit();
#endif
	ib_unregister_device(&xib->ib_dev);

	/* free rtr buffers */
	xl = xib->xl;

	if (xl->retry_buf_va)
		xib_free_coherent(xib, (rtr_count * XRNIC_SIZE_OF_DATA_BUF),
				xl->retry_buf_va, xl->retry_buf_pa);
	/* free incoming error pkt buffer space */
	if (xl->in_pkt_err_va)
		xib_free_coherent(xib, (ibdev->dev_attr.max_qp * QP_ERR_INFO_SIZE),
				xl->in_pkt_err_va, xl->in_pkt_err_ba);
	/* free SQ, RQ DB area */

	xrnic_hw_deinit(xib);

	return 0;
}

#ifdef ARCH_HAS_PS
struct device *xlnx_pl_alloc_get_device(void);
#endif

static const struct ib_device_ops xib_dev_ops = {
	.owner	= THIS_MODULE,
	.driver_id = RDMA_DRIVER_XLNX,
	.uverbs_abi_ver	= 1,

	.query_device	= xib_query_device,
	.query_port	= xib_query_port,
	.query_pkey	= xib_query_pkey,
	.alloc_ucontext	= xib_alloc_ucontext,
	.dealloc_ucontext = xib_dealloc_ucontext,
	.mmap	= xib_mmap,
//	.query_gid	= xib_query_gid,
	.add_gid	= xib_add_gid,
	.del_gid	= xib_del_gid,
	.alloc_pd	= xib_alloc_pd,
	.alloc_mr	= xib_alloc_mr,
	.map_mr_sg	= xib_map_mr_sge,
	.dealloc_pd	= xib_dealloc_pd,
	.get_link_layer	= xib_get_link_layer,
	.get_netdev	= xib_get_netdev,
	.create_ah	= xib_create_ah,
	.destroy_ah	= xib_destroy_ah,
	.create_qp	= xib_create_qp,
	.modify_qp	= xib_modify_qp,
	.query_qp	= xib_query_qp,
	.destroy_qp	= xib_destroy_qp,
	.post_send	= xib_post_send,
	.drain_sq	= xib_drain_sq,
	.drain_rq	= xib_drain_rq,
	.post_recv	= xib_post_recv,
	.create_cq	= xib_create_cq,
	.destroy_cq	= xib_destroy_cq,
	.modify_cq	= xib_modify_cq,
	.poll_cq	= xib_poll_cq,
	.req_notify_cq = xib_arm_cq,
	.get_dma_mr	= xib_get_dma_mr,
	.dereg_mr	= xib_dereg_mr,
	.get_port_immutable	= xib_port_immutable,

	.reg_user_mr	= xib_reg_user_mr,
	.reg_user_mr_ex	= xib_reg_user_mr,
	INIT_RDMA_OBJ_SIZE(ib_ah, xib_ah, ib_ah),
        INIT_RDMA_OBJ_SIZE(ib_cq, xib_cq, ib_cq),
	INIT_RDMA_OBJ_SIZE(ib_pd, xib_pd, ib_pd),
	INIT_RDMA_OBJ_SIZE(ib_qp, xib_qp, ib_qp),
	INIT_RDMA_OBJ_SIZE(ib_ucontext, xib_ucontext, ib_uc),
};

void xib_set_dev_caps(struct ib_device *ibdev)
{
	ibdev->phys_port_cnt		= 1;
	ibdev->num_comp_vectors		= 1;
	ibdev->local_dma_lkey		= 0;
	ibdev->uverbs_cmd_mask		=
		(1ULL << IB_USER_VERBS_CMD_GET_CONTEXT) |
		(1ULL << IB_USER_VERBS_CMD_QUERY_DEVICE) |
		(1ULL << IB_USER_VERBS_CMD_QUERY_PORT) |
		(1ULL << IB_USER_VERBS_CMD_ALLOC_PD) |
		(1ULL << IB_USER_VERBS_CMD_DEALLOC_PD) |
		(1ULL << IB_USER_VERBS_CMD_REG_MR) |
		(1ULL << IB_USER_VERBS_CMD_REG_MR_EX) |
		(1ULL << IB_USER_VERBS_CMD_DEREG_MR) |
		(1ULL << IB_USER_VERBS_CMD_CREATE_COMP_CHANNEL) |
		(1ULL << IB_USER_VERBS_CMD_CREATE_CQ) |
		(1ULL << IB_USER_VERBS_CMD_DESTROY_CQ) |
		(1ULL << IB_USER_VERBS_CMD_CREATE_QP) |
		(1ULL << IB_USER_VERBS_CMD_MODIFY_QP) |
		(1ULL << IB_USER_VERBS_CMD_QUERY_QP) |
		(1ULL << IB_USER_VERBS_CMD_DESTROY_QP);
}

static int check_queue_mem_types(void)
{
	bool stat = false;

	/* check if the provided SQ type has processor access */
	stat = get_xib_mem_accessible_status(xib_mem_str_to_type(sq_mem));
	if (!stat) {
		pr_err("SQ memory type [%s] should be available and "
			"accessible to processor\n", sq_mem);
		return -EFAULT;
	}

	stat = get_xib_mem_accessible_status(xib_mem_str_to_type(cq_mem));
	if (!stat) {
		pr_err("CQ memory type [%s] should be available and "
			"accessible to processor\n", cq_mem);
		return -EFAULT;
	}

	stat = get_xib_mem_accessible_status(xib_mem_str_to_type(rq_mem));
	if (!stat) {
		pr_err("RQ memory type [%s] should be available and "
			"accessible to processor\n", rq_mem);
		return -EFAULT;
	}

	return 0;
}

static int xlnx_ernic_probe(struct platform_device *pdev)
{
	int err;
	struct device_node *net_node, *np;
	struct xrnic_local *xl;
	unsigned long debug_phys;
	struct net_device *netdev;
	struct resource resource;
	u32 qpn, rtr_count;
	u64 rtr_addr = 0;

	dev_dbg(&pdev->dev, "%s : <---------- \n", __func__);

	if (err = check_queue_mem_types())
		return err;

#ifdef ARCH_HAS_PS
	xib_register_pl_allocator(xlnx_pl_alloc_get_device());
#endif

	xib_bram_init();
	xib_ext_ddr_init();
	ibdev = (struct xilinx_ib_dev *)ib_alloc_device(xilinx_ib_dev, ib_dev);
	if(!ibdev) {
		dev_err(&pdev->dev, "cant alloc ibdev\n");
		return -ENOMEM;
	}

	err = of_property_read_u32(pdev->dev.of_node, "xlnx,num-qp",
			&ibdev->dev_attr.max_qp);
        if (err) {
                ibdev->dev_attr.max_qp = 32;
                pr_debug("num-qp entry not found. Configuring max qp's to [%d]\n",
                        ibdev->dev_attr.max_qp);
        }

	ibdev->mtu = QP_PMTU_4096;
	ibdev->pdev = pdev;
	platform_set_drvdata(pdev, ibdev);
	xl = xrnic_hw_init(pdev, ibdev);
	if (!xl) {
		dev_err(&pdev->dev, "xrnic init failed\n");
		return -ENODEV;
	}
	ibdev->xl = xl;
	xl->xib = ibdev;

	net_node = of_parse_phandle(pdev->dev.of_node, "eth-handle", 0);

	ibdev->netdev = of_find_net_device_by_node(net_node);

	if(!ibdev->netdev) {
		dev_err(&pdev->dev, "no netdev found\n");
		return -EINVAL;
	}

	err = update_mtu(ibdev->netdev);
        np = of_find_node_by_name(NULL, "ernic");
        if (!np) {
                dev_err(&pdev->dev, "Unable to find the ernic node\n");
                return -EFAULT;
        }

	netdev = ibdev->netdev;

	xib_get_guid(netdev->dev_addr, (u8 *)&ibdev->ib_dev.node_guid);

	xl->num_intr_banks = ibdev->dev_attr.max_qp / 32;
	pr_info("Number of interrupt banks: %u\n", xl->num_intr_banks);


	err = of_property_read_u32(pdev->dev.of_node, "xlnx,num-pd",
			&ibdev->dev_attr.max_pd);
	if (err) {
		ibdev->dev_attr.max_pd = 256;
	}

	err = of_property_read_u32(pdev->dev.of_node, "xlnx,max-sgl-depth",
			&ibdev->dev_attr.max_send_sge);

	dev_dbg(&pdev->dev, "%s: qp:%d pd:%d sgl_depth:%d \n", __func__, ibdev->dev_attr.max_qp,
			ibdev->dev_attr.max_pd,	ibdev->dev_attr.max_send_sge);

	ibdev->dev_attr.max_cq_wqes	= 1024;
	ibdev->dev_attr.max_mr		= ibdev->dev_attr.max_pd;

	/* initialize retry bufs
	* qp 0 doesnt exist
	* qp 1 is UD so no rtr buf needed
	*/
#if 0
	rtr_count = (ibdev->dev_attr.max_qp - 2 )*16;
#else
	rtr_count = 1024;
#endif
	xl->retry_buf_va = xib_alloc_coherent(rtr_mem, ibdev,
		(rtr_count * XRNIC_SIZE_OF_DATA_BUF),
		&rtr_addr,
		GFP_KERNEL);
	if (!rtr_addr) {
		dev_err(&pdev->dev, "Failed to allocate rtr bufs\n");
		return -ENOMEM;
	}
	xl->retry_buf_pa = rtr_addr;

	xrnic_iow(xl, XRNIC_DATA_BUF_BASE_LSB, rtr_addr);
	xrnic_iow(xl, XRNIC_DATA_BUF_BASE_MSB, UPPER_32_BITS(rtr_addr));
	wmb();
	xrnic_iow(xl, XRNIC_DATA_BUF_SZ,
		rtr_count | ( XRNIC_SIZE_OF_DATA_BUF << 16));
	wmb();

	/* initialize in_pkt_errq bufs */
	xl->in_pkt_err_va = dma_alloc_coherent(&pdev->dev,
			(ibdev->dev_attr.max_qp * 8), &xl->in_pkt_err_ba, GFP_KERNEL);
	if (!xl->in_pkt_err_ba) {
		dev_err(&pdev->dev, "Failed to allocate in_pkt_err bufs\n");
		return -ENOMEM;
	}
	xrnic_iow(xl, XRNIC_INCG_PKT_ERRQ_BASE_LSB, xl->in_pkt_err_ba);
	xrnic_iow(xl, XRNIC_INCG_PKT_ERRQ_BASE_MSB,
					UPPER_32_BITS(xl->in_pkt_err_ba));
	wmb();
	/* each entry is 8 bytes, max sz = 64  */
	xrnic_iow(xl, XRNIC_INCG_PKT_ERRQ_SZ, ibdev->dev_attr.max_qp);
	wmb();

	spin_lock_init(&ibdev->lock);

	/* allocate qp list */
	ibdev->qp_list = (struct xib_qp **) kzalloc
			((sizeof(struct xib_qp *) * ibdev->dev_attr.max_qp), GFP_KERNEL);

	/* alloc pd bmap */
	xib_bmap_alloc(&ibdev->pd_map, ibdev->dev_attr.max_pd, "PD");
	/* alloc qp bmap */
	xib_bmap_alloc(&ibdev->qp_map, ibdev->dev_attr.max_qp, "QP");
	/* alloc mr bmap */
	xib_bmap_alloc(&ibdev->mr_map, ibdev->dev_attr.max_mr, "MR");

	/*
	*  TODO do we need to pass PL DDR for infiniband core
	* allocations?
	*/
	ibdev->ib_dev.dev.parent = &pdev->dev;

	strlcpy(ibdev->ib_dev.name, "xib_%d", IB_DEVICE_NAME_MAX);
	ibdev->ib_dev.node_type	= RDMA_NODE_IB_CA;

	xib_set_dev_caps(&ibdev->ib_dev);
	ib_set_device_ops(&ibdev->ib_dev, &xib_dev_ops);

	err = ib_register_device(&ibdev->ib_dev, ibdev->ib_dev.name, &pdev->dev);
	if (err) {
		dev_err(&pdev->dev, "failed to regiser xib device\n");
		goto err_1;
	}

	/* set dma mask */
	/* TODO set relevant mask for 64bit and 32 bit */
	err = dma_set_mask_and_coherent(&ibdev->pdev->dev, DMA_BIT_MASK(32));
	if (err != 0)
		dev_err(&pdev->dev, "unable to set dma mask\n");

	/* the phy attached to 40G is not giving out active speed or width
	 * since we are using 40G in the design default to 40G */ /*TODO */
	#if 0
	ib_get_eth_speed(&ibdev->ib_dev, 1, &ibdev->active_speed,
			&ibdev->active_width);
	#endif
	ibdev->active_speed = IB_SPEED_FDR10;
	ibdev->active_width = IB_WIDTH_4X;

	/* set the mac address */
	xrnic_set_mac(xl, netdev->dev_addr);

	err = set_ip_address(netdev, 1);
	err = set_ip_address(netdev, 0);

	/* pre-reserve QP1
	 * there is no QP0 in ernic HW
	 */
	spin_lock_bh(&ibdev->lock);
	xib_bmap_alloc_id(&ibdev->qp_map, &qpn);
	spin_unlock_bh(&ibdev->lock);

	dev_dbg(&pdev->dev, "gsi qpn: %d\n", qpn);

	if(pfc_create_sysfs_entries("pfc", &ibdev->ib_dev.dev.kobj,
			&pfc_attr_group, &ibdev->pfc_kobj)) {
		dev_err(&pdev->dev, "Failed to create PFC sysfs entry\n");
		goto err_2;
	}

	/* register irq */
	err = request_irq(xl->irq, xib_irq, IRQF_SHARED, "xrnic_intr0",
			(void *)ibdev);
	if (err) {
		dev_err(&pdev->dev, "request irq error!\n");
		goto err_3;
	}

	/* start ernic HW */
	xl->qps_enabled = (ibdev->dev_attr.max_qp); //changed for ernic 4.2
	xl->udp_sport = 0x8cd1;
	xrnic_start(xl);
	/* register for net dev notifications */
	register_netdevice_notifier(&cmac_netdev_notifier);
	register_inetaddr_notifier(&cmac_inetaddr_notifier);
	register_inet6addr_notifier(&cmac_inet6addr_notifier);

	axidma_init(ibdev, pdev->dev.of_node);
#ifdef CONFIG_MICROBLAZE
	axitimer_init(pdev->dev.of_node);
#endif

	return 0;
err_3:
	kobject_put(ibdev->pfc_kobj);
err_2:
	ib_unregister_device(&ibdev->ib_dev);
err_1:
	return err;
}


static const struct xlnx_ernic_config xlnx_ernic_v1_config = {
	.dummy = 1,
};

static const struct of_device_id xlnx_of_match[] = {
	{ .compatible = "xlnx,ernic-1.0", .data = &xlnx_ernic_v1_config},
	{ .compatible = "xlnx,ernic-2.0", .data = &xlnx_ernic_v1_config},
	{ .compatible = "xlnx,ernic-3.0", .data = &xlnx_ernic_v1_config},
	{ .compatible = "xlnx,ernic-3.1", .data = &xlnx_ernic_v1_config},
	{ .compatible = "xlnx,ernic-4.0", .data = &xlnx_ernic_v1_config},
	{ .compatible = "xlnx,ernic-4.2", .data = &xlnx_ernic_v1_config},
	{},
};
MODULE_DEVICE_TABLE(of, xlnx_of_match);

static struct platform_driver xlnx_ernic_driver = {
	.probe = xlnx_ernic_probe,
	.remove = xlnx_ernic_remove,
	.driver = {
		.name = "xlnx_ernic",
		.of_match_table = xlnx_of_match,
	},
};

module_platform_driver(xlnx_ernic_driver);

MODULE_AUTHOR("syed s <syeds@xilinx.com>");
MODULE_DESCRIPTION("Xilinx ERNIC IB driver");
MODULE_LICENSE("GPL");
