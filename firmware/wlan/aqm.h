/* SPDX-License-Identifier: GPL-3.0-only */
/*
 * Per-station limit on LAN to WiFi frames in the chip: a hard limit,
 * and CoDel drops against a standing in-chip delay.
 */
#ifndef __WLAN_AQM_H
#define __WLAN_AQM_H

#include "fw/types.h"

#define AQM_STAS		1024	/* stations by wcid; others unmanaged */
#define AQM_INTERVAL_MAX_US	150000
#define AQM_NONE		0xFFFF
#define AQM_NOT_SENT		0xFFFE	/* a token the chip does not hold */

/* times in cycles */
struct aqm_cfg {
	u32 on;
	u32 limit;			/* frames */
	u32 target;			/* frames above which it stands; 0 off */
	u32 delay;			/* in-chip time above which it stands */
	u32 interval;
	u32 min_q;			/* frames; a shorter queue never stands */
	u32 small;			/* bytes; never dropped early */
	u32 lost;			/* a timed frame older than this is gone */
};

struct aqm_sta {
	u16 sent, done;
	u16 count, last;		/* drops now, when dropping last began */
	u16 probe;			/* token timed through the chip */
	u8 flags;
	u8 rsv;
	u32 probe_t;
	u32 delay;			/* the last timed frame's time in the chip */
	u32 above;			/* when above target becomes standing */
	u32 next;			/* next drop */
};

enum aqm_verdict {
	AQM_PASS,
	AQM_LIMIT,
	AQM_DROP,
};

void aqm_defaults(struct aqm_cfg *c, u32 mhz);
void aqm_sta_init(struct aqm_sta *s);
enum aqm_verdict aqm_decide(const struct aqm_cfg *c, struct aqm_sta *s,
			    u32 len, u32 now);
void aqm_sent(struct aqm_sta *s, u16 tok, u32 now);
void aqm_done(struct aqm_sta *s, u16 tok, u32 now);
bool aqm_dropping(const struct aqm_sta *s);

#endif
