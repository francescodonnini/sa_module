// SPDX-License-Identifier: GPL-2.0
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include "core.h"
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
