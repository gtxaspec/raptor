/*
 * rsd555_backchannel.h -- ONVIF backchannel (two-way audio) subsession
 *
 * Receives PCMU/8000 audio from the RTSP client, decodes to PCM16,
 * upsamples 8kHz→16kHz, and publishes to the speaker ring.
 */

#ifndef RSD555_BACKCHANNEL_H
#define RSD555_BACKCHANNEL_H

#include <OnDemandServerMediaSubsession.hh>
#include <SimpleRTPSource.hh>
#include <MediaSink.hh>
#include <Groupsock.hh>
#include <GroupsockHelper.hh>

extern "C" {
#include "rsd555.h"
}

/* MediaSink that receives decoded PCMU audio and publishes to the
 * speaker ring as PCM16 upsampled from 8kHz to 16kHz. */
class BackchannelSink : public MediaSink {
public:
	static BackchannelSink *createNew(UsageEnvironment &env);

protected:
	BackchannelSink(UsageEnvironment &env);
	virtual ~BackchannelSink();
	virtual Boolean continuePlaying();

private:
	static void afterGettingFrame(void *clientData, unsigned frameSize,
				      unsigned numTruncatedBytes,
				      struct timeval presentationTime,
				      unsigned durationInMicroseconds);
	void processFrame(unsigned frameSize);

	rss_ring_t *fSpeakerRing;
	uint8_t fReceiveBuffer[1024];
};

/* ServerMediaSubsession with inverted direction — receives RTP from
 * client instead of sending. Gated on ONVIF backchannel Require header. */
class BackchannelSubsession : public OnDemandServerMediaSubsession {
public:
	static BackchannelSubsession *createNew(UsageEnvironment &env);

protected:
	BackchannelSubsession(UsageEnvironment &env);
	virtual ~BackchannelSubsession();

	virtual char const *sdpLines(int addressFamily);
	virtual FramedSource *createNewStreamSource(unsigned clientSessionId,
						    unsigned &estBitrate);
	virtual RTPSink *createNewRTPSink(Groupsock *rtpGroupsock,
					  unsigned char rtpPayloadTypeIfDynamic,
					  FramedSource *inputSource);
	virtual void getStreamParameters(
		unsigned clientSessionId,
		struct sockaddr_storage const &clientAddress,
		Port const &clientRTPPort, Port const &clientRTCPPort,
		int tcpSocketNum, unsigned char rtpChannelId,
		unsigned char rtcpChannelId, TLSState *tlsState,
		struct sockaddr_storage &destinationAddress,
		u_int8_t &destinationTTL, Boolean &isMulticast,
		Port &serverRTPPort, Port &serverRTCPPort,
		void *&streamToken);
	virtual void startStream(unsigned clientSessionId, void *streamToken,
				 TaskFunc *rtcpRRHandler,
				 void *rtcpRRHandlerClientData,
				 unsigned short &rtpSeqNum,
				 unsigned &rtpTimestamp,
				 ServerRequestAlternativeByteHandler *handler,
				 void *handlerClientData);
	virtual void deleteStream(unsigned clientSessionId, void *&streamToken);
	virtual void pauseStream(unsigned clientSessionId, void *streamToken);

private:
	char *fSDPLines;
	char fCNAME[100];
};

/* Per-client backchannel stream state */
struct BackchannelStreamState {
	RTPSource *rtpSource;
	BackchannelSink *sink;
	Groupsock *rtpGroupsock;
	Groupsock *rtcpGroupsock;
	RTCPInstance *rtcpInstance;
	unsigned clientSessionId;

	BackchannelStreamState()
		: rtpSource(NULL), sink(NULL), rtpGroupsock(NULL),
		  rtcpGroupsock(NULL), rtcpInstance(NULL), clientSessionId(0) {}
	~BackchannelStreamState();
};

#endif /* RSD555_BACKCHANNEL_H */
