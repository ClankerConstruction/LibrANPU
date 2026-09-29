// SPDX-License-Identifier: GPL-3.0-only

#include "fw/lib.h"

void *memcpy(void *dst, const void *src, size_t n)
{
	u8 *d = dst;
	const u8 *s = src;

	if (!(((uintptr_t)d | (uintptr_t)s) & 3)) {
		for (; n >= 4; n -= 4, d += 4, s += 4)
			*(u32 *)d = *(const u32 *)s;
	}
	while (n--)
		*d++ = *s++;
	return dst;
}

void *memset(void *dst, int c, size_t n)
{
	u8 *d = dst;
	u32 w = (u8)c * 0x01010101u;

	while (n && ((uintptr_t)d & 3)) {
		*d++ = (u8)c;
		n--;
	}
	for (; n >= 4; n -= 4, d += 4)
		*(u32 *)d = w;
	while (n--)
		*d++ = (u8)c;
	return dst;
}

int memcmp(const void *a, const void *b, size_t n)
{
	const u8 *x = a, *y = b;

	for (; n; n--, x++, y++)
		if (*x != *y)
			return *x - *y;
	return 0;
}

void copy_words(volatile u32 *dst, const volatile u32 *src, size_t bytes)
{
	size_t i;

	for (i = 0; i < ALIGN_UP(bytes, 4) / 4; i++)
		dst[i] = src[i];
}
