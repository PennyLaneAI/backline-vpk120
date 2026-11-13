/*
 * AMD FPGA AMD ERNIC kernel memory manager driver
 *
 * Copyright (C) 2024 AMD, Inc. All rights reserved.
 * Copyright 2026 Xanadu Quantum Technologies Inc.
 *
 * Author: Ravali P <ravalip@xilinx.com>
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
#include <linux/mm.h>
#include <linux/device.h>
#include <linux/cdev.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/of_platform.h>
#include <linux/mutex.h>
#include <linux/of_address.h>
#include <linux/ioctl.h>
#include <linux/dma-mapping.h>
#include <linux/of_reserved_mem.h>
#include <linux/iommu.h>
#include <linux/memremap.h>
#include <linux/ioport.h>
#include <linux/pfn_t.h>
#include <linux/mmzone.h>
#include "xib_kmm.h"
#include "xib_kmm_ioctl.h"

struct xib_kmem_info *mmr = NULL;
struct list_head mmr_head = LIST_HEAD_INIT(mmr_head);
struct list_head *chunk_head;
struct cdev 	*cdevice;
struct device 	*dev;
dev_t dev_num;
struct class *cl;
int chunk_size;
volatile unsigned int file_open_cnt = 0;
spinlock_t mm_list_lock;
struct xib_kmm_ctx kmm_ctx;

static struct file_operations fops =
{
	.write = xib_write,
	.read = xib_read,
	.unlocked_ioctl = xib_ioctl,
	.mmap = xib_mmap,
	.release = xib_close,
	.open = xib_open
};

static void xpagemap_page_free(struct page *page)
{
        wake_up_var(&page->_refcount);
}

static const struct dev_pagemap_ops xpagemap_ops = {
	.page_free	= xpagemap_page_free,
};

void vma_open(struct vm_area_struct *vma)
{
	printk("%s va %lx, pa %lx\n", __func__,
			vma->vm_start, vma->vm_pgoff << PAGE_SHIFT);
}

void vma_close(struct vm_area_struct *vma)
{
	;
}

struct page **xkmm_get_page_info(uint64_t pa, unsigned int *pg_idx) {
	struct xib_kmem_info *curr, *tmp, *node = NULL;
	uint64_t va;

	list_for_each_entry_safe(curr, tmp, &mmr_head, mmr_list) {
		if (curr->base_addr <= pa && ((curr->base_addr + curr->size) >= pa)) {
			*pg_idx = (((pa - curr->base_addr) + (PAGE_SIZE - 1)) / PAGE_SIZE);
			return curr->pages;
		}
	}
	return NULL;
}

#ifndef CONFIG_MICROBLAZE
static unsigned int vma_nopage(struct vm_fault *vmf)
{
	struct page *pageptr, **pg_info;
	struct vm_area_struct *vma = vmf->vma;
	unsigned long vmf_address = vmf->address;
	unsigned int pg_idx, ret, npages;
	unsigned long offset = vma->vm_pgoff << PAGE_SHIFT;
	unsigned long physaddr = (unsigned long) vmf->address - vma->vm_start + offset;

	pg_info = xkmm_get_page_info(physaddr, &pg_idx);
	if (!pg_info) {
		printk("Invalid ptr %s:%d\n", __func__, __LINE__);
		return -EINVAL;
	}

	npages = (vma->vm_end - vma->vm_start) / PAGE_SIZE;
	vma->vm_page_prot = pgprot_noncached(vma->vm_page_prot);
	while (npages) {
		ret = vm_insert_page(vma, vmf_address, pg_info[pg_idx]);

		switch (ret) {
		case -EAGAIN:
		case 0:
		case -ERESTARTSYS:
			return VM_FAULT_NOPAGE;
		case -ENOMEM:
			return VM_FAULT_OOM;
		default:
			return VM_FAULT_SIGBUS;
		}
		pg_idx++;
		npages--;
	}

	return 0;
}
#endif

static struct vm_operations_struct nopage_vm_ops = {
	.open =   vma_open,
	.close =  vma_close,
#ifndef CONFIG_MICROBLAZE
	.fault = vma_nopage,
#endif
};

static int xib_mmap(struct file *fp, struct vm_area_struct *vma)
{
	ssize_t length;

	if ((vma->vm_flags & VM_MAYSHARE) != VM_MAYSHARE) {
		pr_info("mmap failed: can't create private mapping\n");
		return -EINVAL;
	}
	vma->vm_page_prot = pgprot_noncached(vma->vm_page_prot);
        vm_flags_set(vma, VM_MIXEDMAP);
	vma->vm_ops = &nopage_vm_ops;
#ifdef CONFIG_MICROBLAZE
	length = vma->vm_end - vma->vm_start;
	if (remap_pfn_range(vma,
			       vma->vm_start,
			       vma->vm_pgoff,
			       length,
			       vma->vm_page_prot)) {
		printk("%s Failed to map device memory\n", __func__);
		return -EAGAIN;
	}
#endif
	return 0;
}

static long xib_ioctl(struct file *fp,unsigned int cmd, unsigned long arg)
{
	int err = 0;
	struct xib_umem_ioctl_info req;
	unsigned int mem_type;

	if (_IOC_TYPE(cmd) != XMEM_MGR_MAGIC_KEY)
		return -ENOTTY;

	if (_IOC_NR(cmd) > XMEM_MAX_KMM_FNS)
		return -ENOTTY;

	if (_IOC_DIR(cmd) & _IOC_READ)
		err = !access_ok((void __user *)arg, _IOC_SIZE(cmd));
	else if(_IOC_DIR(cmd) & _IOC_WRITE)
		err = !access_ok((void __user *)arg, _IOC_SIZE(cmd));

	if (err)
		return -EFAULT;
	switch(cmd) {
		case XMM_ALLOC_OPC:
			copy_from_user(&req, (const void __user *)arg, sizeof(req));
			err = alloc_mem(&req);
			if (err) {
				printk("Error while allocating chunk %s:%d\n",
							__func__, __LINE__);
				break;
			}
			err = copy_to_user((void __user *)arg, &req, sizeof(req));
			break;
		case XMM_FREE_OPC:
			copy_from_user(&req, (const void __user *)arg, sizeof(req));
			err = free_mem(&req);
			err = copy_to_user((void __user *)arg, &req, sizeof(req));
			break;
		case XMM_SUPPORT_OPC:
			err = copy_to_user((void __user *)arg, &kmm_ctx.platform_mem_info,
					sizeof(kmm_ctx.platform_mem_info));
			break;
		default:
			printk("Invalid ioctl command\n");
			return -EFAULT;
	}

	return err;
}


int alloc_mem(struct xib_umem_ioctl_info *req)
{
	int ret;
	u64 prev_addr = 0, prev_size = 0, found = 0;
	struct xib_kmem_info *curr, *tmp, *node = NULL;
	struct xib_kmem_chunk *curr_chunk, *tmp_chunk, *chunk = NULL, *prev_chunk = NULL;

	spin_lock(&mm_list_lock);
	list_for_each_entry_safe(curr, tmp, &mmr_head, mmr_list) {
		if (curr->type == req->type) {
			node = curr;
			break;
		}
	}

	if (node == NULL) {
		printk("%s:%d the type of memory requested for doesn't exist\n",
			__func__, __LINE__);
		spin_unlock(&mm_list_lock);
		return -ENOMEM;
	}

	if (req->size > node->available_size) {
		printk("Mem type : %d. Requested size %#lx is more than"
			" available size %#lx\n", req->type, req->size,
			node->available_size);
		spin_unlock(&mm_list_lock);
		return -ENOMEM;
	}

	chunk = kmalloc (sizeof(struct xib_kmem_chunk), GFP_KERNEL);
	if (!chunk) {
		printk("Failed to alloc memory\n");
		chunk = NULL;
		spin_unlock(&mm_list_lock);
		return -ENOMEM;
	}
	chunk->size = req->size;

	if (list_empty(&node->chunk_list)) {
		list_add_tail(&chunk->chunk_list, &node->chunk_list);
		chunk->start_addr = node->base_addr;
		req->chunk_ofs = chunk->start_addr;
	} else {
		list_for_each_entry_safe(curr_chunk, tmp_chunk, &node->chunk_list, chunk_list) {
			if (prev_chunk) {
				if ((curr_chunk->start_addr - (prev_chunk->start_addr + prev_chunk->size))
									>= req->size) {
					chunk->start_addr = prev_chunk->start_addr + prev_chunk->size;
					list_add(&chunk->chunk_list, &prev_chunk->chunk_list);
					found = 1;
					break;
				}
			} else {
				if ((curr_chunk->start_addr - node->base_addr) >= req->size) {
					chunk->start_addr = curr_chunk->start_addr;
					/* create a node and add it after head */
					list_add(&chunk->chunk_list, &node->chunk_list);
					found = 1;
					break;
				}
			}
			prev_chunk = curr_chunk;
		}

		if (!found) {
			if (((node->base_addr + node->size) - (prev_chunk->start_addr + prev_chunk->size))
						>= req->size) {
				chunk->start_addr = prev_chunk->start_addr + prev_chunk->size;
				list_add(&chunk->chunk_list, &prev_chunk->chunk_list);
			} else {
				kfree(chunk);
				printk("A chunk cannot be allotted for the requested memory\n");
				spin_unlock(&mm_list_lock);
				return -ENOMEM;
			}
		}

	}

	req->chunk_ofs = chunk->start_addr;
	req->chunk_id = node->max_chunk++;
	req->chunk_id = ((req->chunk_id) & 0x00FFFFFF) | ((req->type) << 24);
	chunk->chunk_id = req->chunk_id;
	node->available_size = node->available_size - req->size;
	spin_unlock(&mm_list_lock);
	return 0;
}

