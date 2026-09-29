/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef __DBG_PROBE_H
#define __DBG_PROBE_H

#include "core/task.h"

int probe_init(void);
/* ctx is the hart; on hart 0 it also drives the control side */
int probe_task(struct task *t, int budget);

#endif
