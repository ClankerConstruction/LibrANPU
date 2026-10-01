// SPDX-License-Identifier: GPL-3.0-only
/*
 * Host task: frames from the rx task and the ones the PPE handed back
 * to the host rx ring, by buffer id.
 * The host sees a new index as an interrupt; it is published after
 * mod_frames entries, when no frame is waiting, or after mod_cycles.
 */

#include "fw/csr.h"
#include "wlan/wlan.h"

#define HRX_PROD		8	/* +8 our index, +0xC the host's */
#define HRX_CONS		0xC

struct host_state {
	u32 epoch;
	struct spsc_cons in;
	struct spsc_cons ppe;
	struct spsc_prod drop;
	u32 widx;
	u32 hidx;			/* host index last read */
	u32 pending;			/* entries not published */
	u32 first;			/* cycles of the first pending entry */
	struct wlan_rx_msg seg[WLAN_MAX_SEGS];
	u32 nseg;
	u32 delivered;
	u32 segs;			/* the radio's host_segs */
	bool full;			/* the host ring was full last time */
	/* fixed per attach */
	u32 base;
	u32 entries;
	u32 esize;
	u32 regs;
	u32 buf;			/* buffer word but the id */
	u32 mod_frames;
	u32 mod_cycles;
};

static struct host_state hs __hart_local;

static void __attribute__((noinline)) host_reset(struct wlan_radio *r)
{
	struct wlan_host_ring *h = &r->hrx[0];

	hs.epoch = r->epoch;
	spsc_cons_init(&hs.in, r->rx2host);
	spsc_cons_init(&hs.ppe, r->ppe2host);
	spsc_prod_init(&hs.drop, r->host2buf);
	hs.widx = 0;
	hs.hidx = 0;
	hs.pending = 0;
	hs.nseg = 0;
	hs.segs = 0;
	hs.full = false;
	hs.base = h->base;
	hs.entries = h->entries;
	hs.esize = h->entry_size;
	hs.regs = h->regs;
	hs.buf = FIELD_PREP(LIBRANPU_HRX_OFFSET, r->headroom);
	hs.mod_frames = r->mod_frames;
	hs.mod_cycles = r->mod_cycles;
	/* held ids come back through the return ring like delivered ones */
	hs.delivered = r->held;
	WRITE_ONCE(r->delivered, r->held);
	REG32(hs.regs + HRX_PROD) = 0;
}

static u32 ring_room(bool reload)
{
	if (reload) {
		hs.hidx = REG32(hs.regs + HRX_CONS);
		if (unlikely(hs.hidx >= hs.entries))
			hs.hidx %= hs.entries;
	}
	return hs.hidx > hs.widx ? hs.hidx - hs.widx - 1 :
	       hs.entries - hs.widx + hs.hidx - 1;
}

static volatile struct libranpu_host_rx *host_entry(u32 idx)
{
	return (void *)(hs.base + hs.esize * idx);
}

/* the segments of one frame; done bits from the tail to the head */
static bool host_deliver(struct wlan_radio *r)
{
	u32 i, idx, len = 0, n = hs.nseg;

	if (ring_room(false) < n && ring_room(true) < n) {
		if (!hs.full)
			r->stats.host_full++;
		hs.full = true;
		return false;
	}
	hs.full = false;

	for (i = 0; i < n; i++)
		len += hs.seg[i].len;
	for (i = 0, idx = hs.widx; i < n; i++) {
		volatile struct libranpu_host_rx *e = host_entry(idx);
		struct wlan_rx_msg *m = &hs.seg[i];

		e->info = FIELD_PREP(LIBRANPU_HRX_SEGS, n) |
			  (m->info & ~WRX_LAST);
		e->data = 0;
		e->buf = FIELD_PREP(LIBRANPU_HRX_ID, m->id) | hs.buf;
		if (++idx == hs.entries)
			idx = 0;
	}
	wmb();
	/* idx is one past the last segment */
	for (i = n; i-- > 0;) {
		idx = idx ? idx - 1 : hs.entries - 1;
		host_entry(idx)->ctrl = LIBRANPU_HRX_DONE |
			FIELD_PREP(LIBRANPU_HRX_SEG_LEN, hs.seg[i].len) |
			FIELD_PREP(LIBRANPU_HRX_LEN, len) |
			(i == n - 1 ? LIBRANPU_HRX_LAST : 0);
	}

	if (!hs.pending)
		hs.first = cycles();
	hs.widx += n;
	if (hs.widx >= hs.entries)
		hs.widx -= hs.entries;
	hs.pending += n;
	hs.delivered += n;
	hs.segs += n;
	hs.nseg = 0;
	return true;
}

