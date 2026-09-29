/* SPDX-License-Identifier: GPL-3.0-only */
/*
 * Run-time allocator for NPU SRAM and cluster SRAM. Only hart 0
 * allocates; a detach frees every block of its owner at once.
 */
#ifndef __CORE_ARENA_H
#define __CORE_ARENA_H

#include "fw/types.h"

#define ARENA_BLOCKS		64

enum arena_owner {
	OWNER_FREE,
	OWNER_CORE,
	OWNER_DBG,
	OWNER_RADIO0,
	OWNER_RADIO1,
	OWNER_RADIO2,
	OWNER_SVC,
};

struct arena_block {
	u32 start;
	u32 size;
	u8 owner;
};

struct arena {
	u32 base;
	u32 size;
	u32 n;
	struct arena_block blk[ARENA_BLOCKS];	/* sorted by start */
};

void arena_init(struct arena *a, u32 base, u32 size);
void *arena_alloc(struct arena *a, u32 size, u32 align, u8 owner);
void arena_free_owner(struct arena *a, u8 owner);
u32 arena_free_bytes(const struct arena *a);

extern struct arena npu_sram;
extern struct arena cluster_sram;

#endif
