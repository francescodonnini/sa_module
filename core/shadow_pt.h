/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _SHDWSTCK_SHADOW_PT_H
#define _SHDWSTCK_SHADOW_PT_H
#include <linux/mm_types.h>
#include <linux/pgtable.h>
#include <linux/types.h>

struct sa_batch;

struct sa_gather {
	struct mm_struct *mm;
	struct sa_batch *tables;
	struct sa_batch *pages;
	unsigned long nr_freed;
};

void sa_unmap_slot(struct sa_gather *g, unsigned long stack_base);

unsigned long sa_gather_finish(struct sa_gather *g);

int sa_install_shadow_page(struct mm_struct *mm, unsigned long shadow,
			   struct page *page);

struct page *sa_lookup_shadow_page(struct mm_struct *mm, unsigned long shadow);

long sa_copy_shadow_locked(struct mm_struct *parent_mm, struct mm_struct *mm,
			   u64 stack_base, u64 sp);

void sa_unmap_range(struct sa_gather *g, unsigned long start,
		    unsigned long end);

unsigned long sa_unmap_slot_locked(struct mm_struct *mm, u64 stack_base);

unsigned long sa_free_shadow_area_locked(struct mm_struct *mm);

unsigned long sa_free_shadow_area(struct mm_struct *mm);

static inline p4d_t *sa_area_p4d(struct mm_struct *mm, unsigned long addr)
{
	return p4d_offset(pgd_offset(mm, addr), addr);
}

bool sa_area_empty(struct mm_struct *mm);

#endif