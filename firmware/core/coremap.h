/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef __CORE_COREMAP_H
#define __CORE_COREMAP_H

/* hart 0, before the others start: every hart's task list */
int coremap_build(void);

#endif
