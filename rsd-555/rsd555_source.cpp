/*
 * rsd555_source.cpp -- live555 FramedSource implementations
 *
 * Each source instance owns a private frame queue registered with
 * the reader thread. The reader fans out frames to all queues.
 */

#include "rsd555_source.h"

#include <string.h>
#include <sys/time.h>
#include <unistd.h>

/* ================================================================
 * RingVideoSource
 * ================================================================ */

RingVideoSource *RingVideoSource::createNew(UsageEnvironment &env, rsd555_video_ctx_t *ctx)
{
	RingVideoSource *src = new RingVideoSource(env, ctx);
	if (!src->fValid) {
		Medium::close(src);
		return NULL;
	}
	return src;
}

RingVideoSource::RingVideoSource(UsageEnvironment &env, rsd555_video_ctx_t *ctx)
	: FramedSource(env), fCtx(ctx), fValid(false), fPendingFrame(NULL), fPendingOffset(0)
{
	if (rsd555_queue_init(&fQueue, 16) != 0) {
		envir() << "RingVideoSource: queue init failed\n";
		return;
	}
	if (rsd555_video_add_source(fCtx, &fQueue) != 0) {
		envir() << "RingVideoSource: max sources exceeded\n";
		rsd555_queue_destroy(&fQueue);
		return;
	}
	if (fCtx->state &&
	    __atomic_load_n(&fCtx->state->active_clients, __ATOMIC_RELAXED) >=
	    fCtx->state->max_clients) {
		envir() << "RingVideoSource: max clients reached\n";
		rsd555_video_remove_source(fCtx, &fQueue);
		rsd555_queue_destroy(&fQueue);
		return;
	}

	envir().taskScheduler().turnOnBackgroundReadHandling(
		fQueue.event_fd, onDataAvailable, this);
	if (fCtx->state)
		__atomic_add_fetch(&fCtx->state->active_clients, 1, __ATOMIC_RELAXED);
	fValid = true;
}

RingVideoSource::~RingVideoSource()
{
	if (fValid) {
		envir().taskScheduler().turnOffBackgroundReadHandling(fQueue.event_fd);
		rsd555_video_remove_source(fCtx, &fQueue);
		rsd555_queue_destroy(&fQueue);
		if (fCtx->state)
			__atomic_sub_fetch(&fCtx->state->active_clients, 1, __ATOMIC_RELAXED);
	}
	if (fPendingFrame)
		rsd555_frame_free(fPendingFrame);
}

void RingVideoSource::doStopGettingFrames()
{
	FramedSource::doStopGettingFrames();
}

void RingVideoSource::onDataAvailable(void *clientData, int /*mask*/)
{
	RingVideoSource *src = (RingVideoSource *)clientData;
	uint64_t val;
	(void)read(src->fQueue.event_fd, &val, sizeof(val));
	if (src->isCurrentlyAwaitingData())
		src->deliverFrame();
}

static const uint8_t *find_start_code(const uint8_t *start, const uint8_t *end)
{
	for (const uint8_t *p = start; p + 3 < end; p++) {
		if (p[0] == 0 && p[1] == 0 && p[2] == 0 && p[3] == 1)
			return p;
	}
	return end;
}

void RingVideoSource::doGetNextFrame()
{
	deliverFrame();
}

void RingVideoSource::deliverFrame()
{
	if (!isCurrentlyAwaitingData())
		return;

	while (true) {
		if (!fPendingFrame) {
			fPendingFrame = rsd555_queue_pop(&fQueue);
			fPendingOffset = 0;
			if (!fPendingFrame)
				return;
		}

		rsd555_shared_frame_t *sf = fPendingFrame->shared;
		const uint8_t *data = sf->data;
		uint32_t len = sf->len;
		const uint8_t *end = data + len;
		const uint8_t *p = data + fPendingOffset;

		const uint8_t *sc = find_start_code(p, end);
		if (sc >= end) {
			rsd555_frame_free(fPendingFrame);
			fPendingFrame = NULL;
			fPendingOffset = 0;
			continue;
		}

		const uint8_t *nalu_start = sc + 4;
		const uint8_t *nalu_end = find_start_code(nalu_start, end);
		uint32_t nalu_len = (uint32_t)(nalu_end - nalu_start);

		if (nalu_len == 0) {
			fPendingOffset = (uint32_t)(nalu_end - data);
			continue;
		}

		gettimeofday(&fPresentationTime, NULL);

		if (nalu_len > fMaxSize) {
			fFrameSize = fMaxSize;
			fNumTruncatedBytes = nalu_len - fMaxSize;
		} else {
			fFrameSize = nalu_len;
			fNumTruncatedBytes = 0;
		}
		memcpy(fTo, nalu_start, fFrameSize);

		fPendingOffset = (uint32_t)(nalu_end - data);

		afterGetting(this);
		return;
	}
}

/* ================================================================
 * RingAudioSource
 * ================================================================ */

RingAudioSource *RingAudioSource::createNew(UsageEnvironment &env, rsd555_audio_ctx_t *ctx)
{
	RingAudioSource *src = new RingAudioSource(env, ctx);
	if (!src->fValid) {
		Medium::close(src);
		return NULL;
	}
	return src;
}

RingAudioSource::RingAudioSource(UsageEnvironment &env, rsd555_audio_ctx_t *ctx)
	: FramedSource(env), fCtx(ctx), fValid(false)
{
	if (rsd555_queue_init(&fQueue, 32) != 0) {
		envir() << "RingAudioSource: queue init failed\n";
		return;
	}
	if (rsd555_audio_add_source(fCtx, &fQueue) != 0) {
		envir() << "RingAudioSource: max sources exceeded\n";
		rsd555_queue_destroy(&fQueue);
		return;
	}
	envir().taskScheduler().turnOnBackgroundReadHandling(
		fQueue.event_fd, onDataAvailable, this);
	fValid = true;
}

RingAudioSource::~RingAudioSource()
{
	if (fValid) {
		envir().taskScheduler().turnOffBackgroundReadHandling(fQueue.event_fd);
		rsd555_audio_remove_source(fCtx, &fQueue);
		rsd555_queue_destroy(&fQueue);
	}
}

void RingAudioSource::doStopGettingFrames()
{
	FramedSource::doStopGettingFrames();
}

void RingAudioSource::onDataAvailable(void *clientData, int /*mask*/)
{
	RingAudioSource *src = (RingAudioSource *)clientData;
	uint64_t val;
	(void)read(src->fQueue.event_fd, &val, sizeof(val));
	if (src->isCurrentlyAwaitingData())
		src->deliverFrame();
}

void RingAudioSource::doGetNextFrame()
{
	deliverFrame();
}

void RingAudioSource::deliverFrame()
{
	if (!isCurrentlyAwaitingData())
		return;

	rsd555_frame_t *f = rsd555_queue_pop(&fQueue);
	if (!f)
		return;

	rsd555_shared_frame_t *sf = f->shared;
	gettimeofday(&fPresentationTime, NULL);

	if (sf->len > fMaxSize) {
		fFrameSize = fMaxSize;
		fNumTruncatedBytes = sf->len - fMaxSize;
	} else {
		fFrameSize = sf->len;
		fNumTruncatedBytes = 0;
	}
	memcpy(fTo, sf->data, fFrameSize);

	rsd555_frame_free(f);
	afterGetting(this);
}
