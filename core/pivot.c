// SPDX-License-Identifier: GPL-2.0
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include "alloc.h"
#include "core.h"
#include "debug.h"
#include "layout.h"
#include "shadow_pt.h"
#include "stack_vma.h"
#include <linux/auxvec.h>
#include <linux/err.h>
#include <linux/mm.h>
#include <linux/mmap_lock.h>
#include <linux/pid.h>
#include <linux/ptrace.h>
#include <linux/sched/task_stack.h>
#include <linux/spinlock.h>

/**
 * sa_pivot - Moves the user stack of the calling process from the original region to the one
 * located in `new_stack_address`, and creates the corresponding allocator and thread objects.
 * @rsp: stack pointer of the calling thread (it's needed to locate the vma of the stack area)
 * @new_stack_address: address of the new stack area
 *
 * Returns 0 on success, <0 otherwise
 */
long sa_pivot(u64 rsp, u64 new_stack_address, u64 *sa_delta)
{
	struct mm_struct *mm;
	struct sa_allocator_desc *alloc;
	struct sa_thread_desc *thread;
	struct vm_area_struct *old_area, *new_area;
	unsigned long old_start, new_start, sp, addr;
	size_t old_len;
	long delta, err, n;
	int i;

	mm = current->mm;
	if (!mm)
		return -EINVAL;

	alloc = alloc_new(mm);
	if (IS_ERR(alloc))
		return PTR_ERR(alloc);

	thread = &alloc->main;
	mmap_write_lock(mm);
	if (!sa_area_empty(mm)) {
		/* not our tables: do not free anything */
		err = -EBUSY;
		goto err_unlock_nofree;
	}

	old_area = find_vma(mm, rsp);
	if (!old_area || old_area->vm_start > rsp) {
		sa_user_error("no_vma for rsp %llx\n", rsp);
		err = -EINVAL;
		goto err_unlock;
	}

	new_area = find_vma(mm, new_stack_address);
	if (!new_area || new_area->vm_start > new_stack_address ||
	    !sa_is_stack_vma(new_area)) {
		sa_user_error("no vma for new stack %llx or invalid mapping\n",
			      new_stack_address);
		err = -EINVAL;
		goto err_unlock;
	}

	new_start = new_area->vm_start;
	if (!valid_stack_base(new_start) ||
	    new_area->vm_end - new_area->vm_start != shdwstck_size) {
		sa_user_error("bad new stack vma [%lx-%lx]\n",
			      new_area->vm_start, new_area->vm_end);
		err = -EINVAL;
		goto err_unlock;
	}

	sp = current_pt_regs()->sp;
	if (sp < new_start || sp >= new_start + shdwstck_size) {
		err = -EINVAL;
		goto err_unlock;
	}

	n = 0;
	for (addr = ALIGN_DOWN(sp, PAGE_SIZE); addr < new_start + shdwstck_size;
	     addr += PAGE_SIZE) {
		err = sa_install_shadow_page(mm, shdwstck_addr(addr), NULL);
		if (err < 0)
			goto err_unlock;

		n += err;
	}

	sa_debug(SA_DEBUG_PAGES, "pivot: %ld shadow pages installed\n", n);
	sa_debug(SA_DEBUG_PGTABLE, "pivot: area p4d after install %lx\n",
		 (unsigned long)p4d_val(*sa_area_p4d(mm, shdwstck_start)));
	old_start = old_area->vm_start;
	old_len = old_area->vm_end - old_area->vm_start;
	mmap_write_unlock(mm);
	thread->stack_base = new_start;
	thread->owner = get_pid(task_pid(current));
	thread->state = SA_OWNED;
	delta = new_stack_address - rsp;
	spin_lock(&mm->arg_lock);
	mm->start_stack = new_start;
	mm->arg_start += delta;
	mm->arg_end += delta;
	mm->env_start += delta;
	mm->env_end += delta;
	for (i = 0; i < AT_VECTOR_SIZE; i += 2) {
		unsigned long key = mm->saved_auxv[i];

		if (key == AT_NULL)
			break;

		if (key == AT_PLATFORM || key == AT_RANDOM || key == AT_EXECFN)
			mm->saved_auxv[i + 1] += delta;
	}

	spin_unlock(&mm->arg_lock);
	vm_munmap(old_start, old_len);
	*sa_delta = shdwstck_delta;
	sa_debug(SA_DEBUG_PIVOT,
		 "pivot: [%lx-%lx) -> [%lx-%lx), delta %ld, shadow %llx\n",
		 old_start, old_start + old_len, new_start,
		 new_start + shdwstck_size, delta,
		 shdwstck_addr(new_stack_address));
	return 0;

err_unlock:
	mmap_write_unlock(mm);
	sa_free_shadow_area(mm);
	goto err_out;

err_unlock_nofree:
	mmap_write_unlock(mm);
err_out:
	sa_debug(SA_DEBUG_FAILED, "failed with error %ld\n", err);
	alloc_destroy(alloc);
	return err;
}