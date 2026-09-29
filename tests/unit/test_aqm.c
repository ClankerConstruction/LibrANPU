// SPDX-License-Identifier: GPL-3.0-only

#include "harness.h"
#include "wlan/aqm.h"

#define MHZ		1		/* 1 cycle per us */
#define MS		1000

static struct aqm_cfg cfg;

/* q frames in the chip, each taking delay_ms; returns drops in span */
static u32 run(struct aqm_sta *s, u32 *now, u32 q, u32 delay_ms,
	       u32 span_ms, u32 len)
{
	u32 drops = 0, end = *now + span_ms * MS;
	u16 tok = 0;

	while ((s16)(s->sent - s->done) < (s16)q)
		aqm_sent(s, tok++, *now);
	for (; (s32)(*now - end) < 0; *now += 100) {
		/* one frame out, one in, 100 us apart: a standing queue */
		aqm_done(s, s->probe, *now);
		s->delay = delay_ms * MS;
		if (aqm_decide(&cfg, s, len, *now) != AQM_PASS)
			drops++;
		else
			aqm_sent(s, tok++, *now);
	}
	return drops;
}

TEST(short_delay_never_drops)
{
	struct aqm_sta s;
	u32 now = 0;

	aqm_defaults(&cfg, MHZ);
	aqm_sta_init(&s);
	CHECK_EQ(run(&s, &now, 500, 5, 2000, 1500), 0);
}

TEST(standing_delay_drops_faster_in_time)
{
	struct aqm_sta s;
	u32 now = 0, first, later;

	aqm_defaults(&cfg, MHZ);
	aqm_sta_init(&s);
	/* nothing in the first interval, then CoDel drops */
	CHECK_EQ(run(&s, &now, 500, 20, 99, 1500), 0);
	first = run(&s, &now, 500, 20, 500, 1500);
	later = run(&s, &now, 500, 20, 500, 1500);
	CHECK(first >= 3);
	CHECK(later > first);
	/* below target again: no more drops */
	CHECK_EQ(run(&s, &now, 500, 2, 1000, 1500), 0);
}

TEST(small_and_short_queues_pass)
{
	struct aqm_sta s;
	u32 now = 0;

	aqm_defaults(&cfg, MHZ);
	aqm_sta_init(&s);
	CHECK_EQ(run(&s, &now, 500, 50, 1000, 200), 0);
	aqm_sta_init(&s);
	CHECK_EQ(run(&s, &now, 32, 50, 1000, 1500), 0);
}

TEST(hard_limit)
{
	struct aqm_sta s;
	u16 t;

	aqm_defaults(&cfg, MHZ);
	cfg.limit = 100;
	aqm_sta_init(&s);
	for (t = 0; t < 100; t++)
		aqm_sent(&s, t, 0);
	CHECK_EQ(aqm_decide(&cfg, &s, 64, 0), AQM_LIMIT);
	aqm_done(&s, 5, 10);
	CHECK_EQ(aqm_decide(&cfg, &s, 64, 10), AQM_PASS);
}

TEST(lost_report_retimes)
{
	struct aqm_sta s;
	u16 t;

	aqm_defaults(&cfg, MHZ);
	aqm_sta_init(&s);
	for (t = 7; t < 7 + 64; t++)
		aqm_sent(&s, t, 0);
	CHECK_EQ(s.probe, 7);
	/* past the lost time the probe is dropped and the next is timed */
	aqm_decide(&cfg, &s, 1500, 2000 * MS);
	CHECK_EQ(s.probe, AQM_NONE);
	aqm_sent(&s, 100, 2000 * MS);
	aqm_done(&s, 100, 2003 * MS);
	CHECK_EQ(s.delay, 3 * MS);
}

TEST(off_passes_all)
{
	struct aqm_sta s;
	u32 now = 0;

	aqm_defaults(&cfg, MHZ);
	cfg.on = 0;
	aqm_sta_init(&s);
	CHECK_EQ(run(&s, &now, 500, 50, 1000, 1500), 0);
}
