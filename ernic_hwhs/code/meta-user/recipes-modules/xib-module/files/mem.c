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
#include <linux/module.h>
#include <linux/of_platform.h>
#include <linux/of_address.h>
#include <linux/list.h>
#include "xib.h"

#define BRAM_ALLOC_UNIT (4096)

/* Incase of HW HS, each QP requires SQ depth of 16 i.e, 1KB in size
	And for 256QPs, this would be sufficient*/
#define BRAM_MAX_ALLOC_UNIT (1024 * 256)

#define DEBUG_PRINTS_LIST 0
#define BRAM_CTRL_CNT	(2)

struct device *pl_alloc_dev = NULL;
/* bram/uram allocation map */
struct xib_bmap bram_map;
u64	bram_free_size;

u32 bram_ctrl_cnt = 0;

struct mem_mgr_info {
	struct list_head mem_free;
	struct list_head mem_alloc;
	u64	free_mem;
	u64	used_mem;
	u64	tot_mem;
	u64	mem_base_p;
	void 	*mem_base_va;
};

struct mem_mgr_info *bram_mem_mgr[BRAM_CTRL_CNT];
struct mem_mgr_info *eddr_mem_mgr;

struct mem_container {
	u64	phy_addr;
	u64	blk_size;
	struct list_head  list;
	bool	is_free;
};


int xib_register_pl_allocator(struct device *dev)
{
	pl_alloc_dev = dev;

	pr_debug("%s: pl_alloc_dev : %px\n", __func__, pl_alloc_dev);

	return 0;
}

void print_list(struct list_head *head)
{
	struct mem_container *iter, *tmp;

	list_for_each_entry_safe(iter, tmp, head, list) {
		printk("addr :[%#x] size [%#x]\n", iter->phy_addr, iter->blk_size);
	}
}

u64 free_alloc_table(u64 addr, struct list_head *head)
{
	struct mem_container *iter, *tmp;

	list_for_each_entry_safe(iter, tmp, head, list) {
		if (iter->phy_addr == addr) {
			__list_del_entry(&iter->list);
			kfree(iter);
			return iter->blk_size;
		}
	}

#if DEBUG_PRINTS_LIST
	print_list(head);
#endif
	return 0;
}

bool is_mem_overlapped(struct list_head *head, u64 addr,
			struct mem_container **pos, u64 size)
{
	struct mem_container *iter, *tmp;
	u64 temp_addr;
	bool addr_merged = false;

	*pos = NULL;

	list_for_each_entry_safe(iter, tmp, head, list) {
		if (((iter->phy_addr <= addr) && (iter->phy_addr + iter->blk_size) > addr))
			return true;

		if (addr_merged) {
			if ((*pos)->phy_addr + (*pos)->blk_size == iter->phy_addr) {
				/* Merge & delete next block */
				(*pos)->blk_size += iter->blk_size;
				__list_del_entry(&iter->list);
				kfree(iter);
			}
			*pos = NULL;
			return false;
		}

		/* TODO:*/
		if (addr < iter->phy_addr) {
			if ((addr + size) == iter->phy_addr) {
				iter->phy_addr = addr;
				iter->blk_size += size;
				addr_merged = true;
			} else {
				*pos = iter;
				return false;
			}
		} else {
			if (addr == (iter->phy_addr + size)) {
				iter->blk_size += size;
				addr_merged = true;
			}
		}
		*pos = iter;
	}

	if (addr_merged)
		*pos = NULL;

	return false;
}