int free_mem(struct xib_umem_ioctl_info *req)
{
	int ret, found = 0;
	struct xib_kmem_info *curr, *tmp, *node = NULL;
	struct xib_kmem_chunk *chunk_curr, *chunk_tmp, *chunk = NULL;

	spin_lock(&mm_list_lock);
	list_for_each_entry_safe(curr, tmp, &mmr_head, mmr_list) {
		if (curr->type == req->type) {
			node = curr;
			break;
		}
	}

	if (node == NULL) {
		pr_debug("the type of memory doesn't exist to delete\n");
		spin_unlock(&mm_list_lock);
		return -ENOMEM;
	}

	list_for_each_entry_safe(chunk_curr, chunk_tmp, &node->chunk_list, chunk_list) {
		if (chunk_curr->chunk_id == req->chunk_id) {
			req->chunk_ofs = chunk_curr->start_addr;
			node->available_size = node->available_size + chunk_curr->size;
			__list_del_entry(&chunk_curr->chunk_list);
			kfree(chunk_curr);
			found = 1;
			break;
		}
	}

	if (found == 0) {
		pr_debug("The chunk requested to be deleted is not present\n");
		spin_unlock(&mm_list_lock);
		return -EFAULT;
	}

	spin_unlock(&mm_list_lock);
	return 0;
}

