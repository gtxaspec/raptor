/*
 * rsd555_backchannel.h -- ONVIF backchannel (two-way audio) subsession
 *
 * Subclasses ServerMediaSubsession directly (not OnDemandServerMediaSubsession)
 * because the inverted direction (receive from client, not send) is incompatible
 * with OnDemand's assumption of outgoing streams.
 */

#ifndef RSD555_BACKCHANNEL_H
#define RSD555_BACKCHANNEL_H

#include <ServerMediaSession.hh>
#include <SimpleRTPSource.hh>
#include <MediaSink.hh>
#include <Groupsock.hh>
#include <GroupsockHelper.hh>
#include <RTCP.hh>
#include <RTPInterface.hh>

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

/* Per-client backchannel stream state */
struct BackchannelStreamState {
	RTPSource *rtpSource;
	BackchannelSink *sink;
	RTCPInstance *rtcpInstance;
	Groupsock *rtpGroupsock;
	Groupsock *rtcpGroupsock;
	int tcpSocketNum;
	unsigned char rtpChannelId;
	unsigned char rtcpChannelId;
	TLSState *tlsState;
	char cname[100];

	BackchannelStreamState();
	~BackchannelStreamState();
};

/* ServerMediaSubsession with inverted direction — receives RTP from
 * client instead of sending. Bypasses OnDemandServerMediaSubsession
 * entirely. */
class BackchannelSubsession : public ServerMediaSubsession {
public:
	static BackchannelSubsession *createNew(UsageEnvironment &env);

protected:
	BackchannelSubsession(UsageEnvironment &env);
	virtual ~BackchannelSubsession();

	virtual char const *sdpLines(int addressFamily);
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
	virtual void getRTPSinkandRTCP(void *streamToken, RTPSink *&rtpSink,
				       RTCPInstance *&rtcp);
	virtual FramedSource *getStreamSource(void *streamToken);

private:
	char *fSDPLines;
};

#endif /* RSD555_BACKCHANNEL_H */
