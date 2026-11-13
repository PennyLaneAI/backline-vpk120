/*
 * AMD FPGA ERNIC kernel memory manager driver 
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
#ifndef _XIB_KMM_EXPORT_H
#define _XIB_KMM_EXPORT_H

typedef enum xib_mem_type {
	XMEM_TYPE_MIN_VAL = 0,
	XMEM_PL_DDR,
	XMEM_PS_DDR,
	XMEM_EXT_DDR,
	XMEM_PCI_DDR,
	XMEM_BRAM,
	XMEM_MAX_MEM_TYPES,
} xib_mem_type;

enum xib_mem_proc_access_stat {
	XMEM_NO_PROC_ACCESS = 0,
	XMEM_PROC_HAS_ACCESS = 1,
};

#define XIB_PS_STR      "ps"
#define XIB_PL_STR      "pl"
#define XIB_BRAM_STR    "bram"
#define XIB_EDDR_STR    "eddr"

static int xib_mem_str_to_type(char *str)
{
	int mem_type;

        if (!str)
                return -EINVAL;

	if (!strcasecmp(str, XIB_PS_STR))
		mem_type = XMEM_PS_DDR;
	else if (!strcasecmp(str, XIB_PL_STR))
		mem_type = XMEM_PL_DDR;
	else if (!strcasecmp(str, XIB_BRAM_STR))
		mem_type = XMEM_BRAM;
	else if (!strcasecmp(str, XIB_EDDR_STR))
		mem_type = XMEM_EXT_DDR;
	else {
                pr_err("Invalid memory type: %s\n", str);
                mem_type = -EINVAL;
        }

        return mem_type;
}

bool is_xib_memory_available(unsigned int type);
bool get_xib_mem_accessible_status(unsigned int type);
int xib_kmm_free_mem(unsigned int memtype, uint32_t id);
uint64_t xib_kmm_alloc_mem(unsigned int memtype, uint64_t size, uint32_t *id);
#endif
