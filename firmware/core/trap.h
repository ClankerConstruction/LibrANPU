/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef __CORE_TRAP_H
#define __CORE_TRAP_H

#include "fw/types.h"

/*
 * A probe that may fault arms the catch: the faulting access is
 * skipped and its cause kept, instead of parking the hart.
 */
void trap_catch(bool on);
/* mcause of the last caught fault, 0 if none, then cleared */
u32 trap_caught(void);

#endif