/* segments of a frame cut short go back to the pool */
static bool host_drop(struct wlan_radio *r, u16 id)
{
	if (!spsc_room(&hs.drop, 1))
		return false;
	*(u32 *)spsc_slot(&hs.drop, 0) = id;
	spsc_publish(&hs.drop, 1);
	r->stats.host_dropped++;
	return true;
}

static void host_publish(struct wlan_radio *r, bool idle)
{
	if (!hs.pending)
		return;
	if (!idle && hs.pending < hs.mod_frames &&
	    cycles() - hs.first < hs.mod_cycles)
		return;
	r->stats.host_segs = hs.segs;
	wmb();
	REG32(hs.regs + HRX_PROD) = hs.widx;
	WRITE_ONCE(r->delivered, hs.delivered);
	hs.pending = 0;
}

/* one input: rx task segments, or single frames back from the PPE */
static int host_take(struct wlan_radio *r, struct spsc_cons *in, u32 st,
		     int max)
{
	u32 avail = spsc_avail(in, 1), i = 0;
	int n = 0;

	while (n < max && i < avail) {
		struct wlan_rx_msg *m = spsc_peek(in, i);

		if (st == WLAN_STOPPING) {
			if (!host_drop(r, m->id))
				break;
		} else if (hs.nseg == WLAN_MAX_SEGS) {
			/* no last segment in reach: drop what is held */
			while (hs.nseg && host_drop(r, hs.seg[hs.nseg - 1].id))
				hs.nseg--;
			if (hs.nseg)
				break;
			continue;
		} else {
			hs.seg[hs.nseg++] = *m;
		}
		i++;
		n++;
		if (hs.nseg && (hs.seg[hs.nseg - 1].info & WRX_LAST) &&
		    !host_deliver(r))
			break;
		if (i == avail)
			avail = spsc_avail(in, i + 1);
	}
	/* the messages are copied: their slots go back at once */
	if (i)
		spsc_release(in, i);
	return n;
}
int wlan_host_task(struct task *t, int budget)
{
	struct wlan_radio *r = t->ctx;
	u32 st = wlan_state(r);
	int n = 0;

	if (st == WLAN_DETACHED)
		return 0;
	if (hs.epoch != r->epoch)
		host_reset(r);
	if (st != WLAN_RUNNING && st != WLAN_STOPPING) {
		WRITE_ONCE(r->ack[WT_HOST], st);
		return 0;
	}

	/* a frame the full ring refused goes first */
	if (hs.nseg && (hs.seg[hs.nseg - 1].info & WRX_LAST) &&
	    st == WLAN_RUNNING && !host_deliver(r))
		goto out;

	n = host_take(r, &hs.in, st, budget * 32);
	/* single segments: only between chains */
	if (!hs.nseg)
		n += host_take(r, &hs.ppe, st, budget * 32);

	if (st == WLAN_STOPPING) {
		while (hs.nseg && host_drop(r, hs.seg[hs.nseg - 1].id))
			hs.nseg--;
		host_publish(r, true);
		if (!hs.nseg && !spsc_avail(&hs.in, 1) &&
		    !spsc_avail(&hs.ppe, 1))
			WRITE_ONCE(r->ack[WT_HOST], st);
		return n;
	}
out:
	host_publish(r, !spsc_avail(&hs.in, 1) && !spsc_avail(&hs.ppe, 1));
	return n;
}


