/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef __CTL_CTL_H
#define __CTL_CTL_H

#include "core/task.h"
#include <linux/soc/airoha/libranpu_abi.h>

int ctl_task(struct task *t, int budget);
int health_task(struct task *t, int budget);

void caps_fill(struct libranpu_caps *caps);
u32 harts_up(void);

#endif
