/*
 * rsd555_backchannel.cpp -- ONVIF backchannel implementation
 *
 * Receives PCMU/8000 audio from the RTSP client via RTP, decodes
 * to PCM16, upsamples 8kHz→16kHz (duplicate each sample), and
 * publishes to the "speaker" ring. RAD's AO playback thread reads
 * the ring and sends to the hardware speaker.
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
	}

	/* PCMU/8000 → PCM16/16kHz: decode + 2x upsample (duplicate samples) */
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
	if (rtcpGroupsock != rtpGroupsock)
		delete rtcpGroupsock;
	rtpGroupsock = NULL;
	rtcpGroupsock = NULL;
}

/* ================================================================
 * BackchannelSubsession — inverted-direction ServerMediaSubsession
 * ================================================================ */

BackchannelSubsession *BackchannelSubsession::createNew(UsageEnvironment &env)
{
	return new BackchannelSubsession(env);
}

BackchannelSubsession::BackchannelSubsession(UsageEnvironment &env)
	: OnDemandServerMediaSubsession(env, False), fSDPLines(NULL)
{
	gethostname(fCNAME, sizeof(fCNAME));
	fCNAME[sizeof(fCNAME) - 1] = '\0';
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

/* Not used — backchannel receives, doesn't source */
FramedSource *BackchannelSubsession::createNewStreamSource(
	unsigned /*clientSessionId*/, unsigned & /*estBitrate*/)
{
	return NULL;
}

RTPSink *BackchannelSubsession::createNewRTPSink(
	Groupsock * /*rtpGroupsock*/,
	unsigned char /*rtpPayloadTypeIfDynamic*/,
	FramedSource * /*inputSource*/)
{
	return NULL;
}

void BackchannelSubsession::getStreamParameters(
	unsigned clientSessionId,
	struct sockaddr_storage const &clientAddress,
	Port const & /*clientRTPPort*/, Port const & /*clientRTCPPort*/,
	int tcpSocketNum, unsigned char /*rtpChannelId*/,
	unsigned char /*rtcpChannelId*/, TLSState * /*tlsState*/,
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
	state->clientSessionId = clientSessionId;

	/* Create groupsocks */
	struct sockaddr_storage nullAddr;
	memset(&nullAddr, 0, sizeof(nullAddr));
	nullAddr.ss_family = AF_INET;
	Port dummyPort(0);

	if (tcpSocketNum >= 0) {
		/* TCP interleaved */
		serverRTPPort = 0;
		serverRTCPPort = 0;
		state->rtpGroupsock = new Groupsock(envir(), nullAddr, dummyPort, 0);
		state->rtcpGroupsock = new Groupsock(envir(), nullAddr, dummyPort, 0);
	} else {
		/* UDP — allocate server-side ports */
		portNumBits portNum = 6970;
		while (true) {
			serverRTPPort = portNum;
			state->rtpGroupsock = new Groupsock(envir(), nullAddr, serverRTPPort, 0);
			if (state->rtpGroupsock->socketNum() >= 0)
				break;
			delete state->rtpGroupsock;
			portNum += 2;
			if (portNum > 65534) {
				delete state;
				return;
			}
		}
		serverRTCPPort = portNum + 1;
		state->rtcpGroupsock = new Groupsock(envir(), nullAddr, serverRTCPPort, 0);
		if (state->rtcpGroupsock->socketNum() < 0) {
			delete state->rtpGroupsock;
			delete state->rtcpGroupsock;
			state->rtpGroupsock = NULL;
			state->rtcpGroupsock = NULL;
			delete state;
			return;
		}
	}

	/* Create RTP source (receives from client) */
	state->rtpSource = SimpleRTPSource::createNew(envir(), state->rtpGroupsock,
						      0, 8000, "audio/PCMU", 0, False);
	if (!state->rtpSource) {
		delete state;
		return;
	}

	/* Create sink (decodes and publishes to speaker ring) */
	state->sink = BackchannelSink::createNew(envir());
	if (!state->sink) {
		Medium::close(state->rtpSource);
		state->rtpSource = NULL;
		delete state;
		return;
	}

	/* RTCP instance for receiver reports */
	unsigned totalBW = 64;
	state->rtcpInstance = RTCPInstance::createNew(
		envir(), state->rtcpGroupsock, totalBW, (unsigned char *)fCNAME,
		NULL, state->rtpSource, False);

	streamToken = (void *)state;
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

	/* Set up TCP interleaved receive if needed */
	if (handler)
		RTPInterface::setServerRequestAlternativeByteHandler(
			envir(), state->rtpSource->RTPgs()->socketNum(),
			handler, handlerClientData);

	/* Start receiving */
	if (!state->sink->startPlaying(*state->rtpSource, NULL, NULL))
		envir() << "BackchannelSubsession: startPlaying failed\n";

	rtpSeqNum = 0;
	rtpTimestamp = 0;
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