int xib_kmm_free_mem(unsigned int memtype, uint32_t id)
{
	struct xib_umem_ioctl_info req;
	int ret;

	req.type = memtype;
	req.chunk_id = id;

	ret = free_mem (&req);
	if (ret) {
		printk("Failed to free memory %s:%d\n", __func__, __LINE__);
		return -EINVAL;
	}

	return 0;
}
EXPORT_SYMBOL(xib_kmm_free_mem);

bool is_xib_memory_available(unsigned int type)
{
	if (type >= XMEM_MAX_MEM_TYPES)
		return false;

	return ((kmm_ctx.platform_mem_info.supported_mem_types &\
			(1U << type)) != 0);
}
EXPORT_SYMBOL(is_xib_memory_available);

bool get_xib_mem_accessible_status(unsigned int type)
{
	bool stat = false;

	stat = is_xib_memory_available(type);
	if (!stat)
		return stat;

	return ((kmm_ctx.platform_mem_info.processor_access &\
			(1U << type)) != 0);
}
EXPORT_SYMBOL(get_xib_mem_accessible_status);

uint64_t xib_kmm_alloc_mem(unsigned int memtype, uint64_t size, uint32_t *id)
{
	struct xib_umem_ioctl_info req;
	int ret;
	void *va;

	if (!id) {
		printk("ID pointer must be valid %s:%d\n", __func__, __LINE__);
		return -EINVAL;
	}
	if (size & (PAGE_SIZE - 1))
		size = (size & (~(PAGE_SIZE - 1))) + PAGE_SIZE;

	req.type = memtype;
	req.size = size;

	ret = alloc_mem(&req);
	if (ret) {
		printk("Failed to alloc memory %s:%d\n", __func__, __LINE__);
		return 0;
	}

	*id = req.chunk_id;
	return req.chunk_ofs;
}
EXPORT_SYMBOL(xib_kmm_alloc_mem);

