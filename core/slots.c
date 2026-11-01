// SPDX-License-Identifier: GPL-2.0
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include "alloc.h"
#include "compat.h"
#include "core.h"
#include "debug.h"
#include "layout.h"
#include "shadow_pt.h"
#include "stack_vma.h"
#include <linux/minmax.h>
#include <linux/mm.h>
#include <linux/mmap_lock.h>
#include <linux/pid.h>
#include <linux/ptrace.h>
#include <linux/sched/task.h>
#include <linux/sched/task_stack.h>
#include <linux/slab.h>
#include <linux/uaccess.h>

long sa_prepare(u64 stack_base, bool detached)
{
	struct sa_allocator_desc *alloc;
	struct sa_thread_desc *t;
	unsigned long id;
	long err;
	struct mm_struct *mm;
	struct vm_area_struct *vma;
	bool ok;

	mm = current->mm;
	if (!mm)
		return -EINVAL;

	if (!valid_stack_base(stack_base)) {
		sa_user_error("invalid stack base %llx\n", stack_base);
		return -EINVAL;
	}

	alloc = alloc_find(mm);
	if (!alloc) {
		sa_user_error("no allocator for current mm\n");
		return -EINVAL;
	}

	if (stack_base == alloc->main.stack_base) {
		sa_user_error("%llx is the main stack\n", stack_base);
		return -EBUSY;
	}

	mmap_read_lock(mm);
	// We have to make sure that the VMA containing stack_base is backed by this module
	vma = find_vma(mm, stack_base);
	ok = vma && vma->vm_start == stack_base &&
	     vma->vm_end == stack_base + shdwstck_size && sa_is_stack_vma(vma);
	mmap_read_unlock(mm);
	if (!ok)
		return -EINVAL;

	t = kzalloc_obj(struct sa_thread_desc, GFP_KERNEL);
	if (!t) {
		sa_debug(SA_DEBUG_FAILED, "cannot allocate descriptor\n");
		return -ENOMEM;
	}

	t->stack_base = stack_base;
	t->state = SA_PREPARED;
	t->detached = detached;
	id = shdwstck_idx(stack_base);
	err = xa_insert(&alloc->threads, id, t, GFP_KERNEL);
	if (err == -EBUSY) {
		sa_user_error("slot %lu already in use\n", id);
		goto free_desc;
	}

	if (err) {
		sa_debug(SA_DEBUG_FAILED, "xa_insert slot %lu failed: %ld\n",
			 id, err);
		goto free_desc;
	}

	sa_debug(SA_DEBUG_SLOT,
		 "slot %lu prepared (base %llx, shadow %llx, detached %d)\n",
		 id, stack_base, shdwstck_addr(stack_base), detached);
	return 0;

free_desc:
	kfree(t);
	return err;
}

/**
 * sa_alloc - Makes the calling thread the owner of the prepared slot at
 * @stack_base.
 *
 * Context: Process context.
 * Returns 0 on success, < 0 otherwise.
 */
long sa_alloc(u64 stack_base)
{
	unsigned long sp;
	struct sa_allocator_desc *alloc;
	unsigned long id;
	struct pid *pid;
	long err;
	struct sa_thread_desc *t;

	if (!valid_stack_base(stack_base)) {
		sa_user_error("invalid stack base %llx\n", stack_base);
		return -EINVAL;
	}

	sp = current_pt_regs()->sp;
	if (sp < stack_base || sp >= stack_base + shdwstck_size) {
		sa_user_error("sp %lx outside stack [%llx-%llx)\n", sp,
			      stack_base, stack_base + shdwstck_size);
		return -EINVAL;
	}

	alloc = alloc_find(current->mm);
	if (!alloc) {
		sa_user_error("no allocator for current mm\n");
		return -EINVAL;
	}

	id = shdwstck_idx(stack_base);
	pid = get_pid(task_pid(current));
	err = -EINVAL;
	xa_lock(&alloc->threads);
	t = xa_load(&alloc->threads, id);
	if (t && t->state == SA_PREPARED) {
		t->owner = pid;
		t->state = SA_OWNED;
		err = 0;
	}

	xa_unlock(&alloc->threads);
	if (err) {
		sa_user_error("slot %lu not prepared (state %d)\n", id,
			      t ? t->state : -1);
		put_pid(pid);
	}

	sa_debug(SA_DEBUG_SLOT, "slot %lu owned (base %llx)\n", id, stack_base);
	return err;
}

long sa_detach(u64 stack_base)
{
	struct mm_struct *mm;
	struct sa_allocator_desc *alloc;
	struct sa_thread_desc *t;
	unsigned long id;
	long err = 0;

	mm = current->mm;
	if (!mm || !valid_stack_base(stack_base)) {
		sa_user_error("invalid stack base %llx\n", stack_base);
		return -EINVAL;
	}

	alloc = alloc_find(mm);
	if (!alloc) {
		sa_user_error("no allocator for current mm\n");
		return -EINVAL;
	}

	if (stack_base == alloc->main.stack_base) {
		sa_debug(SA_DEBUG_SLOT, "main stack, ignored\n");
		return 0;
	}

	id = shdwstck_idx(stack_base);
	xa_lock(&alloc->threads);
	t = xa_load(&alloc->threads, id);
	if (t)
		t->detached = true;
	else
		err = -EINVAL;

	xa_unlock(&alloc->threads);

	if (err)
		sa_user_error("slot %lu not found\n", id);
	else
		sa_debug(SA_DEBUG_SLOT, "slot %lu detached\n", id);

	return err;
}

