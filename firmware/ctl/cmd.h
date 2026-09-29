/* SPDX-License-Identifier: GPL-3.0-only */
/*
 * Command ring, host to NPU. Hart 0 takes commands in ring order; a
 * handler answers at once or later through cmd_finish.
 */
#ifndef __CTL_CMD_H
#define __CTL_CMD_H

#include "fw/types.h"
#include <linux/soc/airoha/libranpu_abi.h>

/* handler return: the answer comes later */
#define CMD_ASYNC		1

struct cmd_ref {
	u32 seq;
	u16 slot;
	u16 opcode;
	u8 service;
	u8 flags;
};

struct cmd_ctx {
	struct cmd_ref ref;
	u16 len;
	u16 rsp_len;
	u32 req[LIBRANPU_CMD_PAYLOAD / 4];
	u32 rsp[LIBRANPU_CMD_PAYLOAD / 4];
};

struct cmd_handler {
	u16 opcode;
	u16 min_len;
	int (*fn)(struct cmd_ctx *c);
};

struct cmd_service {
	const struct cmd_handler *h;
	u32 n;
};

void cmd_init(void *ring, u32 entries);
/* take up to budget commands; returns how many */
int cmd_poll(int budget);
void cmd_finish(const struct cmd_ref *ref, int status, const void *rsp,
		u16 len);

extern const struct cmd_service ctl_service;
extern const struct cmd_service dbg_service;

#endif