int xib_close(struct inode *inode, struct file *fp)
{
	/* free memory from all the chunks */
	struct xib_kmem_info *curr, *tmp, *node = NULL;
	struct xib_kmem_chunk *chunk_curr, *chunk_tmp, *chunk = NULL, *prev_chunk = NULL;

	file_open_cnt--;

	if (file_open_cnt)
		return 0;

	spin_lock(&mm_list_lock);
	list_for_each_entry_safe(curr, tmp, &mmr_head, mmr_list) {
		list_for_each_entry_safe(chunk_curr, chunk_tmp, &curr->chunk_list, chunk_list) {
			curr->available_size += chunk_curr->size;
			__list_del_entry(&chunk_curr->chunk_list);
			kfree(chunk_curr);
		}
	}

	spin_unlock(&mm_list_lock);
	return 0;
}

static int xib_open(struct inode *inode, struct file *fp)
{
	fp->private_data = (void *)cdevice;
	file_open_cnt++;
	return 0;
}

static ssize_t xib_read (struct file *fp, char *buf, size_t len, loff_t *ofs)
{
	printk("Not implemented\n");
	return 0;
}


static ssize_t xib_write(struct file *fp, const char *buf, size_t len, loff_t *ofs)
{
	printk("Not implemented\n");
	return 0;
}

int add_node(xib_mem_type type, u64 base_addr, u64 size, struct resource *res,
			struct device *dev, bool proc_access)
{
	struct xib_kmem_info *temp = NULL;
	unsigned int page_cnt, i;
	void *va;
	struct percpu_ref *ref;

	if (!res || (!dev)) {
		printk("Failed: No resource info\n");
		return -EINVAL;
	}

	if (mmr == NULL) {
		mmr = kmalloc (sizeof(struct xib_kmem_info), GFP_KERNEL);
		if (!mmr) {
			printk("Failed to alloc memory\n");
			mmr = NULL;
			return -ENOMEM;
		}

		list_add_tail(&mmr->mmr_list, &mmr_head);
		mmr->type = type;
		mmr->base_addr = base_addr;
		mmr->size = size;
		mmr->available_size = size;
		mmr->max_chunk = 1;
		mmr->dev = dev;
		temp = mmr;
	} else {
		temp = kmalloc (sizeof(struct xib_kmem_info), GFP_KERNEL);
		if (!temp) {
			printk("Failed to alloc memory\n");
			temp = NULL;
			return -ENOMEM;
		}

		list_add_tail(&temp->mmr_list, &mmr_head);
		temp->type = type;
		temp->base_addr = base_addr;
		temp->size = size;
		temp->available_size = size;
		temp->max_chunk = 1;
		temp->dev = dev;
	}

