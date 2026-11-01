/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _SHDWSTCK_DEBUG_H
#define _SHDWSTCK_DEBUG_H
#include <linux/printk.h>
#include <linux/sched.h>
#include <linux/types.h>

enum {
	SA_DEBUG_USER_ERROR = 1U << 0,
	SA_DEBUG_FAILED = 1U << 1,
	SA_DEBUG_ALLOCATOR = 1U << 2,
	SA_DEBUG_PIVOT = 1U << 3,
	SA_DEBUG_FORK = 1U << 4,
	SA_DEBUG_SLOT = 1U << 5,
	SA_DEBUG_RECLAIM = 1U << 6,
	SA_DEBUG_FREELIST = 1U << 7,
	SA_DEBUG_PAGES = 1U << 8,
	SA_DEBUG_PGTABLE = 1U << 9,
	SA_DEBUG_TLB = 1U << 10,
};

extern u32 sa_debug_mask;

#define sa_debug(mask, fmt, ...)                                               \
	do {                                                                   \
		if (unlikely(sa_debug_mask & (mask)))                          \
			pr_info_ratelimited("%d:%d %s:%d " fmt, current->tgid, \
					    current->pid, __func__, __LINE__,  \
					    ##__VA_ARGS__);                    \
	} while (0)

#define sa_user_error(fmt, ...) \
	sa_debug(SA_DEBUG_USER_ERROR, fmt, ##__VA_ARGS__)
#endif