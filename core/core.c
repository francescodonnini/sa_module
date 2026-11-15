// SPDX-License-Identifier: GPL-2.0
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include "core.h"
#include "alloc.h"
#include "layout.h"
#include "stack_vma.h"

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