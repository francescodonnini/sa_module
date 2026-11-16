// SPDX-License-Identifier: GPL-2.0
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include "layout.h"
#include "debug.h"
#include <linux/align.h>
#include <linux/cache.h>
#include <linux/errno.h>
#include <linux/mm.h>
#include <linux/moduleparam.h>
#include <asm/page.h>

u32 sa_debug_mask = SA_DEBUG_USER_ERROR | SA_DEBUG_FAILED;
module_param_named(debug_mask, sa_debug_mask, uint, 0644);

#ifdef SHADOWSTACK_DEBUG
#define SHDWSTCK_PARAM_PERM 0444
#else
#define SHDWSTCK_PARAM_PERM 0
#endif

unsigned long shdwstck_area_size __ro_after_init;
module_param(shdwstck_area_size, ulong, SHDWSTCK_PARAM_PERM);

unsigned long shdwstck_user_start __ro_after_init;
module_param(shdwstck_user_start, ulong, SHDWSTCK_PARAM_PERM);

unsigned long shdwstck_start __ro_after_init;
module_param(shdwstck_start, ulong, SHDWSTCK_PARAM_PERM);

unsigned long shdwstck_unit __ro_after_init;
module_param(shdwstck_unit, ulong, SHDWSTCK_PARAM_PERM);

unsigned long shdwstck_guard __ro_after_init = 1UL << 20;
module_param(shdwstck_guard, ulong, SHDWSTCK_PARAM_PERM);

unsigned long shdwstck_user_end __ro_after_init;
unsigned long shdwstck_end __ro_after_init;
unsigned long shdwstck_delta __ro_after_init;

bool sa_stack_range_ok(unsigned long start, unsigned long end)
{
	if (!PAGE_ALIGNED(start) || !PAGE_ALIGNED(end) || end <= start)
		return false;

	if (start < shdwstck_user_start || end > shdwstck_user_end)
		return false;

	return shdwstck_addr(start) >= shdwstck_start &&
	       shdwstck_addr(end) <= shdwstck_end;
}

long sa_layout_init(void)
{
	if (!shdwstck_start || !shdwstck_user_start || !shdwstck_area_size) {
		sa_debug(
			SA_DEBUG_FAILED,
			"area_size, user_stack_start e shadow_stack_start are mandatory\n");

		return -EINVAL;
	}

	if (shdwstck_start <= shdwstck_user_start)
		return -EINVAL;

	if (shdwstck_start <= TASK_SIZE)
		return -EINVAL;

	if (shdwstck_user_start >= TASK_SIZE)
		return -EINVAL;

	shdwstck_user_end = shdwstck_user_start + shdwstck_area_size;
	if (shdwstck_user_end >= TASK_SIZE)
		return -EINVAL;

	if (!PAGE_ALIGNED(shdwstck_guard))
		return -EINVAL;

	shdwstck_end = shdwstck_start + shdwstck_area_size;
	shdwstck_delta = shdwstck_start - shdwstck_user_start;
	return 0;
}