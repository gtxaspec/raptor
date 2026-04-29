/*
 * rsd555_backchannel.cpp -- ONVIF backchannel implementation
 *
 * Subclasses ServerMediaSubsession directly to handle the inverted
 * direction (receive RTP from client). Creates SimpleRTPSource +
 * BackchannelSink per client session. Decoded PCMU audio is
 * upsampled 8kHz→16kHz and published to the "speaker" ring.
 */

#include "rsd555_backchannel.h"

#include <string.h>
#include <unistd.h>

/* ================================================================
 * PCMU decoder (ITU-T G.711 mu-law)
 * ================================================================ */

static int16_t ulaw_decode(uint8_t ulaw)
{
	ulaw = ~ulaw;
	int sign = (ulaw & 0x80);
	int exponent = (ulaw >> 4) & 0x07;
	int mantissa = ulaw & 0x0f;
	int magnitude = ((mantissa << 3) + 0x84) << exponent;
	magnitude -= 0x84;
	return (int16_t)(sign ? -magnitude : magnitude);
}

/* ================================================================
 * BackchannelSink — receives audio, publishes to speaker ring
 * ================================================================ */

BackchannelSink *BackchannelSink::createNew(UsageEnvironment &env)
{
	return new BackchannelSink(env);
}

BackchannelSink::BackchannelSink(UsageEnvironment &env)
	: MediaSink(env), fSpeakerRing(NULL)
{
}

BackchannelSink::~BackchannelSink()
{
	if (fSpeakerRing)
		rss_ring_destroy(fSpeakerRing);
}

Boolean BackchannelSink::continuePlaying()
{
	if (!fSource)
		return False;
	fSource->getNextFrame(fReceiveBuffer, sizeof(fReceiveBuffer),
			      afterGettingFrame, this, onSourceClosure, this);
	return True;
}

void BackchannelSink::afterGettingFrame(void *clientData, unsigned frameSize,
					unsigned /*numTruncatedBytes*/,
					struct timeval /*presentationTime*/,
					unsigned /*durationInMicroseconds*/)
{
	BackchannelSink *sink = (BackchannelSink *)clientData;
	sink->processFrame(frameSize);
	sink->continuePlaying();
}

void BackchannelSink::processFrame(unsigned frameSize)
{
	if (!fSpeakerRing) {
		fSpeakerRing = rss_ring_open("speaker");
		if (!fSpeakerRing)
			fSpeakerRing = rss_ring_create("speaker", 16, 64 * 1024);
		if (!fSpeakerRing)
			return;
		rss_ring_set_stream_info(fSpeakerRing, 0x11, 0, 0, 0, 16000, 1, 0, 0);
		RSS_INFO("backchannel: speaker ring ready");
	}

	int n = (int)frameSize;
	if (n > 480)
		n = 480;
	int16_t pcm[960];
	for (int i = 0; i < n; i++) {
		int16_t s = ulaw_decode(fReceiveBuffer[i]);
		pcm[i * 2] = s;
		pcm[i * 2 + 1] = s;
	}
	rss_ring_publish(fSpeakerRing, (const uint8_t *)pcm, n * 4,
			 rss_timestamp_us(), 0, 0);
}

/* ================================================================
 * BackchannelStreamState — per-client cleanup
 * ================================================================ */

BackchannelStreamState::BackchannelStreamState()
	: rtpSource(NULL), sink(NULL), rtcpInstance(NULL),
	  rtpGroupsock(NULL), rtcpGroupsock(NULL),
	  tcpSocketNum(-1), rtpChannelId(0), rtcpChannelId(0),
	  tlsState(NULL)
{
	gethostname(cname, sizeof(cname));
	cname[sizeof(cname) - 1] = '\0';
}

BackchannelStreamState::~BackchannelStreamState()
{
	if (rtcpInstance) {
		RTCPInstance::close(rtcpInstance);
		rtcpInstance = NULL;
	}
	if (sink) {
		sink->stopPlaying();
		Medium::close(sink);
		sink = NULL;
	}
	if (rtpSource) {
		Medium::close(rtpSource);
		rtpSource = NULL;
	}
	delete rtpGroupsock;
	if (rtcpGroupsock && rtcpGroupsock != rtpGroupsock)
		delete rtcpGroupsock;
	rtpGroupsock = NULL;
	rtcpGroupsock = NULL;
}

/* ================================================================
 * BackchannelSubsession — direct ServerMediaSubsession subclass
 * ================================================================ */

BackchannelSubsession *BackchannelSubsession::createNew(UsageEnvironment &env)
{
	return new BackchannelSubsession(env);
}

BackchannelSubsession::BackchannelSubsession(UsageEnvironment &env)
	: ServerMediaSubsession(env), fSDPLines(NULL)
{
}

BackchannelSubsession::~BackchannelSubsession()
{
	delete[] fSDPLines;
}

