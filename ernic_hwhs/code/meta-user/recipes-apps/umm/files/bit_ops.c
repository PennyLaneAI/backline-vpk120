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

#include <stdio.h>
#include <stdarg.h>
#include "bit_ops.h"

/* Counts number of zeroes from a given bit number */
unsigned int ctz(unsigned int val, unsigned int from, unsigned int max_bits)
{
	unsigned int cnt = 0, i;

	val >>= from;
	for (i = 0; i < (max_bits - from); i++)
		if (!(val & 0x1U)) {
			cnt++;
			val >>= 1;
		} else break;

	return cnt;
}

unsigned int count_trailing_ones(unsigned int val, unsigned int max_bits)
{
	unsigned int cnt = 0;

	return ctz(~val, 0, max_bits);
}

unsigned int ffz(unsigned int val, unsigned int from, unsigned int max_bits)
{
	if (from >= BITS_IN_INT)
		return BITS_IN_INT;
	if (max_bits > BITS_IN_INT) {
		printf("Invalid max bit count \n");
		return BITS_IN_INT;
	}

	if (!val)
		return 0;

	if (val == 0xFFFFFFFFu)
		return BITS_IN_INT; // All bits are 1's

	return (count_trailing_ones(val >> from, (max_bits - from)) + from);
}

uint32_t find_zero_bits(unsigned int *bmap, unsigned int cnt, unsigned int max_bits)
{
	unsigned int val, bit_cnt = 0, rem_bits = max_bits;
	unsigned int ret, contig_free_blks = 0;

	do {
		val = *bmap++;
		bit_cnt = (rem_bits >= BITS_IN_INT)? BITS_IN_INT: rem_bits;
		ret = 0;

		while (1) {
			ret = ffz(val, ret, bit_cnt);


			/* if no block is free */
			if (ret >= BITS_IN_INT) {
				contig_free_blks = 0;	
				break;
			}

			/* If free blocks are available but not continuously */
			if (contig_free_blks && ret) {
				contig_free_blks = 0;
				ret++;
				continue;
			}

			if ((ret + cnt - contig_free_blks) > BITS_IN_INT) {
				if (!((val >> ret) & BIT_MASK(BITS_IN_INT - ret))) {
					/* store it contig_free_blks to check the if zeroes spread
					across different INTs */
					contig_free_blks += BITS_IN_INT - ret;
					break;
				} else
					contig_free_blks = 0;
			} else {
				if (!((val >> ret) & BIT_MASK(cnt - contig_free_blks)))
					return (max_bits - rem_bits + ret - contig_free_blks);

				contig_free_blks= 0;
			}

			/* ignore already processed bit number */
			ret++;
		}

		rem_bits -= bit_cnt;
	} while(rem_bits);
	return max_bits;
}

void update_bit_map(unsigned int *bmap, unsigned int start,
			unsigned int cnt, unsigned int is_set)
{
	unsigned int temp = cnt, bits, *map;

	if (!bmap) {
		printf("Invalid bit map pointer\n");
		return;
	}

	map = &bmap[BIT_POS_TO_INT(start)];
	start = start % BITS_IN_INT;

	while (temp) {
		if ((temp + start) > BITS_IN_INT)
			bits = BITS_IN_INT - start;
		else
			bits = temp;

		if (is_set)
			*map |= (BIT_MASK(bits) << (start % BITS_IN_INT));
		else
			*map &= ~(BIT_MASK(bits) << (start % BITS_IN_INT));

		start = 0;
		temp -= bits;
		if (temp)
			++map;
	}
}

void set_bit_map(unsigned int *bmap, unsigned int start, unsigned int cnt)
{
	update_bit_map(bmap, start, cnt, 1);
}

void clear_bit_map(unsigned int *bmap, unsigned int start, unsigned int cnt)
{
	update_bit_map(bmap, start, cnt, 0);
}