	spin_lock_init(&temp->chunk_lock);
	INIT_LIST_HEAD(&temp->chunk_list);

#ifdef CONFIG_MICROBLAZE
	if (XMEM_PL_DDR == type) {
		int *buf;

		temp->base_va = devm_memremap(dev, base_addr, size, MEMREMAP_WB);
		if (!temp->base_va) {
			printk("%s:%d failed to remap pages\n", __func__, __LINE__);
			return -EFAULT;
		}

		buf = temp->base_va;
		for(i = 0; i < (size / 64); i++)
			buf[i * 16] = 0;
	}
#else
	if (temp) {
		int ret, min_align = 0;
		uint32_t nr_pages;

		/* create resource information */
		memset(&temp->pg_map, 0, sizeof(temp->pg_map));
		//temp->pg_map.ref = NULL; this is not a pointer now

		temp->pg_map.nr_range = 1;
		temp->pg_map.range.start = res->start;
		temp->pg_map.range.end = res->start + resource_size(res);

		/* @ToDo: zone device implementaion limits minimum size to be
			mapped. The following workaround sets minimum expected
			memory size if a particular memory type doesn't meet the
			expected criteria */
		if (IS_ENABLED(CONFIG_SPARSEMEM_VMEMMAP))
			min_align = PAGES_PER_SUBSECTION;
		else
			min_align = PAGES_PER_SECTION;
		if ((resource_size(res) >> PAGE_SHIFT) < min_align) {
			printk("Minimum size for zone device is not met. "
				"Setting the size to minimum acceptable"
				" value\n");
			temp->pg_map.range.end = res->start + \
						min_align * PAGE_SIZE;
		}
		temp->pg_map.type = MEMORY_DEVICE_PCI_P2PDMA;
		init_completion(&temp->comp);

                ref = &temp->pg_map.ref;
                temp->percpu_ref = ref;
		temp->pg_map.ops = &xpagemap_ops;

		temp->base_va = devm_memremap_pages(dev, &temp->pg_map);
		if (!temp->base_va) {
			printk("%s:%d failed to remap pages\n", __func__, __LINE__);
			return -EFAULT;
		}

		/* prepare pages entries */
		nr_pages = resource_size(res) >> PAGE_SHIFT;
		temp->pages = vmalloc(nr_pages * sizeof(struct page *));
		if (!temp->pages) {
			printk("%s:%d failed\n", __func__, __LINE__);
			return -EFAULT;
		}

		for (i = 0 ; i < nr_pages; i++) {
                    temp->pages[i] = virt_to_page(temp->base_va + (i * PAGE_SIZE));
                    if (IS_ERR(temp->pages[i])) {
                        printk("%s:%d failed\n", __func__, __LINE__);
                        printk("code[%d] %d\n", i, temp->pages[i] );
                        return -EFAULT;
                    }
		}
	}
#endif
	kmm_ctx.platform_mem_info.supported_mem_types |= (1U << type);
	if (proc_access)
		kmm_ctx.platform_mem_info.processor_access |= (1U << type);
	return 0;
}

int xib_check_processor_access(struct device_node *np)
{
	int ret, val;

	if (!np)
		return 0;

	ret = of_property_read_u32(np, "xlnx,proc-access", &val);
	if (!val)
		return XMEM_NO_PROC_ACCESS;
	/* if no DT parameter is specified, its to be assumed that processor has
		   access */
	if ((ret < 0) || val)
		return XMEM_PROC_HAS_ACCESS;
	return 0;
}

// Only BRAM_1 (0x9100_0000) is ERNIC/PS/Engine reachable
#define XIB_BRAM1_BASE 0x91000000ULL

int xib_bram_init(struct platform_device *pdev)
{
    struct device_node *np = NULL;
    struct resource res;
    int ret;

    // Several axi_bram_ctrl nodes may exist; pick BRAM_1 by its base address.
    for (np = of_find_node_by_name(NULL, "axi_bram_ctrl"); np;
         np = of_find_node_by_name(np, "axi_bram_ctrl")) {
        if (of_address_to_resource(np, 0, &res) < 0) {
            continue;
		}
        if (res.start == XIB_BRAM1_BASE) {
            break;
		}
    }
    if (!np) {
        pr_warn("cant find BRAM_1 (%#llx) node\n", (unsigned long long)XIB_BRAM1_BASE);
        return -ENOMEM;
    }

    res.name = NULL;
    res.flags = IORESOURCE_MEM;

    pr_info("BRAM_1 addr %#lx size %#lx\n", res.start, resource_size(&res));
    ret = add_node(XMEM_BRAM, res.start, resource_size(&res), &res, &pdev->dev,
                   xib_check_processor_access(np));
    return ret;
}

