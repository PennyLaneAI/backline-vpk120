// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * AMD ERNIC hardware handshake driver
 *
 * Copyright (C) 2024 AMD, Inc. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation and may be copied,
 * distributed, and modified under those terms.
 */
#include <linux/of_platform.h>
#include <linux/of_net.h>
#include <linux/module.h>
#include <linux/of_address.h>
#include <linux/of_reserved_mem.h>
#include <linux/mm.h>
#include <linux/dma-mapping.h>
#include <linux/slab.h>

struct pl_alloc_ctrl {
	struct device *dev;
	int ref_count; /* number of users */
};

struct pl_alloc_ctrl *plc = NULL;

static int xlnx_pl_alloc_probe(struct platform_device *pdev)
{
	int rc;
	struct device_node *np = NULL;

	plc = kzalloc(sizeof(struct pl_alloc_ctrl), GFP_KERNEL);
	if (!plc)
		dev_err(&pdev->dev, "couldnt allocate memory\n");

	/* Initialize reserved memory resources */
	rc = of_reserved_mem_device_init(&pdev->dev);
	if(rc) {
                dev_err(&pdev->dev, "Could not get reserved memory %d\n", rc);
		goto error1;
	}

	plc->dev = &pdev->dev;
	dma_set_coherent_mask(&pdev->dev, 0xFFFFFFFF);

	dev_info(&pdev->dev, "%s: plc->dev : %px\n", __func__, plc->dev);
	return 0;
error1:
	kfree(plc);
	plc = NULL;
	return -EFAULT;
}

struct device *xlnx_pl_alloc_get_device(void)
{
	plc->ref_count++;
	return plc->dev;
}

EXPORT_SYMBOL_GPL(xlnx_pl_alloc_get_device);

static int xlnx_pl_alloc_remove (struct platform_device *pdev)
{
	return 0;
}

struct xlnx_pl_alloc_config {
	int dummy;
};

static const struct xlnx_pl_alloc_config xlnx_pl_alloc_v1_config = {
	.dummy = 1,
};

static const struct of_device_id xlnx_of_match[] = {
	{ .compatible = "xlnx,pl-allocator-1.0", .data = &xlnx_pl_alloc_v1_config},
	{},
};
MODULE_DEVICE_TABLE(of, xlnx_of_match);

static struct platform_driver xlnx_pl_alloc_driver = {
	.probe = xlnx_pl_alloc_probe,
	.remove = xlnx_pl_alloc_remove,
	.driver = {
		.name = "pl-allocator",
		.of_match_table = xlnx_of_match,
	},
};

module_platform_driver(xlnx_pl_alloc_driver);

MODULE_AUTHOR("syed s <syeds@xilinx.com>");
MODULE_DESCRIPTION("Xilinx PL Allocator driver");
MODULE_LICENSE("GPL");
