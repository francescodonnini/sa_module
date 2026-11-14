// SPDX-License-Identifier: GPL-2.0
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include "shadow_pt.h"
#include "debug.h"
#include "layout.h"
#include "pt_bindings.h"
#include <linux/atomic.h>
#include <linux/gfp.h>
#include <linux/highmem.h>
#include <linux/mm.h>
#include <linux/mmap_lock.h>
#include <linux/moduleparam.h>
#include <linux/sysfs.h>

static atomic_long_t sa_live_pages = ATOMIC_LONG_INIT(0);

static int sa_live_pages_get(char *buf, const struct kernel_param *kp)
{
	return sysfs_emit(buf, "%ld\n", atomic_long_read(&sa_live_pages));
}

static const struct kernel_param_ops sa_live_pages_ops = {
	.get = sa_live_pages_get,
};

module_param_cb(live_shadow_pages, &sa_live_pages_ops, NULL, 0444);

struct sa_batch {
	unsigned int nr;
	void *batch[];
};
#define MAX_BATCH_SIZE ((PAGE_SIZE - sizeof(struct sa_batch)) / sizeof(void *))

/**
 * sa_gather_flush - flushes the tlb and frees all the pages gathered earlier.
 */
static void sa_gather_flush(struct sa_gather *g)
{
	unsigned i;

	if (!(g->pages && g->pages->nr) && !(g->tables && g->tables->nr))
		return;

	my_flush_tlb_mm(g->mm);
	if (g->pages) {
		for (i = 0; i < g->pages->nr; ++i)
			__free_page(g->pages->batch[i]);

		g->pages->nr = 0;
	}

	if (g->tables) {
		for (i = 0; i < g->tables->nr; ++i)
			sa_free_table_page(g->tables->batch[i]);

		g->tables->nr = 0;
	}
}

static void sa_gather_add(struct sa_gather *g, struct sa_batch **b,
			  struct page *page, bool table)
{
	if (!*b) {
		*b = (struct sa_batch *)__get_free_page(GFP_NOWAIT);
		if (!*b) {
			my_flush_tlb_mm(g->mm);
			if (table)
				sa_free_table_page(page);
			else
				__free_page(page);

			return;
		}

		(*b)->nr = 0;
	}

	(*b)->batch[(*b)->nr++] = page;
	if ((*b)->nr == MAX_BATCH_SIZE)
		sa_gather_flush(g);
}

#define sa_gather_page(g, p) sa_gather_add(g, &(g)->pages, p, false)
#define sa_gather_table(g, p) sa_gather_add(g, &(g)->tables, p, true)

unsigned long sa_gather_finish(struct sa_gather *g)
{
	sa_gather_flush(g);
	if (g->pages)
		free_page((unsigned long)g->pages);

	if (g->tables)
		free_page((unsigned long)g->tables);

	return g->nr_freed;
}

static pmd_t *sa_walk_alloc_pmd(struct mm_struct *mm, unsigned long addr)
{
	pgd_t *pgd;
	p4d_t *p4d;
	pud_t *pud;

	pgd = pgd_offset(mm, addr);
	p4d = my_p4d_alloc(mm, pgd, addr);
	if (!p4d) {
		sa_debug(SA_DEBUG_PGTABLE | SA_DEBUG_FAILED,
			 "cannot allocate p4d table page\n");
		return NULL;
	}

	pud = my_pud_alloc(mm, p4d, addr);
	if (!pud) {
		sa_debug(SA_DEBUG_PGTABLE | SA_DEBUG_FAILED,
			 "cannot allocate pud table page\n");
		return NULL;
	}
	return my_pmd_alloc(mm, pud, addr);
}

int sa_install_shadow_page(struct mm_struct *mm, unsigned long shadow,
			   struct page *page)
{
	spinlock_t *ptl;
	pmd_t *pmd;
	pte_t *pte;
	int ret = 0;

	pmd = sa_walk_alloc_pmd(mm, shadow);
	if (!pmd || my_pte_alloc(mm, pmd))
		goto nomem;

	if (!page) {
		pte = my_pte_offset_map_lock(mm, pmd, shadow, &ptl);
		if (!pte)
			goto nomem;
		ret = pte_none(ptep_get(pte));
		pte_unmap_unlock(pte, ptl);
		if (!ret)
			return 0;

		page = alloc_page(GFP_KERNEL | __GFP_ZERO);
		if (!page)
			return -ENOMEM;

		ret = 0;
	}

	pte = my_pte_offset_map_lock(mm, pmd, shadow, &ptl);
	if (!pte)
		goto nomem;

	if (pte_none(ptep_get(pte))) {
		set_pte_at(mm, shadow, pte, mk_pte(page, PAGE_SHARED));
		page = NULL;
		ret = 1;
	}

	pte_unmap_unlock(pte, ptl);
	if (page)
		__free_page(page);

	if (ret)
		atomic_long_inc(&sa_live_pages);

	return ret;

nomem:
	if (page)
		__free_page(page);

	return -ENOMEM;
}

