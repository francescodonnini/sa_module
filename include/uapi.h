/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _SHDWSTCK_UAPI_H
#define _SHDWSTCK_UAPI_H
#include <linux/ioctl.h>
#include <linux/types.h>

#define IOCTL_SHADOW_MAGIC 0xca
#define IOCTL_SHADOW_PIVOT_NO 0x74
#define IOCTL_SHADOW_GET_OFFSET_NO 0x88

struct ioctl_pivot_params {
	__s64 error;
	__u64 new_stack_address;
	__u64 rsp;
	__u64 delta;
	__u64 old_start;
	__u64 old_len;
};

#define IOCTL_SHADOW_PIVOT                               \
	_IOWR(IOCTL_SHADOW_MAGIC, IOCTL_SHADOW_PIVOT_NO, \
	      struct ioctl_pivot_params)
#define IOCTL_SHADOW_GET_OFFSET \
	_IOR(IOCTL_SHADOW_MAGIC, IOCTL_SHADOW_GET_OFFSET_NO, __u64)
#endif