char const *BackchannelSubsession::sdpLines(int /*addressFamily*/)
{
	if (fSDPLines)
		return fSDPLines;

	char buf[256];
	snprintf(buf, sizeof(buf),
		 "m=audio 0 RTP/AVP 0\r\n"
		 "c=IN IP4 0.0.0.0\r\n"
		 "b=AS:64\r\n"
		 "a=rtpmap:0 PCMU/8000\r\n"
		 "a=control:%s\r\n"
		 "a=sendonly\r\n",
		 trackId());

	fSDPLines = new char[strlen(buf) + 1];
	if (fSDPLines)
		strcpy(fSDPLines, buf);
	return fSDPLines;
}

void BackchannelSubsession::getStreamParameters(
	unsigned clientSessionId,
	struct sockaddr_storage const &clientAddress,
	Port const & /*clientRTPPort*/, Port const & /*clientRTCPPort*/,
	int tcpSocketNum, unsigned char rtpChannelId,
	unsigned char rtcpChannelId, TLSState *tlsState,
	struct sockaddr_storage &destinationAddress,
	u_int8_t & /*destinationTTL*/, Boolean &isMulticast,
	Port &serverRTPPort, Port &serverRTCPPort,
	void *&streamToken)
{
	isMulticast = False;
	streamToken = NULL;

	if (addressIsNull(destinationAddress))
		destinationAddress = clientAddress;

	BackchannelStreamState *state = new BackchannelStreamState();
	if (!state)
		return;
	state->tcpSocketNum = tcpSocketNum;
	state->rtpChannelId = rtpChannelId;
	state->rtcpChannelId = rtcpChannelId;
	state->tlsState = tlsState;

	/* Dummy groupsocks — actual transport is TCP interleaved */
	struct sockaddr_storage nullAddr;
	memset(&nullAddr, 0, sizeof(nullAddr));
	nullAddr.ss_family = AF_INET;
	Port dummyPort(0);

	serverRTPPort = 0;
	serverRTCPPort = 0;
	state->rtpGroupsock = new Groupsock(envir(), nullAddr, dummyPort, 0);
	state->rtcpGroupsock = new Groupsock(envir(), nullAddr, dummyPort, 0);

	/* RTP source — receives PCMU from client */
	state->rtpSource = SimpleRTPSource::createNew(
		envir(), state->rtpGroupsock, 0, 8000,
		"audio/PCMU", 0, False);
	if (!state->rtpSource) {
		delete state;
		return;
	}

	/* Sink — decodes and publishes to speaker ring */
	state->sink = BackchannelSink::createNew(envir());
	if (!state->sink) {
		Medium::close(state->rtpSource);
		state->rtpSource = NULL;
		delete state;
		return;
	}

	/* RTCP for receiver reports */
	state->rtcpInstance = RTCPInstance::createNew(
		envir(), state->rtcpGroupsock, 64,
		(unsigned char *)state->cname,
		NULL, state->rtpSource, False);

	streamToken = (void *)state;
	RSS_DEBUG("backchannel SETUP: session=%u tcp=%d ch=%u/%u",
		  clientSessionId, tcpSocketNum, rtpChannelId, rtcpChannelId);
}

void BackchannelSubsession::startStream(
	unsigned /*clientSessionId*/, void *streamToken,
	TaskFunc * /*rtcpRRHandler*/, void * /*rtcpRRHandlerClientData*/,
	unsigned short &rtpSeqNum, unsigned &rtpTimestamp,
	ServerRequestAlternativeByteHandler *handler,
	void *handlerClientData)
{
	BackchannelStreamState *state = (BackchannelStreamState *)streamToken;
	if (!state || !state->sink || !state->rtpSource)
		return;

	rtpSeqNum = 0;
	rtpTimestamp = 0;

	/* Wire up TCP interleaved receive */
	if (state->tcpSocketNum >= 0) {
		state->rtpSource->setStreamSocket(
			state->tcpSocketNum, state->rtpChannelId,
			state->tlsState);
		if (state->rtcpInstance)
			state->rtcpInstance->addStreamSocket(
				state->tcpSocketNum, state->rtcpChannelId,
				state->tlsState);
		if (handler) {
			RTPInterface::setServerRequestAlternativeByteHandler(
				envir(), state->tcpSocketNum,
				handler, handlerClientData);
		}
		RSS_DEBUG("backchannel: tcp=%d rtp_ch=%u rtcp_ch=%u handler=%p",
			  state->tcpSocketNum, state->rtpChannelId,
			  state->rtcpChannelId, (void *)handler);
	}

	/* Start receiving audio */
	if (!state->sink->startPlaying(*state->rtpSource, NULL, NULL))
		RSS_WARN("backchannel: startPlaying failed");
	else
		RSS_INFO("backchannel: streaming started");
}

void BackchannelSubsession::deleteStream(unsigned /*clientSessionId*/,
					  void *&streamToken)
{
	BackchannelStreamState *state = (BackchannelStreamState *)streamToken;
	delete state;
	streamToken = NULL;
}

void BackchannelSubsession::pauseStream(unsigned /*clientSessionId*/,
					 void * /*streamToken*/)
{
}

void BackchannelSubsession::getRTPSinkandRTCP(void * /*streamToken*/,
					       RTPSink *&rtpSink,
					       RTCPInstance *&rtcp)
{
	rtpSink = NULL;
	rtcp = NULL;
}

FramedSource *BackchannelSubsession::getStreamSource(void * /*streamToken*/)
{
	return NULL;
}
