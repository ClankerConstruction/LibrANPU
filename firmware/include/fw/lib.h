/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef __FW_LIB_H
#define __FW_LIB_H

#include <stdarg.h>
#include "fw/types.h"

void *memcpy(void *dst, const void *src, size_t n);
void *memset(void *dst, int c, size_t n);
int memcmp(const void *a, const void *b, size_t n);

/* word copies for uncached host memory: both sides 4-byte aligned */
void copy_words(volatile u32 *dst, const volatile u32 *src, size_t bytes);

int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap);
int snprintf(char *buf, size_t size, const char *fmt, ...)
	__attribute__((format(printf, 3, 4)));

/* console: boot and fatal lines only, off unless the host asks */
void con_enable(bool on);
void con_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

#endif
