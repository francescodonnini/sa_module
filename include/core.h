/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _SHDWSTCK_CORE_H
#define _SHDWSTCK_CORE_H
#include <linux/fs.h>
#include <linux/mm_types.h>
#include <linux/types.h>
#include <linux/version.h>

long sa_init(void);

void sa_exit(void);

void alloc_index_destroy(void);

long resolve_symbols(void);

/* ioctl handlers */
long sa_pivot(u64 rsp, u64 new_stack_address, u64 *sa_delta, u64 *out_old_start,
	      u64 *out_old_len);

long sa_prepare(u64 stack_base, bool detached);

long sa_get_offset(u64 *offset);

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 16, 0)
#define SA_HAVE_MMAP_PREPARE 1
#endif

#if SA_HAVE_MMAP_PREPARE
int sa_mmap_prepare(struct vm_area_desc *desc);
#else
int sa_mmap(struct file *file, struct vm_area_struct *vma);
#endif

#endif