int xib_ext_ddr_init(struct platform_device *pdev)
{
	struct device_node *np = NULL;
	struct resource res;
	u64 eddr_start, eddr_size;
	u32 reg_buf[4];
	int ret;

	np = of_find_node_by_name(NULL, "ext_ddr");
	if (!np) {
		pr_warn("can't find extra DDR node\n");
		return -ENOMEM;
	}
	ret = of_property_read_variable_u32_array(np,
			"reg", &(reg_buf[0]), 4, 4);

	if (ret < 0) {
		pr_err( "can't get extra DDR resource\n");
		return -ENOMEM;
	}
	eddr_start = (u64)reg_buf[0] << 32 | reg_buf[1];
	eddr_size = (u64)reg_buf[2] << 32 | reg_buf[3];

	res.name  = NULL;
	res.flags = IORESOURCE_MEM;
	res.start = eddr_start;
	res.end = eddr_start + eddr_size;
	pr_info("%s extra ddr addr %llx size %llx\n", __func__, eddr_start, eddr_size);
	ret = add_node(XMEM_EXT_DDR, eddr_start, eddr_size, &res, &pdev->dev,
				xib_check_processor_access(np));
	return ret;
}

int xib_pl_ddr_init(struct platform_device *pldev)
{
	int ret;
	struct device *xlnx_pl_alloc_get_device(void);
	struct device *pl_dev = &pldev->dev;
	struct device_node *np = NULL;
	struct resource res;

	/* Initialize reserved memory resources */
	np = of_parse_phandle(pl_dev->of_node, "pl-mem-region", 0);
	if(!np) {
		dev_err(pl_dev, "Could not get reserved memory\n");
		return -EFAULT;
	}

	ret = of_address_to_resource(np, 0, &res);
	if (ret) {
		dev_err(dev, "No memory address assigned to the region\n");
		of_node_put(np);
		return -EFAULT;
	}

	printk("pl ddr base addr is %#lx, size is %#lx\n", res.start,
						resource_size(&res));
	res.name  = NULL;
	res.flags = IORESOURCE_MEM;
	ret = add_node(XMEM_PL_DDR, res.start, resource_size(&res), &res,
			pl_dev, xib_check_processor_access(np));
	return ret;
}

int xib_ps_ddr_init(struct platform_device *pdev)
{
	struct device_node *np = NULL;
	struct resource res;
	u64 psddr_start, psddr_size;
	dma_addr_t paddr;
	int ret, rc;

	/* Initialize reserved memory resources */
	np = of_parse_phandle(pdev->dev.of_node, "memory-region", 0);
	if(!np) {
		dev_err(&pdev->dev, "Could not get reserved memory\n");
		return -EFAULT;
	}

	rc = of_address_to_resource(np, 0, &res);
	if (rc) {
		dev_err(dev, "No memory address assigned to the region\n");
		of_node_put(np);
		return -EFAULT;
	}

	res.name  = NULL;
	res.flags = IORESOURCE_MEM;

	printk("PS DDR start address is %#lx, size is %#lx\n",
			res.start, resource_size(&res));

	ret = add_node(XMEM_PS_DDR, res.start, resource_size(&res), &res,
			&pdev->dev, xib_check_processor_access(np));
	return ret;
}

int xib_pci_ddr_init(void)
{
	struct device_node *np = NULL;
	struct resource res;
	u64 pciddr_start, pciddr_size;
	u32 reg_buf[4];
	int ret;

	np = of_find_node_by_name(NULL, "pci_ddr");
	if (!np) {
		pr_warn("can't find PCI DDR node\n");
		return -ENOMEM;
	}
	ret = of_property_read_variable_u32_array(np,
			"reg", &(reg_buf[0]), 4, 4);

	if (ret < 0) {
		pr_err( "can't get PCI DDR resource\n");
		return -ENOMEM;
	}
	pciddr_start = (u64)reg_buf[0] << 32 | reg_buf[1];
	pciddr_size = (u64)reg_buf[2] << 32 | reg_buf[3];
	pr_info("%s PCI DDR start address %llx size %llx\n", __func__,
			pciddr_start, pciddr_size);

	ret = add_node(XMEM_PCI_DDR,pciddr_start,pciddr_size, &res, NULL,
			xib_check_processor_access(np));

	return ret;
}

