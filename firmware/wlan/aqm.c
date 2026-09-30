// SPDX-License-Identifier: GPL-3.0-only
/*
 * Per-station limit on LAN to WiFi frames in the chip. Frames wait
 * there outside any host queue; a station's queue that stands above
 * target, in frames or in time, for an interval starts drops spaced
 * interval / sqrt(count), and dipping below ends them.
 */

#include "wlan/aqm.h"

#define ABOVE			BIT(0)
#define DROPPING		BIT(1)

void aqm_defaults(struct aqm_cfg *c, u32 mhz)
{
	c->on = 1;
	c->limit = 8192;
	c->target = 0;
	c->delay = 10000 * mhz;
	c->interval = 100000 * mhz;
	c->min_q = 64;
	c->small = 256;
	c->lost = 1000000 * mhz;
}

void aqm_sta_init(struct aqm_sta *s)
{
	*s = (struct aqm_sta){ .probe = AQM_NONE };
}

static u32 isqrt(u32 x)
{
	u32 r = 0, b = 1u << 30;

	while (b > x)
		b >>= 2;
	for (; b; b >>= 2) {
		if (x >= r + b) {
			x -= r + b;
			r = (r >> 1) + b;
		} else {
			r >>= 1;
		}
	}
	return r;
}

/* time in the chip: the last timed frame's, or the one out if older */
static u32 sta_delay(const struct aqm_cfg *c, struct aqm_sta *s, u32 now)
{
	u32 age;

	if (s->probe == AQM_NONE)
		return s->delay;
	age = now - s->probe_t;
	if (age > c->lost) {
		/* its report is gone: time the next one */
		s->probe = AQM_NONE;
		return s->delay;
	}
	return MAX(age, s->delay);
}

enum aqm_verdict aqm_decide(const struct aqm_cfg *c, struct aqm_sta *s,
			    u32 len, u32 now)
{
	s16 d = (s16)(s->sent - s->done);
	u32 q = d > 0 ? (u32)d : 0, n;
	bool above;

	if (!c->on)
		return AQM_PASS;
	if (c->limit && q >= c->limit)
		return AQM_LIMIT;

	above = (c->target && q >= c->target) ||
		(c->delay && q >= c->min_q && sta_delay(c, s, now) >= c->delay);
	if (!above) {
		s->flags = 0;
		return AQM_PASS;
	}
	if (!(s->flags & ABOVE)) {
		s->flags = ABOVE;
		s->above = now + c->interval;
		return AQM_PASS;
	}
	if ((s32)(now - s->above) < 0 || len <= c->small)
		return AQM_PASS;

	if (s->flags & DROPPING) {
		if ((s32)(now - s->next) < 0)
			return AQM_PASS;
		if (s->count != 0xFFFF)
			s->count++;
		/* no frame for a while: space from now, not from a late next */
		if ((s32)(now - s->next) > (s32)c->interval)
			s->next = now;
	} else {
		/* back to dropping soon after it ended: keep the rate */
		s32 since = now - s->next;

		n = (u16)(s->count - s->last);
		s->count = n > 1 && (since < 0 ||
				     (u64)since < 16ull * c->interval) ? n : 1;
		s->last = s->count;
		s->flags |= DROPPING;
		s->next = now;
	}
	s->next += c->interval / isqrt(s->count);
	return AQM_DROP;
}

bool aqm_dropping(const struct aqm_sta *s)
{
	return s->flags & DROPPING;
}

void aqm_sent(struct aqm_sta *s, u16 tok, u32 now)
{
	s->sent++;
	if (s->probe == AQM_NONE) {
		s->probe = tok;
		s->probe_t = now;
	}
}

void aqm_done(struct aqm_sta *s, u16 tok, u32 now)
{
	s->done++;
	if (s->probe == tok) {
		s->delay = now - s->probe_t;
		s->probe = AQM_NONE;
	}
}
