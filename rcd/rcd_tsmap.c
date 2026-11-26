/*
 * rcd_tsmap.c -- map an external camera's clock onto CLOCK_MONOTONIC
 */

#include "rcd_tsmap.h"

#define TSMAP_WINDOW_US 10000000
#define TSMAP_SLEW_US	2000

int64_t rcd_tsmap(rcd_tsmap_t *m, uint64_t cam_us, int64_t local_us)
{
	int64_t d = local_us - (int64_t)cam_us;

	if (!m->init || cam_us < m->last_in) {
		if (m->init)
			m->resets++;
		m->init = true;
		m->offset = d;
		m->win_min = d;
		m->win_start = local_us;
	}
	m->last_in = cam_us;

	if (d < m->offset)
		m->offset = d;
	if (d < m->win_min)
		m->win_min = d;
	if (local_us - m->win_start >= TSMAP_WINDOW_US) {
		if (m->win_min > m->offset) {
			int64_t step = m->win_min - m->offset;
			m->offset += step > TSMAP_SLEW_US ? TSMAP_SLEW_US : step;
		}
		m->win_min = d;
		m->win_start = local_us;
	}

	int64_t out = (int64_t)cam_us + m->offset;
	if (m->last_out && out <= m->last_out)
		out = m->last_out + 1;
	m->last_out = out;
	return out;
}

#define SEQ_RESTART_US 1000000

uint32_t rcd_seq(rcd_seq_t *q, uint64_t cam_us, uint64_t period_us, uint64_t *eff)
{
	if (period_us == 0)
		period_us = 1;
	if (!q->init) {
		q->init = true;
		q->last = cam_us;
		*eff = cam_us;
		return q->seq;
	}
	if (cam_us + SEQ_RESTART_US < q->last) {
		q->resets++;
		q->seq++;
		q->last = cam_us;
		*eff = cam_us;
		return q->seq;
	}
	uint64_t t = cam_us > q->last ? cam_us : q->last + period_us;
	uint64_t steps = (t - q->last + period_us / 2) / period_us;
	q->seq += steps ? (uint32_t)steps : 1;
	q->last = t;
	*eff = t;
	return q->seq;
}

int rcd_backoff_take(rcd_backoff_t *b, int64_t now_us, int64_t *wait_us)
{
	if (b->next_ok && now_us < b->next_ok) {
		*wait_us = b->next_ok - now_us;
		if (b->warned_for == b->next_ok)
			return RCD_BACKOFF_REFUSE_QUIET;
		b->warned_for = b->next_ok;
		return RCD_BACKOFF_REFUSE_WARN;
	}
	int64_t spacing = b->spacing ? b->spacing : RCD_BACKOFF_MIN_US;
	b->next_ok = now_us + spacing;
	b->spacing = spacing >= RCD_BACKOFF_LAST_US ? RCD_BACKOFF_MAX_US : spacing * 2;
	b->stream_since = 0;
	*wait_us = 0;
	return RCD_BACKOFF_GO;
}

void rcd_backoff_streaming(rcd_backoff_t *b, int64_t now_us)
{
	if (!b->stream_since || now_us - b->last_note > RCD_BACKOFF_GAP_US)
		b->stream_since = now_us;
	b->last_note = now_us;
	if (now_us - b->stream_since >= RCD_BACKOFF_STABLE_US)
		b->spacing = 0;
}