int free_mem_alloc(struct mem_mgr_info *mgr, u64 addr, u64 size)
{
	u64 var;

	struct mem_container *tmp = NULL, *pos, *dup;

	if (mgr->tot_mem < (mgr->free_mem + size))
		return -EFAULT;

	tmp = kzalloc(sizeof(*tmp), GFP_KERNEL);
	if (!tmp) {
		pr_err("Error while allocating mem container\n");
		return -ENOMEM;
	}

	INIT_LIST_HEAD(&tmp->list);
	tmp->phy_addr = addr;
	tmp->blk_size = size;
	tmp->is_free = true;

#if DEBUG_PRINTS_LIST
	printk("Printing free mem list\n");
	print_list(&mgr->mem_free);
#endif
	if (list_empty(&mgr->mem_free)) {
		list_add_tail(&tmp->list, &mgr->mem_free);
		goto done;
	}

	if (is_mem_overlapped(&mgr->mem_free, addr, &pos, size))
		return -EFAULT;

	if (pos) {
		/* close entry found */
		/* check whether we can merge or not */
		if (addr < pos->phy_addr)
			/* create an entry just before pos */
			list_add_tail(&tmp->list, &pos->list);
		else
			list_add(&tmp->list, &pos->list);
	} else
		kfree(tmp);
done:
#if DEBUG_PRINTS_LIST
	printk("Printing free mem list\n");
	print_list(&mgr->mem_free);
#endif
	mgr->free_mem += size;
	var = free_alloc_table(addr, &mgr->mem_alloc);
	if (var && (var != size))
		pr_debug("Allocated %#lx but removing %#lx size\n",
			var, size);

	return 0;
}

bool mgr_has_memory(struct mem_mgr_info *mgr, u64 size)
{
	return (mgr->free_mem >= size);
}

u32 get_mem_ofs(struct mem_mgr_info *mgr, u64 addr)
{
	return (mgr->mem_base_p - addr);
}

int alloc_mem(struct mem_mgr_info *mgr, u32 size, u64 *phy)
{
	struct mem_container *tmp = NULL, *next, *pos;
	u64 addr = 0;
	bool lblk_found = false;
	int ret = 0;

	size = round_up(size, 256);

	if (!mgr_has_memory(mgr, size)) {
		pr_debug("Manager doesnt have enuf memory\n");
		return -ENOMEM;
	}

	/* find the free block */
	list_for_each_entry_safe(next, tmp, &mgr->mem_free, list) {
		if (next->blk_size >= size) {
			lblk_found = true;
			break;
		}
	}

	if (!lblk_found)
		return -ENOMEM;


	tmp = kzalloc(sizeof(*tmp), GFP_KERNEL);
	if (!tmp)
		return -ENOMEM;

	if ((next->blk_size - size) >= BRAM_ALLOC_UNIT) {
		addr = next->phy_addr;
		next->phy_addr += size;
		next->blk_size -= size;
	} else {
		addr = next->phy_addr;
		size = next->blk_size;
		/* delete entry */
		__list_del_entry(&next->list);
		kfree(next);
	}

	*phy = addr;
	tmp->blk_size = size;
	tmp->phy_addr = addr;
	mgr->free_mem -= size;
	/* add it to the alloc table entries */
	list_add_tail(&tmp->list, &mgr->mem_alloc);
#if DEBUG_PRINTS_LIST
	printk("Printing free mem list\n");
	print_list(&mgr->mem_free);
	printk("Printing allocated mem list\n");
	print_list(&mgr->mem_alloc);
#endif
	return 0;
}

