// SPDX-License-Identifier: GPL-3.0-only
/*
 * Host task: frames from the rx task to the host rx ring, by buffer id.
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
	struct spsc_prod drop;
	u32 widx;
	u32 hidx;			/* host index last read */
	u32 pending;			/* entries not published */
	u32 first;			/* cycles of the first pending entry */
	struct wlan_rx_msg seg[WLAN_MAX_SEGS];
	u32 nseg;
	u32 delivered;
};

static struct host_state hs;

static void host_reset(struct wlan_radio *r)
{
	hs.epoch = r->epoch;
	spsc_cons_init(&hs.in, r->rx2host);
	spsc_prod_init(&hs.drop, r->host2buf);
	hs.widx = 0;
	hs.hidx = 0;
	hs.pending = 0;
	hs.nseg = 0;
	hs.delivered = 0;
	WRITE_ONCE(r->delivered, 0);
	REG32(r->hrx[0].regs + HRX_PROD) = 0;
}

static u32 ring_room(const struct wlan_host_ring *h, bool reload)
{
	if (reload)
		hs.hidx = REG32(h->regs + HRX_CONS) % h->entries;
	return (hs.hidx + h->entries - hs.widx - 1) % h->entries;
}

/* the segments of one frame; done bits from the tail to the head */
static bool host_deliver(struct wlan_radio *r)
{
	struct wlan_host_ring *h = &r->hrx[0];
	u32 i, len = 0, n = hs.nseg;

	if (ring_room(h, false) < n && ring_room(h, true) < n) {
		r->stats.host_full++;
		return false;
	}

	for (i = 0; i < n; i++)
		len += hs.seg[i].len;
	for (i = 0; i < n; i++) {
		struct wlan_rx_msg *m = &hs.seg[i];
		volatile struct libranpu_host_rx *e = (void *)(h->base +
			h->entry_size * ((hs.widx + i) % h->entries));

		e->info = FIELD_PREP(LIBRANPU_HRX_SEGS, n) |
			  FIELD_PREP(LIBRANPU_HRX_REASON, m->reason);
		e->data = FIELD_PREP(LIBRANPU_HRX_BAND, m->band);
		e->buf = FIELD_PREP(LIBRANPU_HRX_ID, m->id) |
			 FIELD_PREP(LIBRANPU_HRX_OFFSET, LIBRANPU_RX_HEADROOM);
	}
	wmb();
	for (i = n; i-- > 0;) {
		volatile struct libranpu_host_rx *e = (void *)(h->base +
			h->entry_size * ((hs.widx + i) % h->entries));

		e->ctrl = LIBRANPU_HRX_DONE |
			  FIELD_PREP(LIBRANPU_HRX_SEG_LEN, hs.seg[i].len) |
			  FIELD_PREP(LIBRANPU_HRX_LEN, len) |
			  (i == n - 1 ? LIBRANPU_HRX_LAST : 0);
	}

	if (!hs.pending)
		hs.first = cycles();
	hs.widx = (hs.widx + n) % h->entries;
	hs.pending += n;
	hs.delivered += n;
	r->stats.host_segs += n;
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
	if (!idle && hs.pending < r->mod_frames &&
	    cycles() - hs.first < r->mod_cycles)
		return;
	wmb();
	REG32(r->hrx[0].regs + HRX_PROD) = hs.widx;
	WRITE_ONCE(r->delivered, hs.delivered);
	hs.pending = 0;
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
	if (hs.nseg && (hs.seg[hs.nseg - 1].flags & WRX_LAST) &&
	    st == WLAN_RUNNING && !host_deliver(r))
		goto out;

	while (n < budget * 32 && spsc_avail(&hs.in, 1)) {
		struct wlan_rx_msg *m = spsc_peek(&hs.in, 0);

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
		spsc_release(&hs.in, 1);
		n++;
		if (hs.nseg && (hs.seg[hs.nseg - 1].flags & WRX_LAST) &&
		    !host_deliver(r))
			break;
	}

	if (st == WLAN_STOPPING) {
		while (hs.nseg && host_drop(r, hs.seg[hs.nseg - 1].id))
			hs.nseg--;
		host_publish(r, true);
		if (!hs.nseg && !spsc_avail(&hs.in, 1))
			WRITE_ONCE(r->ack[WT_HOST], st);
		return n;
	}
out:
	host_publish(r, !spsc_avail(&hs.in, 1));
	return n;
}