struct page *sa_lookup_shadow_page(struct mm_struct *mm, unsigned long shadow)
{
	struct page *page = NULL;
	spinlock_t *ptl;
	pgd_t *pgd;
	p4d_t *p4d;
	pud_t *pud;
	pmd_t *pmd;
	pte_t *pte;

	pgd = pgd_offset(mm, shadow);
	if (pgd_none(*pgd) || pgd_bad(*pgd))
		return NULL;

	p4d = p4d_offset(pgd, shadow);
	if (p4d_none(*p4d) || p4d_bad(*p4d))
		return NULL;

	pud = pud_offset(p4d, shadow);
	if (pud_none(*pud) || pud_bad(*pud))
		return NULL;

	pmd = pmd_offset(pud, shadow);
	if (pmd_none(*pmd) || pmd_bad(*pmd))
		return NULL;

	pte = my_pte_offset_map_lock(mm, pmd, shadow, &ptl);
	if (!pte)
		return NULL;

	if (pte_present(ptep_get(pte))) {
		page = pte_page(ptep_get(pte));
		get_page(page);
	}

	pte_unmap_unlock(pte, ptl);
	return page;
}

long sa_copy_shadow_locked(struct mm_struct *parent_mm, struct mm_struct *mm,
			   u64 stack_base, u64 sp)
{
	u64 shadow = shdwstck_addr(stack_base);
	unsigned long nr = shdwstck_size >> PAGE_SHIFT;
	unsigned long first = (sp - stack_base) >> PAGE_SHIFT;
	int i, copied = 0;
	long err = 0;

	mmap_assert_locked(parent_mm);
	mmap_assert_locked(mm);
	for (i = 0; i < nr && err >= 0; ++i) {
		struct page *old, *new;

		old = sa_lookup_shadow_page(parent_mm, shadow + i * PAGE_SIZE);
		if (!old)
			continue;

		new = alloc_page(GFP_KERNEL | __GFP_ZERO);
		if (new) {
			if (i >= first)
				copy_highpage(new, old);

			err = sa_install_shadow_page(mm, shadow + i * PAGE_SIZE,
						     new);
			if (err > 0)
				copied += err;
		} else {
			err = -ENOMEM;
		}
		put_page(old);
	}

	sa_debug(SA_DEBUG_FORK, "fork: %d shadow pages copied\n", copied);
	return err < 0 ? err : 0;
}

static void unmap_pte_range(struct sa_gather *g, pmd_t *pmd, unsigned long addr,
			    unsigned long end)
{
	pte_t *pte = pte_offset_kernel(pmd, addr);

	do {
		pte_t old;

		if (pte_none(ptep_get(pte)))
			continue;

		old = ptep_get_and_clear(g->mm, addr, pte);
		if (pte_present(old)) {
			sa_gather_page(g, pte_page(old));
			g->nr_freed++;
			atomic_long_dec(&sa_live_pages);
		}

	} while (pte++, addr += PAGE_SIZE, addr != end);
}

static void unmap_pmd_range(struct sa_gather *g, pud_t *pud, unsigned long addr,
			    unsigned long end)
{
	pmd_t *pmd = pmd_offset(pud, addr);
	unsigned long next;

	do {
		next = pmd_addr_end(addr, end);
		if (pmd_none(*pmd) || pmd_bad(*pmd))
			continue;

		unmap_pte_range(g, pmd, addr, next);
	} while (pmd++, addr = next, addr != end);
}

static void unmap_pud_range(struct sa_gather *g, p4d_t *p4d, unsigned long addr,
			    unsigned long end)
{
	pud_t *pud = pud_offset(p4d, addr);
	unsigned long next;

	do {
		next = pud_addr_end(addr, end);
		if (pud_none(*pud) || pud_bad(*pud))
			continue;

		unmap_pmd_range(g, pud, addr, next);
	} while (pud++, addr = next, addr != end);
}

static void unmap_p4d_range(struct sa_gather *g, pgd_t *pgd, unsigned long addr,
			    unsigned long end)
{
	p4d_t *p4d = p4d_offset(pgd, addr);
	unsigned long next;

	do {
		next = p4d_addr_end(addr, end);
		if (p4d_none(*p4d) || p4d_bad(*p4d))
			continue;

		unmap_pud_range(g, p4d, addr, next);
	} while (p4d++, addr = next, addr != end);
}

static inline void unmap_range(struct sa_gather *g, u64 addr, u64 end)
{
	unsigned long next;
	pgd_t *pgd;

	mmap_assert_locked(g->mm);
	sa_debug(SA_DEBUG_PGTABLE, "unmapping [%llx-%llx)\n", addr, end);
	pgd = pgd_offset(g->mm, addr);
	do {
		next = pgd_addr_end(addr, end);
		if (pgd_none(*pgd) || pgd_bad(*pgd))
			continue;

		unmap_p4d_range(g, pgd, addr, next);
	} while (pgd++, addr = next, addr != end);
}

void sa_unmap_range(struct sa_gather *g, unsigned long start, unsigned long end)
{
	unmap_range(g, shdwstck_addr(start), shdwstck_addr(end));
}

