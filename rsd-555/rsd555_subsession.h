/*
 * rsd555_subsession.h -- live555 ServerMediaSubsession implementations
 */

#ifndef RSD555_SUBSESSION_H
#define RSD555_SUBSESSION_H

#include <OnDemandServerMediaSubsession.hh>

extern "C" {
#include "rsd555.h"
}

/* H.264 video subsession */
class RingH264Subsession : public OnDemandServerMediaSubsession {
public:
	static RingH264Subsession *createNew(UsageEnvironment &env, rsd555_video_ctx_t *ctx,
					     Boolean reuseSource);

protected:
	RingH264Subsession(UsageEnvironment &env, rsd555_video_ctx_t *ctx, Boolean reuseSource);
	virtual ~RingH264Subsession();
	virtual FramedSource *createNewStreamSource(unsigned clientSessionId, unsigned &estBitrate);
	virtual RTPSink *createNewRTPSink(Groupsock *rtpGroupsock, unsigned char rtpPayloadTypeIfDynamic,
					  FramedSource *inputSource);

private:
	rsd555_video_ctx_t *fCtx;
};

/* H.265 video subsession */
class RingH265Subsession : public OnDemandServerMediaSubsession {
public:
	static RingH265Subsession *createNew(UsageEnvironment &env, rsd555_video_ctx_t *ctx,
					     Boolean reuseSource);

protected:
	RingH265Subsession(UsageEnvironment &env, rsd555_video_ctx_t *ctx, Boolean reuseSource);
	virtual ~RingH265Subsession();
	virtual FramedSource *createNewStreamSource(unsigned clientSessionId, unsigned &estBitrate);
	virtual RTPSink *createNewRTPSink(Groupsock *rtpGroupsock, unsigned char rtpPayloadTypeIfDynamic,
					  FramedSource *inputSource);

private:
	rsd555_video_ctx_t *fCtx;
};

/* AAC audio subsession (RFC 3640, AAC-hbr) */
class RingAACSubsession : public OnDemandServerMediaSubsession {
public:
	static RingAACSubsession *createNew(UsageEnvironment &env, rsd555_audio_ctx_t *ctx,
					    Boolean reuseSource);

protected:
	RingAACSubsession(UsageEnvironment &env, rsd555_audio_ctx_t *ctx, Boolean reuseSource);
	virtual ~RingAACSubsession();
	virtual FramedSource *createNewStreamSource(unsigned clientSessionId, unsigned &estBitrate);
	virtual RTPSink *createNewRTPSink(Groupsock *rtpGroupsock, unsigned char rtpPayloadTypeIfDynamic,
					  FramedSource *inputSource);

private:
	rsd555_audio_ctx_t *fCtx;
};

/* G.711 audio subsession (u-law PT=0 or A-law PT=8) */
class RingG711Subsession : public OnDemandServerMediaSubsession {
public:
	static RingG711Subsession *createNew(UsageEnvironment &env, rsd555_audio_ctx_t *ctx,
					     Boolean reuseSource, Boolean isAlaw);

protected:
	RingG711Subsession(UsageEnvironment &env, rsd555_audio_ctx_t *ctx,
			   Boolean reuseSource, Boolean isAlaw);
	virtual ~RingG711Subsession();
	virtual FramedSource *createNewStreamSource(unsigned clientSessionId, unsigned &estBitrate);
	virtual RTPSink *createNewRTPSink(Groupsock *rtpGroupsock, unsigned char rtpPayloadTypeIfDynamic,
					  FramedSource *inputSource);

private:
	rsd555_audio_ctx_t *fCtx;
	Boolean fIsAlaw;
};

/* Opus audio subsession (RFC 7587) */
class RingOpusSubsession : public OnDemandServerMediaSubsession {
public:
	static RingOpusSubsession *createNew(UsageEnvironment &env, rsd555_audio_ctx_t *ctx,
					     Boolean reuseSource);

protected:
	RingOpusSubsession(UsageEnvironment &env, rsd555_audio_ctx_t *ctx, Boolean reuseSource);
	virtual ~RingOpusSubsession();
	virtual FramedSource *createNewStreamSource(unsigned clientSessionId, unsigned &estBitrate);
	virtual RTPSink *createNewRTPSink(Groupsock *rtpGroupsock, unsigned char rtpPayloadTypeIfDynamic,
					  FramedSource *inputSource);

private:
	rsd555_audio_ctx_t *fCtx;
};

/* L16 (raw PCM 16-bit) audio subsession */
class RingL16Subsession : public OnDemandServerMediaSubsession {
public:
	static RingL16Subsession *createNew(UsageEnvironment &env, rsd555_audio_ctx_t *ctx,
					    Boolean reuseSource);

protected:
	RingL16Subsession(UsageEnvironment &env, rsd555_audio_ctx_t *ctx, Boolean reuseSource);
	virtual ~RingL16Subsession();
	virtual FramedSource *createNewStreamSource(unsigned clientSessionId, unsigned &estBitrate);
	virtual RTPSink *createNewRTPSink(Groupsock *rtpGroupsock, unsigned char rtpPayloadTypeIfDynamic,
					  FramedSource *inputSource);

private:
	rsd555_audio_ctx_t *fCtx;
};

#endif /* RSD555_SUBSESSION_H */
