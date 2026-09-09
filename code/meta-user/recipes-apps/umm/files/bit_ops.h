// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * AMD ERNIC bit operations for UMM
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

#ifndef _XMEM_BIT_OPS_H
#define _XMEM_BIT_OPS_H

#include <stdint.h>

#define BITS_PER_BYTE		(8)
#define BITS_IN_INT		(sizeof(int) * (BITS_PER_BYTE))
#define BIT_MASK(n)		(((1UL << (n - 1)) - 1) | (1U << (n - 1)))

#define ROUND_UP(n, d)		(((n) + (d) -1) / (d))
#define BITS_TO_INTS(n)		ROUND_UP(n, BITS_IN_INT)
#define BIT_POS_TO_INT(n)	((n) / (BITS_IN_INT))

void set_bit_map(unsigned int *bmap, unsigned int pos, unsigned int bit_cnt);
void clear_bit_map(unsigned int *bmap, unsigned int pos, unsigned int bit_cnt);
uint32_t find_zero_bits(unsigned int *bmap, unsigned int cnt, unsigned int max_bits);
#endif
