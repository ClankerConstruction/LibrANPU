/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef __FW_TYPES_H
#define __FW_TYPES_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "fw/abi_types.h"

typedef uint8_t u8;
typedef uint16_t u16;
typedef unsigned int u32;
typedef unsigned long long u64;
typedef int8_t s8;
typedef int16_t s16;
typedef int s32;
typedef long long s64;

#define ARRAY_SIZE(a)		(sizeof(a) / sizeof((a)[0]))
#define ALIGN_UP(x, a)		(((x) + (a) - 1) & ~((a) - 1))
#define IS_POW2(x)		((x) && !((x) & ((x) - 1)))
#define MIN(a, b)		((a) < (b) ? (a) : (b))
#define MAX(a, b)		((a) > (b) ? (a) : (b))

#define __section(s)		__attribute__((section(s)))
#define __aligned(n)		__attribute__((aligned(n)))
#define __noreturn		__attribute__((noreturn))
#define __unused		__attribute__((unused))
#define likely(x)		__builtin_expect(!!(x), 1)
#define unlikely(x)		__builtin_expect(!!(x), 0)

#define READ_ONCE(x)		(*(const volatile typeof(x) *)&(x))
#define WRITE_ONCE(x, v)	(*(volatile typeof(x) *)&(x) = (v))

#define REG32(a)		(*(volatile u32 *)(uintptr_t)(a))

#define barrier()		__asm__ volatile("" ::: "memory")
#ifdef __riscv
/* the host and the other harts see stores in this order */
#define wmb()			__asm__ volatile("fence w, w" ::: "memory")
#define rmb()			__asm__ volatile("fence r, r" ::: "memory")
#define mb()			__asm__ volatile("fence rw, rw" ::: "memory")
/* loads before, stores after */
#define rwmb()			__asm__ volatile("fence r, w" ::: "memory")
#else
/* host unit tests */
#define wmb()			__atomic_thread_fence(__ATOMIC_RELEASE)
#define rmb()			__atomic_thread_fence(__ATOMIC_ACQUIRE)
#define mb()			__atomic_thread_fence(__ATOMIC_SEQ_CST)
#define rwmb()			__atomic_thread_fence(__ATOMIC_RELEASE)
#endif

#endif