static void free_pte_range(struct sa_gather *g, pmd_t *pmd, unsigned long addr,
			   unsigned long end)
{
	pgtable_t pte;

	unmap_pte_range(g, pmd, addr, end);
	pte = pmd_pgtable(*pmd);
	pmd_clear(pmd);
	sa_gather_table(g, pte);
	mm_dec_nr_ptes(g->mm);
}

static void free_pmd_range(struct sa_gather *g, pud_t *pud, unsigned long addr,
			   unsigned long end)
{
	pmd_t *pmd;
	unsigned long next;
	unsigned long start;

	start = addr;
	pmd = pmd_offset(pud, addr);
	do {
		next = pmd_addr_end(addr, end);
		if (!pmd_none(*pmd))
			sa_debug(SA_DEBUG_PGTABLE, "pmd %lx = %lx\n", addr,
				 (unsigned long)pmd_val(*pmd));

		if (pmd_none(*pmd) || pmd_bad(*pmd))
			continue;

		free_pte_range(g, pmd, addr, next);
	} while (pmd++, addr = next, addr != end);

	pmd = pmd_offset(pud, start);
	pud_clear(pud);
	sa_gather_table(g, virt_to_page(pmd));
	mm_dec_nr_pmds(g->mm);
}

static void free_pud_range(struct sa_gather *g, p4d_t *p4d, unsigned long addr,
			   unsigned long end)
{
	pud_t *pud;
	unsigned long next;
	unsigned long start;

	start = addr;
	pud = pud_offset(p4d, addr);
	do {
		next = pud_addr_end(addr, end);
		if (!pud_none(*pud))
			sa_debug(SA_DEBUG_PGTABLE, "pud %lx = %lx\n", addr,
				 (unsigned long)pud_val(*pud));

		if (pud_none(*pud) || pud_bad(*pud))
			continue;

		free_pmd_range(g, pud, addr, next);
	} while (pud++, addr = next, addr != end);

	pud = pud_offset(p4d, start);
	p4d_clear(p4d);
	sa_gather_table(g, virt_to_page(pud));
	mm_dec_nr_puds(g->mm);
}

static void free_p4d_range(struct sa_gather *g, pgd_t *pgd, unsigned long addr,
			   unsigned long end)
{
	p4d_t *p4d;
	unsigned long next;
	unsigned long start;

	start = addr;
	p4d = p4d_offset(pgd, addr);
	do {
		next = p4d_addr_end(addr, end);
		sa_debug(SA_DEBUG_PGTABLE, "p4d %lx = %lx\n", addr,
			 (unsigned long)p4d_val(*p4d));
		if (p4d_none(*p4d) || p4d_bad(*p4d))
			continue;

		free_pud_range(g, p4d, addr, next);
	} while (p4d++, addr = next, addr != end);

	if (!pgtable_l5_enabled())
		return;

	p4d = p4d_offset(pgd, start);
	pgd_clear(pgd);
	sa_gather_table(g, virt_to_page(p4d));
}

/**
 * my_free_pgd_range - Unmaps and frees page tables in the range [addr, end).
 */
static void my_free_pgd_range(struct sa_gather *g, unsigned long addr,
			      unsigned long end)
{
	pgd_t *pgd;
	unsigned long next;

	mmap_assert_write_locked(g->mm);
	sa_debug(SA_DEBUG_PGTABLE, "freeing page tables [%lx-%lx) mm %p\n",
		 addr, end, g->mm);
	pgd = pgd_offset(g->mm, addr);
	do {
		next = pgd_addr_end(addr, end);
		if (pgd_none(*pgd) || pgd_bad(*pgd))
			continue;

		free_p4d_range(g, pgd, addr, next);
	} while (pgd++, addr = next, addr != end);
}

/**
 * sa_free_shadow_area_locked - Frees every page and page table of the area.
 *
 * Context: mmap_write_lock(mm) held.
 */
unsigned long sa_free_shadow_area_locked(struct mm_struct *mm)
{
	struct sa_gather g = { .mm = mm };
	unsigned long nr;

	my_free_pgd_range(&g, shdwstck_start, shdwstck_end);
	nr = sa_gather_finish(&g);
	sa_debug(SA_DEBUG_PAGES, "freed %lu shadow pages\n", nr);
	return nr;
}

unsigned long sa_free_shadow_area(struct mm_struct *mm)
{
	unsigned long nr;

	mmap_write_lock(mm);
	nr = sa_free_shadow_area_locked(mm);
	mmap_write_unlock(mm);
	return nr;
}

/**
 * sa_area_empty - Checks that no top-level entry of the shadow area is populated
 * in @mm. The module is the only one that will ever allocate tables there.
 */
bool sa_area_empty(struct mm_struct *mm)
{
	unsigned long a;

	for (a = shdwstck_start; a < shdwstck_end;
	     a = p4d_addr_end(a, shdwstck_end)) {
		p4d_t *p4d = sa_area_p4d(mm, a);

		if (!p4d_none(*p4d)) {
			sa_debug(SA_DEBUG_FAILED,
				 "shadow area not empty: p4d %lx = %lx\n", a,
				 (unsigned long)p4d_val(*p4d));
			return false;
		}
	}
	return true;
}