static inline bool owner_gone(struct pid *owner)
{
	struct task_struct *task;
	bool gone;

	rcu_read_lock();
	task = pid_task(owner, PIDTYPE_PID);
	if (!task) {
		gone = true;
	} else {
		task_lock(task);
		gone = (task->flags & PF_EXITING) && !task->mm;
		task_unlock(task);
	}

	rcu_read_unlock();
	return gone;
}

static void sa_desc_release_list(struct sa_allocator_desc *alloc,
				 struct list_head *list)
{
	struct sa_gather g = { .mm = alloc->mm };
	struct sa_thread_desc *t, *tmp;
	unsigned long nr;

	if (list_empty(list))
		return;

	mmap_write_lock(alloc->mm);
	list_for_each_entry(t, list, list)
		sa_unmap_slot(&g, t->stack_base);

	nr = sa_gather_finish(&g);
	mmap_write_unlock(alloc->mm);
	sa_debug(SA_DEBUG_PAGES, "freed %lu shadow pages\n", nr);

	list_for_each_entry_safe(t, tmp, list, list) {
		put_pid(t->owner);
		kfree(t);
	}
}

long sa_reclaim(u64 stack_base)
{
	struct mm_struct *mm;
	struct sa_allocator_desc *alloc;
	struct sa_thread_desc *t;
	unsigned long index;
	LIST_HEAD(dead);
	const char *why = NULL;
	long err;

	mm = current->mm;
	if (!mm)
		return -EINVAL;

	if (!valid_stack_base(stack_base)) {
		sa_user_error("invalid stack base %llx\n", stack_base);
		return -EINVAL;
	}

	alloc = alloc_find(mm);
	if (!alloc) {
		sa_user_error("no allocator for current mm\n");
		return -EINVAL;
	}

	err = 0;
	index = shdwstck_idx(stack_base);
	xa_lock(&alloc->threads);
	t = xa_load(&alloc->threads, index);
	if (!t || t->state != SA_OWNED) {
		why = "not owned";
		err = -EINVAL;
	} else if (t->detached) {
		why = "detached";
		err = -EINVAL;
	} else if (!owner_gone(t->owner)) {
		err = -EAGAIN;
	} else {
		__xa_erase(&alloc->threads, index);
		list_add(&t->list, &dead);
	}

	xa_unlock(&alloc->threads);
	if (err == -EINVAL) {
		sa_user_error("slot %lu %s\n", index, why);
		return err;
	}

	if (err == -EAGAIN) {
		sa_debug(SA_DEBUG_RECLAIM, "slot %lu: owner still alive\n",
			 index);
		return err;
	}

	sa_desc_release_list(alloc, &dead);
	sa_debug(SA_DEBUG_RECLAIM, "slot %lu reclaimed\n", index);
	return err;
}

long sa_reclaim_dead(u64 __user *reclaimed, unsigned cap)
{
	struct mm_struct *mm;
	struct sa_allocator_desc *alloc;
	struct sa_thread_desc *t;
	unsigned long index;
	unsigned n;
	LIST_HEAD(dead);
	u64 *bases;
	long ret;

	mm = current->mm;
	if (!mm)
		return -EINVAL;

	alloc = alloc_find(mm);
	if (!alloc) {
		sa_user_error("no allocator for current mm\n");
		return -EINVAL;
	}

	cap = min_t(unsigned, cap, 256);
	if (!cap)
		return 0;

	bases = kmalloc_array(cap, sizeof(*bases), GFP_KERNEL);
	if (!bases) {
		sa_debug(SA_DEBUG_FAILED, "cannot allocate %u bases\n", cap);
		return -ENOMEM;
	}

	xa_lock(&alloc->threads);
	n = 0;
	xa_for_each(&alloc->threads, index, t) {
		if (t->state != SA_OWNED || !t->detached ||
		    !owner_gone(t->owner))
			continue;

		__xa_erase(&alloc->threads, index);
		list_add(&t->list, &dead);
		bases[n++] = t->stack_base;
		if (n == cap)
			break;
	}

	xa_unlock(&alloc->threads);
	sa_desc_release_list(alloc, &dead);
	sa_debug(SA_DEBUG_RECLAIM, "reclaimed %u detached slots (cap %u)\n", n,
		 cap);
	ret = n;
	if (n && copy_to_user(reclaimed, bases, n * sizeof(*bases))) {
		sa_user_error("bad user buffer %p\n", reclaimed);
		ret = -EFAULT;
	}

	kfree(bases);
	return ret;
}

long sa_unprepare(u64 stack_base)
{
	struct mm_struct *mm;
	struct sa_allocator_desc *alloc;
	struct sa_thread_desc *t;
	unsigned long index;
	LIST_HEAD(dead);
	long err = -EINVAL;
	int state = -1;

	if (!valid_stack_base(stack_base)) {
		sa_user_error("invalid stack base %llx\n", stack_base);
		return -EINVAL;
	}

	mm = current->mm;
	if (!mm)
		return -EINVAL;

	alloc = alloc_find(mm);
	if (!alloc) {
		sa_user_error("no allocator for current mm\n");
		return -EINVAL;
	}

	index = shdwstck_idx(stack_base);
	xa_lock(&alloc->threads);
	t = xa_load(&alloc->threads, index);
	if (t) {
		state = t->state;
		if (t->state == SA_PREPARED) {
			__xa_erase(&alloc->threads, index);
			list_add(&t->list, &dead);
			err = 0;
		}
	}

	xa_unlock(&alloc->threads);
	if (err) {
		sa_user_error(
			"sa_unprepare: slot %lu not prepared (state %d)\n",
			index, state);
		return err;
	}

	sa_desc_release_list(alloc, &dead);
	sa_debug(SA_DEBUG_SLOT, "slot %lu unprepared\n", index);
	return err;
}