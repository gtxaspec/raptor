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
