/* SPDX-License-Identifier: GPL-3.0-only */
/*
 * Event ring, NPU to host. Hart 0 is the only producer. Room for one
 * CMD_DONE per command slot is kept, so completions are never lost.
 */
#ifndef __CTL_EVT_H
#define __CTL_EVT_H

#include "fw/types.h"

void evt_init(void *ring, u32 entries, u32 reserve);
int evt_post(u16 type, u32 seq, const void *payload, u16 len);
/* publish posted events, raise the host interrupt when due */
void evt_flush(bool urgent);
u32 evt_dropped(void);

#endif
