/*
 * rsd555_subsession.cpp -- live555 ServerMediaSubsession implementations
 *
 * Each subsession creates the appropriate source + framer + RTP sink
 * chain when a client does SETUP.
 */

#include "rsd555_subsession.h"
#include "rsd555_source.h"

#include <H264VideoStreamDiscreteFramer.hh>
#include <H264VideoRTPSink.hh>
#include <H265VideoStreamDiscreteFramer.hh>
#include <H265VideoRTPSink.hh>
#include <MPEG4GenericRTPSink.hh>
#include <SimpleRTPSink.hh>

/* ================================================================
 * H.264 video
 * ================================================================ */

RingH264Subsession *RingH264Subsession::createNew(UsageEnvironment &env,
						   rsd555_video_ctx_t *ctx,
						   Boolean reuseSource)
{
	return new RingH264Subsession(env, ctx, reuseSource);
}

RingH264Subsession::RingH264Subsession(UsageEnvironment &env, rsd555_video_ctx_t *ctx,
					Boolean reuseSource)
	: OnDemandServerMediaSubsession(env, reuseSource), fCtx(ctx)
{
}

RingH264Subsession::~RingH264Subsession() {}

FramedSource *RingH264Subsession::createNewStreamSource(unsigned /*clientSessionId*/,
							unsigned &estBitrate)
{
	estBitrate = 2000; /* kbps estimate for SDP b= line */
	RingVideoSource *src = RingVideoSource::createNew(envir(), fCtx);
	if (!src)
		return NULL;
	return H264VideoStreamDiscreteFramer::createNew(envir(), src);
}

RTPSink *RingH264Subsession::createNewRTPSink(Groupsock *rtpGroupsock,
					       unsigned char rtpPayloadTypeIfDynamic,
					       FramedSource * /*inputSource*/)
{
	/* Provide SPS/PPS for the SDP sprop-parameter-sets if available */
	uint16_t sps_len = __atomic_load_n(&fCtx->sps_len, __ATOMIC_ACQUIRE);
	uint16_t pps_len = __atomic_load_n(&fCtx->pps_len, __ATOMIC_ACQUIRE);

	return H264VideoRTPSink::createNew(envir(), rtpGroupsock, rtpPayloadTypeIfDynamic,
					   sps_len > 0 ? fCtx->sps : NULL, sps_len,
					   pps_len > 0 ? fCtx->pps : NULL, pps_len);
}

/* ================================================================
 * H.265 video
 * ================================================================ */

RingH265Subsession *RingH265Subsession::createNew(UsageEnvironment &env,
						   rsd555_video_ctx_t *ctx,
						   Boolean reuseSource)
{
	return new RingH265Subsession(env, ctx, reuseSource);
}

RingH265Subsession::RingH265Subsession(UsageEnvironment &env, rsd555_video_ctx_t *ctx,
					Boolean reuseSource)
	: OnDemandServerMediaSubsession(env, reuseSource), fCtx(ctx)
{
}

RingH265Subsession::~RingH265Subsession() {}

FramedSource *RingH265Subsession::createNewStreamSource(unsigned /*clientSessionId*/,
							unsigned &estBitrate)
{
	estBitrate = 2000;
	RingVideoSource *src = RingVideoSource::createNew(envir(), fCtx);
	if (!src)
		return NULL;
	return H265VideoStreamDiscreteFramer::createNew(envir(), src);
}

RTPSink *RingH265Subsession::createNewRTPSink(Groupsock *rtpGroupsock,
					       unsigned char rtpPayloadTypeIfDynamic,
					       FramedSource * /*inputSource*/)
{
	uint16_t vps_len = __atomic_load_n(&fCtx->vps_len, __ATOMIC_ACQUIRE);
	uint16_t sps_len = __atomic_load_n(&fCtx->sps_len, __ATOMIC_ACQUIRE);
	uint16_t pps_len = __atomic_load_n(&fCtx->pps_len, __ATOMIC_ACQUIRE);

	return H265VideoRTPSink::createNew(envir(), rtpGroupsock, rtpPayloadTypeIfDynamic,
					   vps_len > 0 ? fCtx->vps : NULL, vps_len,
					   sps_len > 0 ? fCtx->sps : NULL, sps_len,
					   pps_len > 0 ? fCtx->pps : NULL, pps_len);
}

/* ================================================================
 * AAC audio (RFC 3640, AAC-hbr mode)
 * ================================================================ */

RingAACSubsession *RingAACSubsession::createNew(UsageEnvironment &env, rsd555_audio_ctx_t *ctx,
						 Boolean reuseSource)
{
	return new RingAACSubsession(env, ctx, reuseSource);
}

RingAACSubsession::RingAACSubsession(UsageEnvironment &env, rsd555_audio_ctx_t *ctx,
				      Boolean reuseSource)
	: OnDemandServerMediaSubsession(env, reuseSource), fCtx(ctx)
{
}

RingAACSubsession::~RingAACSubsession() {}

FramedSource *RingAACSubsession::createNewStreamSource(unsigned /*clientSessionId*/,
						       unsigned &estBitrate)
{
	estBitrate = 64; /* kbps */
	return RingAudioSource::createNew(envir(), fCtx);
}