int xib_bram_init(void)
{
	struct device_node *np = NULL;
	struct resource res;
	size_t size;
	int ret;
	dma_addr_t bram_start;
	void	*bram_start_va;

	np = of_find_node_by_name(NULL, "axi_bram_ctrl");
	if (!np) {
		pr_warn("cant find bram node\n");
		bram_mem_mgr[bram_ctrl_cnt] = NULL;
		return -ENOMEM;
	}

	ret = of_address_to_resource(np, 0, &res);
	if (ret < 0) {
		pr_err( "cant get bram resource\n");
		bram_mem_mgr[bram_ctrl_cnt] = NULL;
		return ret;
	}


	size = resource_size(&res);
	bram_start = res.start;
	bram_start_va = ioremap(bram_start, size);

	bram_mem_mgr[bram_ctrl_cnt] = kmalloc (sizeof (struct mem_mgr_info), GFP_KERNEL);
	if (!bram_mem_mgr[bram_ctrl_cnt]) {
		printk("Failed to alloc memory\n");
		bram_mem_mgr[bram_ctrl_cnt] = NULL;
		return -ENOMEM;
	}

	INIT_LIST_HEAD(&bram_mem_mgr[bram_ctrl_cnt]->mem_free);
	INIT_LIST_HEAD(&bram_mem_mgr[bram_ctrl_cnt]->mem_alloc);

	bram_mem_mgr[bram_ctrl_cnt]->free_mem = 0;
	bram_mem_mgr[bram_ctrl_cnt]->tot_mem = size;
	bram_mem_mgr[bram_ctrl_cnt]->mem_base_p = bram_start;
	bram_mem_mgr[bram_ctrl_cnt]->mem_base_va = bram_start_va;
	bram_mem_mgr[bram_ctrl_cnt]->used_mem = 0;

	ret = free_mem_alloc(bram_mem_mgr[bram_ctrl_cnt++], bram_start, size);

	return ret;
}

bool is_mgr_addr_block(struct mem_mgr_info *mem_mgr, u64 addr)
{
	if ((addr >= mem_mgr->mem_base_p) &&
		(addr <= (mem_mgr->mem_base_p + mem_mgr->tot_mem)))
		return true;

	return false;
}

int is_bram_addr(dma_addr_t addr)
{
	u32 i;

	for (i = 0; i < bram_ctrl_cnt; i++) {
		if (bram_mem_mgr[i] && is_mgr_addr_block(bram_mem_mgr[i], addr))
			return i;
	}
	return -EINVAL;
}

bool xib_pl_present(void)
{
#ifdef ARCH_HAS_PS
	if (pl_alloc_dev)
		return true;
	else
		return false;
#else
	return true;
#endif
}

int xib_ext_ddr_init(void)
{
	struct device_node *np = NULL;
	struct resource res;
	u64 eddr_start, eddr_size;
	u32 reg_buf[4];
	int ret;

	np = of_find_node_by_name(NULL, "ext_ddr");
	if (!np) {
		pr_warn("can't find extra DDR node\n");
		eddr_mem_mgr = NULL;
		return -ENOMEM;
	}
	ret = of_property_read_variable_u32_array(np,
			"reg", &(reg_buf[0]), 4, 4);

	if (ret < 0) {
		pr_err( "can't get extra DDR resource\n");
		eddr_mem_mgr = NULL;
		return -ENOMEM;
	}

	eddr_mem_mgr = kmalloc(sizeof (struct mem_mgr_info), GFP_KERNEL);
	if (!eddr_mem_mgr) {
		pr_err("Failed to alloc memory\n");
		eddr_mem_mgr = NULL;
		return -ENOMEM;
	}

	INIT_LIST_HEAD(&eddr_mem_mgr->mem_free);
	INIT_LIST_HEAD(&eddr_mem_mgr->mem_alloc);

	eddr_start = (u64)reg_buf[0] << 32 | reg_buf[1];
	eddr_size = (u64)reg_buf[2] << 32 | reg_buf[3];
	pr_info("%s extra ddr addr %llx size %llx\n", __func__, eddr_start, eddr_size);

	eddr_mem_mgr->free_mem = 0;
	eddr_mem_mgr->tot_mem = eddr_size;
	eddr_mem_mgr->mem_base_p = eddr_start;
	eddr_mem_mgr->used_mem = 0;

	return free_mem_alloc(eddr_mem_mgr, eddr_start, eddr_size);
}

static struct device *xib_get_alloc_dev(char *from, struct xilinx_ib_dev *xib)
{
	struct device *dev;
#ifdef ARCH_HAS_PS
	if (strcasecmp(from, "pl") == 0) {
		/* if pl is not there fall back to ps */
		if (pl_alloc_dev)
			dev = pl_alloc_dev;
		else
			dev = &xib->pdev->dev;
	} else if (strcasecmp(from, "ps") == 0) {
		dev = &xib->pdev->dev;
	} else
		return NULL;
#else
	dev = &xib->pdev->dev;
#endif

