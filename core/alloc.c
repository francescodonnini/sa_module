// SPDX-License-Identifier: GPL-2.0
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include "alloc.h"
#include "compat.h"
#include "debug.h"
#include "shadow_pt.h"
#include <linux/err.h>
#include <linux/mmap_lock.h>
#include <linux/module.h>
#include <linux/pid.h>
#include <linux/slab.h>

static DEFINE_XARRAY(sa_allocators);

static inline unsigned long alloc_key(const struct mm_struct *mm)
{
	return (unsigned long)mm;
}

static inline long alloc_link(struct sa_allocator_desc *alloc)
{
	long err;

	err = xa_insert(&sa_allocators, alloc_key(alloc->mm), alloc,
			GFP_KERNEL);
	return err == -EBUSY ? -EEXIST : err;
}

static inline void alloc_unlink(struct sa_allocator_desc *alloc)
{
	void *old;

	old = xa_erase(&sa_allocators, alloc_key(alloc->mm));
	WARN_ON_ONCE(old != alloc);
}

struct sa_allocator_desc *alloc_find(struct mm_struct *mm)
{
	return xa_load(&sa_allocators, alloc_key(mm));
}

void alloc_index_destroy(void)
{
	WARN_ON(!xa_empty(&sa_allocators));
	xa_destroy(&sa_allocators);
}

struct sa_allocator_desc *alloc_new(struct mm_struct *mm)
{
	struct sa_allocator_desc *alloc;
	long err;

	alloc = kzalloc_obj(*alloc, GFP_KERNEL);
	if (!alloc)
		return ERR_PTR(-ENOMEM);

	alloc->mm = mm;
	xa_init(&alloc->threads);
	if (!try_module_get(THIS_MODULE)) {
		err = -ENODEV;
		goto err_free;
	}

	err = alloc_link(alloc);
	if (err) {
		if (err == -EEXIST)
			sa_user_error("mm %p already has an allocator\n", mm);
		goto err_module;
	}

	sa_debug(SA_DEBUG_ALLOCATOR, "allocator %p created for mm %p\n", alloc,
		 mm);
	return alloc;

err_module:
	module_put(THIS_MODULE);
err_free:
	kfree(alloc);
	return ERR_PTR(err);
}

/**
 * alloc_destroy - Removes @alloc from the index and frees it with its thread
 * descriptors. The shadow area is not touched.
 */
void alloc_destroy(struct sa_allocator_desc *alloc)
{
	struct sa_thread_desc *t;
	unsigned long tid;
	unsigned int nr = 0;

	alloc_unlink(alloc);
	put_pid(alloc->main.owner);
	xa_for_each(&alloc->threads, tid, t) {
		put_pid(t->owner);
		kfree(t);
		nr++;
	}
	xa_destroy(&alloc->threads);
	sa_debug(SA_DEBUG_ALLOCATOR, "allocator %p freed (%u thread slots)\n",
		 alloc, nr);
	kfree(alloc);
	module_put(THIS_MODULE);
}

/**
 * alloc_teardown_locked - Frees the whole shadow area of the mm and then @alloc.
 *
 * Context: mmap_write_lock(alloc->mm) held.
 */
void alloc_teardown_locked(struct sa_allocator_desc *alloc)
{
	mmap_assert_write_locked(alloc->mm);
	sa_free_shadow_area_locked(alloc->mm);
	alloc_destroy(alloc);
}
