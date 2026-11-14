/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _SHDWSTCK_ALLOC_H
#define _SHDWSTCK_ALLOC_H
#include <linux/list.h>
#include <linux/mm_types.h>
#include <linux/types.h>
#include <linux/xarray.h>

struct sa_allocator_desc {
	struct mm_struct *mm;
	u64 main_stack;
	/* the shadow of the child could not be copied: its stack faults SIGSEGV */
	bool broken;
};

struct sa_allocator_desc *alloc_find(struct mm_struct *mm);

struct sa_allocator_desc *alloc_new(struct mm_struct *mm);

void alloc_destroy(struct sa_allocator_desc *alloc);

void alloc_teardown_locked(struct sa_allocator_desc *alloc);

void alloc_index_destroy(void);

#endif