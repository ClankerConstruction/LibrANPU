/* SPDX-License-Identifier: GPL-3.0-only */
/* Kernel type names for the shared ABI header, little-endian NPU */
#ifndef __FW_ABI_TYPES_H
#define __FW_ABI_TYPES_H

#include <stdint.h>

typedef uint8_t __u8;
typedef uint16_t __u16;
typedef unsigned int __u32;
typedef unsigned long long __u64;
typedef uint16_t __le16;
typedef unsigned int __le32;
typedef unsigned long long __le64;

#ifndef BIT
#define BIT(n)		(1u << (n))
#endif
#define GENMASK(h, l)	((~0u >> (31 - (h))) & (~0u << (l)))
#define FIELD_PREP(m, v)	(((unsigned int)(v) << __builtin_ctz(m)) & (m))
#define FIELD_GET(m, v)		(((v) & (m)) >> __builtin_ctz(m))

#endif