RTPSink *RingAACSubsession::createNewRTPSink(Groupsock *rtpGroupsock,
					      unsigned char rtpPayloadTypeIfDynamic,
					      FramedSource * /*inputSource*/)
{
	/* AudioSpecificConfig for AAC-LC mono:
	 * audioObjectType=2 (AAC-LC), channelConfig=1 */
	static const int sr_table[] = {96000, 88200, 64000, 48000, 44100,
				       32000, 24000, 22050, 16000, 12000,
				       11025, 8000,  7350};
	int sr_idx = -1;
	for (int i = 0; i < 13; i++) {
		if (sr_table[i] == (int)fCtx->sample_rate) {
			sr_idx = i;
			break;
		}
	}
	if (sr_idx < 0) {
		envir() << "RingAACSubsession: sample rate " << fCtx->sample_rate
			<< " not in MPEG-4 table, defaulting to 44100\n";
		sr_idx = 4;
	}

	unsigned char asc[2];
	asc[0] = (unsigned char)((2 << 3) | (sr_idx >> 1));
	asc[1] = (unsigned char)(((sr_idx & 1) << 7) | (1 << 3));

	char configStr[5];
	snprintf(configStr, sizeof(configStr), "%02X%02X", asc[0], asc[1]);

	return MPEG4GenericRTPSink::createNew(envir(), rtpGroupsock, rtpPayloadTypeIfDynamic,
					      fCtx->sample_rate, "audio", "AAC-hbr",
					      configStr, 1 /* numChannels */);
}

/* ================================================================
 * G.711 audio (u-law PT=0, A-law PT=8)
 * ================================================================ */

RingG711Subsession *RingG711Subsession::createNew(UsageEnvironment &env, rsd555_audio_ctx_t *ctx,
						  Boolean reuseSource, Boolean isAlaw)
{
	return new RingG711Subsession(env, ctx, reuseSource, isAlaw);
}

RingG711Subsession::RingG711Subsession(UsageEnvironment &env, rsd555_audio_ctx_t *ctx,
				       Boolean reuseSource, Boolean isAlaw)
	: OnDemandServerMediaSubsession(env, reuseSource), fCtx(ctx), fIsAlaw(isAlaw)
{
}

RingG711Subsession::~RingG711Subsession() {}

FramedSource *RingG711Subsession::createNewStreamSource(unsigned /*clientSessionId*/,
							unsigned &estBitrate)
{
	estBitrate = 64;
	return RingAudioSource::createNew(envir(), fCtx);
}

RTPSink *RingG711Subsession::createNewRTPSink(Groupsock *rtpGroupsock,
					      unsigned char /*rtpPayloadTypeIfDynamic*/,
					      FramedSource * /*inputSource*/)
{
	if (fIsAlaw)
		return SimpleRTPSink::createNew(envir(), rtpGroupsock, 8, 8000,
						"audio", "PCMA", 1, False);
	return SimpleRTPSink::createNew(envir(), rtpGroupsock, 0, 8000,
					"audio", "PCMU", 1, False);
}

/* ================================================================
 * L16 (raw 16-bit PCM) audio
 * ================================================================ */

RingL16Subsession *RingL16Subsession::createNew(UsageEnvironment &env, rsd555_audio_ctx_t *ctx,
						Boolean reuseSource)
{
	return new RingL16Subsession(env, ctx, reuseSource);
}

RingL16Subsession::RingL16Subsession(UsageEnvironment &env, rsd555_audio_ctx_t *ctx,
				     Boolean reuseSource)
	: OnDemandServerMediaSubsession(env, reuseSource), fCtx(ctx)
{
}

RingL16Subsession::~RingL16Subsession() {}

FramedSource *RingL16Subsession::createNewStreamSource(unsigned /*clientSessionId*/,
						       unsigned &estBitrate)
{
	estBitrate = fCtx->sample_rate * 16 / 1000; /* bits/s -> kbps */
	return RingAudioSource::createNew(envir(), fCtx);
}

RTPSink *RingL16Subsession::createNewRTPSink(Groupsock *rtpGroupsock,
					     unsigned char rtpPayloadTypeIfDynamic,
					     FramedSource * /*inputSource*/)
{
	return SimpleRTPSink::createNew(envir(), rtpGroupsock, rtpPayloadTypeIfDynamic,
					fCtx->sample_rate, "audio", "L16", 1, False);
}

/* ================================================================
 * Opus audio (RFC 7587)
 * ================================================================ */

RingOpusSubsession *RingOpusSubsession::createNew(UsageEnvironment &env, rsd555_audio_ctx_t *ctx,
						   Boolean reuseSource)
{
	return new RingOpusSubsession(env, ctx, reuseSource);
}

RingOpusSubsession::RingOpusSubsession(UsageEnvironment &env, rsd555_audio_ctx_t *ctx,
				       Boolean reuseSource)
	: OnDemandServerMediaSubsession(env, reuseSource), fCtx(ctx)
{
}

RingOpusSubsession::~RingOpusSubsession() {}

FramedSource *RingOpusSubsession::createNewStreamSource(unsigned /*clientSessionId*/,
							unsigned &estBitrate)
{
	estBitrate = 64;
	return RingAudioSource::createNew(envir(), fCtx);
}

RTPSink *RingOpusSubsession::createNewRTPSink(Groupsock *rtpGroupsock,
					      unsigned char rtpPayloadTypeIfDynamic,
					      FramedSource * /*inputSource*/)
{
	/* RFC 7587: always 48000/2 in SDP regardless of actual rate.
	 * SimpleRTPSink is correct for Opus: each RTP packet contains one
	 * self-delimiting Opus frame (RFC 7587 Section 4.2). Marker bit is
	 * set per-packet which is fine for continuous camera audio. */
	return SimpleRTPSink::createNew(envir(), rtpGroupsock, rtpPayloadTypeIfDynamic,
					48000, "audio", "opus", 2, False);
}
