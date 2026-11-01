/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _SHDWSTCK_LAYOUT_H
#define _SHDWSTCK_LAYOUT_H
#include <linux/types.h>

extern unsigned long shdwstck_size;
extern unsigned long shdwstck_area_size;
extern unsigned long shdwstck_user_start;
extern unsigned long shdwstck_user_end;
extern unsigned long shdwstck_start;
extern unsigned long shdwstck_end;
extern unsigned long shdwstck_delta;

#define shdwstck_addr(stack_base) ((stack_base) + shdwstck_delta)
#define shdwstck_idx(b) (((b) - shdwstck_user_start) / shdwstck_size)

bool valid_stack_base(unsigned long base);

bool sa_stack_range_ok(unsigned long start, unsigned long end);

long sa_layout_init(void);

#endif