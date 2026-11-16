// SPDX-License-Identifier: GPL-2.0
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include "core.h"
#include "alloc.h"
#include "layout.h"
#include "stack_vma.h"
#include <linux/mm.h>
#include <linux/mman.h>
#include <linux/random.h>
#include <linux/sched.h>

long sa_init(void)
{
	long err;

	err = sa_layout_init();
	if (err)
		return err;

	return sa_stack_vma_init();
}

void sa_exit(void)
{
	sa_stack_vma_exit();
}

long sa_get_offset(u64 *offset)
{
	if (!current->mm || !alloc_find(current->mm))
		return -ENOENT;

	*offset = shdwstck_delta;
	return 0;
}

static unsigned long sa_find_gap(struct mm_struct *mm, unsigned long start,
				 unsigned long end, unsigned long len)
{
	const unsigned long real_len = len + 2 * shdwstck_guard;
	unsigned long cur = start;
	while (cur < end && (end - cur) >= real_len) {
		unsigned long gap_end;
		// VMA è NULL se non esistono area sopra cur
		// VMA non è NULL se cur cade all'interno di essa oppure se esiste una VMA
		// che inizia dopo di cur (vm_start > cur)
		struct vm_area_struct *vma = find_vma(mm, cur);
		if (vma &&
		    cur >= vma->vm_start) { /* cur cade all'interno della VMA */
			cur = vma->vm_end;
			continue;
		}

		gap_end = (vma && vma->vm_start < end) ? vma->vm_start : end;
		if (gap_end - cur >= real_len)
			return cur + shdwstck_guard;

		if (gap_end == end)
			break;

		cur = vma->vm_end;
	}

	return 0;
}

unsigned long sa_get_unmapped_area(struct file *filp, unsigned long addr,
				   unsigned long len, unsigned long pgoff,
				   unsigned long flags)
{
	struct mm_struct *mm = current->mm;
	const unsigned long lo = shdwstck_user_start;
	const unsigned long hi = shdwstck_user_end;
	unsigned long start;

	if (!len || len + 2 * shdwstck_guard > hi - lo)
		return -ENOMEM;

	// la posizione degli stack la sceglie il modulo
	if (flags & MAP_FIXED)
		return -EINVAL;

	start = lo +
		((unsigned long)get_random_u32_below((hi - lo) >> PAGE_SHIFT)
		 << PAGE_SHIFT);
	addr = sa_find_gap(mm, start, hi, len);
	if (!addr)
		addr = sa_find_gap(
			mm, lo, min(hi, start + len + 2 * shdwstck_guard), len);

	return addr ? addr : -ENOMEM;
}

long sa_layout(struct ioctl_layout *layout)
{
	struct sa_allocator_desc *alloc;

	memset(layout, 0, sizeof(*layout));
	layout->offset = shdwstck_delta;
	layout->region_start = shdwstck_user_start;
	layout->region_end = shdwstck_user_end;
	layout->unit = shdwstck_unit;
	alloc = current->mm ? alloc_find(current->mm) : NULL;
	if (alloc) {
		layout->main_start = alloc->main_stack;
		layout->main_size = alloc->main_size;
	}

	return 0;
}