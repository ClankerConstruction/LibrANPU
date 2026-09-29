// SPDX-License-Identifier: GPL-3.0-only

#include "fw/lib.h"
#include "core/arena.h"

struct arena npu_sram;
struct arena cluster_sram;

void arena_init(struct arena *a, u32 base, u32 size)
{
	a->base = base;
	a->size = size;
	a->n = 0;
}

/* first fit; the block is zeroed */
void *arena_alloc(struct arena *a, u32 size, u32 align, u8 owner)
{
	u32 i, start = a->base, end;

	if (!size || !IS_POW2(align) || owner == OWNER_FREE ||
	    a->n == ARENA_BLOCKS)
		return NULL;

	for (i = 0; i <= a->n; i++) {
		end = i < a->n ? a->blk[i].start : a->base + a->size;
		start = ALIGN_UP(start, align);
		if (start >= a->base && start <= end && end - start >= size)
			break;
		if (i < a->n)
			start = a->blk[i].start + a->blk[i].size;
	}
	if (i > a->n)
		return NULL;

	for (u32 j = a->n; j > i; j--)
		a->blk[j] = a->blk[j - 1];
	a->blk[i].start = start;
	a->blk[i].size = size;
	a->blk[i].owner = owner;
	a->n++;

	memset((void *)(uintptr_t)start, 0, size);
	return (void *)(uintptr_t)start;
}

void arena_free_owner(struct arena *a, u8 owner)
{
	u32 i, j;

	for (i = 0, j = 0; i < a->n; i++)
		if (a->blk[i].owner != owner)
			a->blk[j++] = a->blk[i];
	a->n = j;
}

u32 arena_free_bytes(const struct arena *a)
{
	u32 i, used = 0;

	for (i = 0; i < a->n; i++)
		used += a->blk[i].size;
	return a->size - used;
}
