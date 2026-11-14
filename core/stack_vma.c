// SPDX-License-Identifier: GPL-2.0
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include "stack_vma.h"
#include "alloc.h"
#include "compat.h"
#include "core.h"
#include "debug.h"
#include "layout.h"
#include "shadow_pt.h"
#include <linux/err.h>
#include <linux/gfp.h>
#include <linux/highmem.h>
#include <linux/mm.h>
#include <linux/mmap_lock.h>
#include <linux/ptrace.h>
#include <linux/sched/task_stack.h>
#include <linux/sched/mm.h>
#include <linux/version.h>

/*
 * The stack VMA is a private mapping of the device. A fault maps this read-only
 * zero page, the write that follows makes the private copy (COW) and installs
 * the shadow page of the faulting address.
 */
static struct page *sa_zero_page;

static int sa_stack_access(struct vm_area_struct *vma, unsigned long addr,
			   void *buf, int len, int write)
{
	unsigned flags;
	int off;
	struct page *page;
	void *kaddr;

	flags = FOLL_FORCE | (write ? FOLL_WRITE : 0);
	off = offset_in_page(addr);
	len = min_t(int, len, PAGE_SIZE - off);
	if (get_user_pages_remote(vma->vm_mm, addr, 1, flags, &page, NULL) != 1)
		return -EFAULT;

	kaddr = kmap_local_page(page);
	if (write) {
		memcpy(kaddr + off, buf, len);
		set_page_dirty_lock(page);
	} else {
		memcpy(buf, kaddr + off, len);
	}

	kunmap_local(kaddr);
	put_page(page);
	return len;
}

static vm_fault_t sa_stack_fault(struct vm_fault *vmf)
{
	struct mm_struct *mm = vmf->vma->vm_mm;
	struct sa_allocator_desc *alloc = alloc_find(mm);
	u64 addr = (vmf->address & PAGE_MASK);
	int ret = 0;

	if (alloc) {
		if (READ_ONCE(alloc->broken))
			return VM_FAULT_SIGSEGV;

		ret = sa_install_shadow_page(mm, addr + shdwstck_delta, NULL);
		if (ret < 0)
			return VM_FAULT_OOM;
	}

	sa_debug(SA_DEBUG_PAGES, "fault %llx: shadow %s\n", addr,
		 !alloc ? "skipped (no allocator)" :
		 ret	? "installed" :
			  "already present");
	get_page(sa_zero_page);
	vmf->page = sa_zero_page;
	return 0;
}

/*
 * Called by dup_mmap() in the context of the parent, with both mm write-locked,
 * for every VMA copied into the child. Only the stack of the forking thread
 * (the one containing sp) is given a shadow: the other threads do not exist
 * in the child.
 */
static void sa_stack_open(struct vm_area_struct *vma)
{
	struct mm_struct *child = vma->vm_mm;
	unsigned long sp = current_pt_regs()->sp;
	struct sa_allocator_desc *alloc;
	long err;

	/* split or mremap in the same mm: nothing to do */
	if (child == current->mm)
		return;

	if (sp < vma->vm_start || sp >= vma->vm_end)
		return;

	if (!alloc_find(current->mm))
		return;

	alloc = alloc_new(child);
	if (IS_ERR(alloc)) {
		sa_debug(SA_DEBUG_FAILED | SA_DEBUG_FORK,
			 "fork: no allocator for child (%ld)\n",
			 PTR_ERR(alloc));
		return;
	}

	err = sa_copy_shadow_locked(current->mm, child, vma->vm_start, sp);
	if (err) {
		/* open cannot fail the fork: the child dies at its first fault */
		WRITE_ONCE(alloc->broken, true);
		sa_debug(SA_DEBUG_FAILED | SA_DEBUG_FORK,
			 "fork: copy failed (%ld)\n", err);
	}

	alloc->main_stack = vma->vm_start;
}

/*
 * munmap, split and mremap leave the area alone: it lives as long as the mm.
 * mm_users is 0 only inside exit_mmap(), which calls close for every VMA with
 * mmap_write_lock held, after freeing the user page tables (the shadow area
 * is in the kernel half and is not touched) and before the pgd is freed.
 * The first stack VMA closed frees everything, the others find no allocator.
 */
static void sa_stack_close(struct vm_area_struct *vma)
{
	struct mm_struct *mm = vma->vm_mm;
	struct sa_allocator_desc *alloc;

	alloc = alloc_find(mm);
	if (!alloc)
		return;

	if (atomic_read(&mm->mm_users)) {
		struct sa_gather g = { .mm = mm };
		unsigned long nr;

		sa_unmap_range(&g, vma->vm_start, vma->vm_end);
		nr = sa_gather_finish(&g);
		sa_debug(SA_DEBUG_PAGES,
			 "close [%lx-%lx): %lu shadow pages freed\n",
			 vma->vm_start, vma->vm_end, nr);
		return;
	}

	alloc_teardown_locked(alloc);
}

static const struct vm_operations_struct sa_stack_vm_ops = {
	.access = sa_stack_access,
	.close = sa_stack_close,
	.fault = sa_stack_fault,
	.open = sa_stack_open,
};

bool sa_is_stack_vma(struct vm_area_struct *vma)
{
	return vma->vm_ops == &sa_stack_vm_ops;
}

#if SA_HAVE_MMAP_PREPARE
int sa_mmap_prepare(struct vm_area_desc *desc)
{
	bool cow;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(7, 0, 0)
	cow = vma_desc_is_cow_mapping(desc);
#else
	cow = is_cow_mapping(desc->vm_flags);
#endif
	if (!cow || !sa_stack_range_ok(desc->start, desc->end))
		return -EINVAL;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(7, 0, 0)
	vma_desc_set_flags(desc, VMA_DONTEXPAND_BIT);
#else
	desc->vm_flags |= VM_DONTEXPAND;
#endif
	desc->vm_ops = &sa_stack_vm_ops;
	sa_debug(SA_DEBUG_PAGES, "mmap stack [%lx-%lx)\n", desc->start,
		 desc->end);
	return 0;
}
#else
int sa_mmap(struct file *file, struct vm_area_struct *vma)
{
	if (!is_cow_mapping(vma->vm_flags) ||
	    !sa_stack_range_ok(vma->vm_start, vma->vm_end))
		return -EINVAL;

	vm_flags_set(vma, VM_DONTEXPAND);
	vma->vm_ops = &sa_stack_vm_ops;
	return 0;
}
#endif

long sa_stack_vma_init(void)
{
	sa_zero_page = alloc_page(GFP_KERNEL | __GFP_ZERO);
	if (!sa_zero_page) {
		sa_debug(SA_DEBUG_FAILED, "cannot allocate zero page\n");
		return -ENOMEM;
	}

	return 0;
}

void sa_stack_vma_exit(void)
{
	if (sa_zero_page)
		__free_page(sa_zero_page);

	sa_zero_page = NULL;
}