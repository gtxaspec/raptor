/*
 * rsd555_source.h -- live555 FramedSource subclasses for ring buffer consumption
 */

#ifndef RSD555_SOURCE_H
#define RSD555_SOURCE_H

#include <FramedSource.hh>

extern "C" {
#include "rsd555.h"
}

/* Video source: each instance owns a private frame queue.
 * The reader thread fans out frames to all registered queues. */
class RingVideoSource : public FramedSource {
public:
	static RingVideoSource *createNew(UsageEnvironment &env, rsd555_video_ctx_t *ctx);

protected:
	RingVideoSource(UsageEnvironment &env, rsd555_video_ctx_t *ctx);
	virtual ~RingVideoSource();
	virtual void doGetNextFrame();
	virtual void doStopGettingFrames();

private:
	static void onDataAvailable(void *clientData, int mask);
	void deliverFrame();

	rsd555_video_ctx_t *fCtx;
	rsd555_frame_queue_t fQueue;
	bool fValid;

	/* NALU parsing state: a single ring frame may contain multiple
	 * NALUs. We deliver one per doGetNextFrame call. */
	rsd555_frame_t *fPendingFrame;
	uint32_t fPendingOffset;
};

/* Audio source: each instance owns a private frame queue. */
class RingAudioSource : public FramedSource {
public:
	static RingAudioSource *createNew(UsageEnvironment &env, rsd555_audio_ctx_t *ctx);

protected:
	RingAudioSource(UsageEnvironment &env, rsd555_audio_ctx_t *ctx);
	virtual ~RingAudioSource();
	virtual void doGetNextFrame();
	virtual void doStopGettingFrames();

private:
	static void onDataAvailable(void *clientData, int mask);
	void deliverFrame();

	rsd555_audio_ctx_t *fCtx;
	rsd555_frame_queue_t fQueue;
	bool fValid;
};

#endif /* RSD555_SOURCE_H */
