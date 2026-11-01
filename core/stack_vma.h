/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _SHDWSTCK_STACK_VMA_H
#define _SHDWSTCK_STACK_VMA_H
#include <linux/mm_types.h>
#include <linux/types.h>

/* true if @vma is a stack mapped from /dev/shdwstck */
bool sa_is_stack_vma(struct vm_area_struct *vma);

long sa_stack_vma_init(void);

void sa_stack_vma_exit(void);

#endif