	return dev;
}

void *xib_alloc_coherent(char *from, void *xib_dev,
		size_t len, u64 *dma_handle, gfp_t flags)
{
	struct device *dev;
	u32 idx, i, ofs;
	int ret;
	long bram_num;
	char *temp;
	struct xilinx_ib_dev *xib = (struct xilinx_ib_dev *)xib_dev;

	temp = strstr(from, "bram");
	if (temp) {
		/* check for bram number */
		if (!strcmp(from, "bram"))
			bram_num = 0;
		else {
			temp += strlen(from) - 1; // remove NULL
			ret = kstrtol(temp, 0, &bram_num);
			if (ret) {
				printk("Failed to convert string to long\n");
				return NULL;
			}
			bram_num -= 1;
		}
		if (alloc_mem(bram_mem_mgr[bram_num], len, dma_handle) < 0) {
                    printk("xrnic_alloc_coherent bram full\n");
			/* if bram is full fall back to pl */
			dev = xib_get_alloc_dev("pl", xib);
			/* if pl is not there fall back to ps */
			if (!dev)
                        {
				dev = xib_get_alloc_dev("ps", xib);
                        }
                        printk("xrnic_alloc_coherent fallback\n");
			goto fallback;
		} else {
			ofs = get_mem_ofs(bram_mem_mgr[bram_num], *dma_handle);
		}
		return (void *)((u8 *)bram_mem_mgr[bram_num]->mem_base_va + ofs);
	}
        printk("xrnic_alloc_coherent !temp\n");

	dev = xib_get_alloc_dev(from, xib);
fallback:
	if (dev)
		return dma_alloc_coherent(dev, len, (dma_addr_t *)dma_handle, flags);
	else
        {
            printk("dma_alloc_coherent fail\n");
		return NULL;
        }
}

/*The function is used only in 32BIT system to access 64BIT ADDR*/
u64 xib_alloc_eddr_mem(char *from, u32 len, u64 *pa)
{
	u64 ofs;

	if (strstr(from, "eddr")) {
		if (eddr_mem_mgr) {
			if (alloc_mem(eddr_mem_mgr, len, pa) < 0) {
				pr_err("Failed to allocate memory from extra DDR\n");
				return 0;
			}
			/* Simply adding 0xFF00_0000_0000 in Physical address to
			 * get dummy Virtual address, 32bit Microblaze cannot access
			 * 64bit address DDR.*/
			return (*pa | 0xFF0000000000);
		} else {
			pr_err("Extra DDR is not exist\n");
			return 0;
		}
	}
	return 0;
}
EXPORT_SYMBOL(xib_alloc_eddr_mem);

void *xib_zalloc_coherent(char *from, struct xilinx_ib_dev *xib,
		size_t len, u64 *dma_handle, gfp_t flag)
{
	return xib_alloc_coherent(from, xib, len, dma_handle,
				       flag | __GFP_ZERO);
}

void xib_free_coherent(void *xib_dev,
		 u64 size, void *cpu_addr, u64 dma_handle)
{
	struct device *dev;
	struct xilinx_ib_dev *xib = (struct xilinx_ib_dev *)xib_dev;
	int ret, bram_num;
	char *temp;

	bram_num = is_bram_addr(dma_handle);
	if (bram_num >= 0) {
		/* check for bram number */
		ret = free_mem_alloc(bram_mem_mgr[bram_num], dma_handle, size);
		return;
	}

	if (eddr_mem_mgr && is_mgr_addr_block(eddr_mem_mgr, dma_handle)) {
		free_mem_alloc(eddr_mem_mgr, dma_handle, size);
		return;
	}

	/* check if addr is in ps */
	if (virt_addr_valid(cpu_addr))
		dev = xib_get_alloc_dev("ps", xib);
	else
		dev = xib_get_alloc_dev("pl", xib);

	if (dev)
		dma_free_coherent(dev, size, cpu_addr, dma_handle);
}