static int xib_kmm_probe(struct platform_device *pdev)
{
	int err;
	struct device_node *rnic_node, *np;

	rnic_node = of_find_node_by_name(NULL, "ernic");
	if (!rnic_node) {
		dev_err(NULL, "Couldn't find rnic node\n");
		return -EFAULT;
	}
	printk("%s : <---------- \n", __func__);
	/* char device */
	err = alloc_chrdev_region(&dev_num, DEV_MINOR_BASE, DEVS_CNT, DEV_NAME);
	if (err) {
		dev_err(NULL, "Failed to allocate device numbers\n");
		goto err;
	}

	cdevice = cdev_alloc();
	if (!cdevice) {
		dev_err(NULL, "Failed to allocate cdev\n");
		unregister_chrdev_region(dev_num, DEVS_CNT);
		goto err;
	}

	cdev_init(cdevice, &fops);
	cdevice->owner = THIS_MODULE;
	err = cdev_add(cdevice, dev_num, DEVS_CNT);
	if (err) {
		dev_err(NULL, "Failed to add cdev to VFS\n");
		unregister_chrdev_region(dev_num, DEVS_CNT);
		goto err;
	}

	cl = class_create("xib_kmm");
	if (!cl) {
		dev_err(NULL, "Failed to create a dev class\n");
		cdev_del(cdevice);
		unregister_chrdev_region(dev_num, DEVS_CNT);
		goto err;
	}

	dev = device_create(cl, NULL, dev_num, NULL, "%s%d", DEV_NAME, MINOR(dev_num));
	if (!dev) {
		dev_err(NULL, "Failed to create device\n");
		class_destroy(cl);
		cdev_del(cdevice);
		unregister_chrdev_region(dev_num, DEVS_CNT);
	}

	xib_pl_ddr_init(pdev);
	xib_ext_ddr_init(pdev);
	xib_bram_init(pdev);
#ifndef CONFIG_MICROBLAZE
	xib_ps_ddr_init(pdev);
	xib_pci_ddr_init();
#endif
	spin_lock_init(&mm_list_lock);
	return 0;
err:
	return -EFAULT;
}

static int xib_kmm_remove (struct platform_device *pdev)
{
	struct xib_kmem_info *curr, *tmp, *node = NULL;
	struct xib_kmem_chunk *chunk_curr, *chunk_tmp, *chunk = NULL, *prev_chunk = NULL;

	spin_lock(&mm_list_lock);
	list_for_each_entry_safe(curr, tmp, &mmr_head, mmr_list) {
		list_for_each_entry_safe(chunk_curr, chunk_tmp, &curr->chunk_list, chunk_list) {
			__list_del_entry(&chunk_curr->chunk_list);
			kfree(chunk_curr);
		}
#ifndef CONFIG_MICROBLAZE
		devm_memunmap_pages(&pdev->dev, &curr->pg_map);
		vfree(curr->pages);
#else
		if (XMEM_PL_DDR == curr->type)
			devm_memunmap(curr->dev, (void*)curr->base_va);
#endif
		kfree(curr);
	}
	spin_unlock(&mm_list_lock);

	if (dev)
		device_destroy(cl, dev_num);
	if (cl)
		class_destroy(cl);
	if (cdevice)
		cdev_del(cdevice);
	unregister_chrdev_region(dev_num, DEVS_CNT);

	return 0;
}

static const struct xlnx_kmm_config xlnx_kmm_v1_config = {
	.dummy = 1,
};

static const struct of_device_id kmm_of_match[] = {
	{ .compatible = "xlnx,xib-kmm-1.0", .data = &xlnx_kmm_v1_config},
	{},
};
MODULE_DEVICE_TABLE(of, kmm_of_match);

static struct platform_driver xib_kmm_driver = {
	.probe = xib_kmm_probe,
	.remove = xib_kmm_remove,
	.driver = {
		.name = "xib_kmm",
		.of_match_table = kmm_of_match,
	},
};

module_platform_driver(xib_kmm_driver);
MODULE_AUTHOR("Ravali P <ravalip@xilinx.com>");
MODULE_DESCRIPTION("Xilinx KMM Module");
MODULE_LICENSE("GPL");
