/*
 * rsd555_main.cpp -- Raptor live555 RTSP Streaming Daemon
 *
 * Standalone RTSP server using live555, consuming SHM ring buffers
 * produced by RVD/RAD. No HAL dependency.
 */

#include <BasicUsageEnvironment.hh>
#include <RTSPServer.hh>
#include <ServerMediaSession.hh>
#include <GroupsockHelper.hh>
#include <RTPSink.hh>

#include "rsd555_subsession.h"
#include "rsd555_source.h"
#include "rsd555_backchannel.h"

#include "rsd555.h"
#include <string.h>
#include <unistd.h>

static rsd555_state_t g_state;

/* live555 watch variable — set by signal handler via running flag */
static char g_watch = 0;

static UsageEnvironment *g_env;

static RTSPServer *g_rtspServer;

static ServerMediaSession *create_stream_session(UsageEnvironment &env,
						 const char *name,
						 const char *info,
						 rsd555_video_ctx_t *vctx,
						 rsd555_audio_ctx_t *actx,
						 bool has_audio,
						 bool backchannel);

/* Periodic check: rebuild sessions when readers discover new streams */
static void check_streams(void * /*clientData*/)
{
	if (!rss_running(g_state.running)) {
		g_watch = 1;
		return;
	}

	for (int s = 0; s < 2; s++) {
		bool need_rebuild = false;

		if (g_state.video[s].width > 0 && !g_state.video_added[s])
			need_rebuild = true;
		if (g_state.audio.sample_rate > 0 && !g_state.audio_added &&
		    g_state.stream_names[s][0])
			need_rebuild = true;

		if (need_rebuild && g_rtspServer && g_state.stream_names[s][0]) {
			ServerMediaSession *sms = create_stream_session(
				*g_env, g_state.stream_names[s],
				g_state.session_name,
				&g_state.video[s], &g_state.audio,
				g_state.audio.sample_rate > 0,
				g_state.backchannel);
			if (sms) {
				g_rtspServer->deleteServerMediaSession(
					g_state.stream_names[s]);
				g_rtspServer->addServerMediaSession(sms);
				if (g_state.video[s].width > 0)
					g_state.video_added[s] = true;
				if (g_state.audio.sample_rate > 0)
					g_state.audio_added = true;
				char *url = g_rtspServer->rtspURL(sms);
				RSS_INFO("stream %d rebuilt: %s", s, url);
				delete[] url;
			}
		}
	}

	g_env->taskScheduler().scheduleDelayedTask(500000, check_streams, NULL);
}

static ServerMediaSession *create_stream_session(UsageEnvironment &env,
						 const char *name,
						 const char *info,
						 rsd555_video_ctx_t *vctx,
						 rsd555_audio_ctx_t *actx,
						 bool has_audio,
						 bool backchannel)
{
	ServerMediaSession *sms = ServerMediaSession::createNew(
		env, name, info, "Raptor RSS (rsd-555)");
	if (!sms)
		return NULL;

	if (vctx && vctx->width > 0) {
		if (vctx->codec == 1)
			sms->addSubsession(RingH265Subsession::createNew(env, vctx, False));
		else
			sms->addSubsession(RingH264Subsession::createNew(env, vctx, False));
	}

	if (has_audio && actx && actx->sample_rate > 0) {
		switch (actx->codec) {
		case RSD555_CODEC_AAC:
			sms->addSubsession(RingAACSubsession::createNew(env, actx, False));
			break;
		case RSD555_CODEC_PCMU:
			sms->addSubsession(RingG711Subsession::createNew(env, actx, False, False));
			break;
		case RSD555_CODEC_PCMA:
			sms->addSubsession(RingG711Subsession::createNew(env, actx, False, True));
			break;
		case RSD555_CODEC_OPUS:
			sms->addSubsession(RingOpusSubsession::createNew(env, actx, False));
			break;
		case RSD555_CODEC_L16:
		default:
			sms->addSubsession(RingL16Subsession::createNew(env, actx, False));
			break;
		}
	}

	/* Backchannel: sendonly PCMU/8000 audio track for two-way audio.
	 * Requires live555 patch 0011 (ONVIF Require header support).
	 * setRequireTag gates SDP inclusion on the client's Require header. */
	if (backchannel) {
		BackchannelSubsession *bc = BackchannelSubsession::createNew(env);
		bc->setRequireTag("www.onvif.org/ver20/backchannel");
		sms->addSubsession(bc);
	}

	return sms;
}

/* Control socket integration: handle one command when the fd is readable */
static void ctrl_socket_handler(void *clientData, int /*mask*/)
{
	rsd555_state_t *st = (rsd555_state_t *)clientData;
	if (st->ctrl)
		rss_ctrl_accept_and_handle(st->ctrl, rsd555_ctrl_handler, st);
}

int main(int argc, char **argv)
{
	rss_daemon_ctx_t dctx;
	int ret = rss_daemon_init(&dctx, "rsd-555", argc, argv, "live555");
	if (ret != 0)
		return ret < 0 ? 1 : 0;

	g_state.cfg = dctx.cfg;
	g_state.config_path = dctx.config_path;
	g_state.running = dctx.running;

	if (!rss_config_get_bool(dctx.cfg, "rtsp", "enabled", true)) {
		RSS_INFO("RTSP disabled in config");
		rss_config_free(dctx.cfg);
		rss_daemon_cleanup("rsd-555");
		return 0;
	}

	g_state.port = rss_config_get_int(dctx.cfg, "rtsp-555", "port",
					rss_config_get_int(dctx.cfg, "rtsp", "port", 554));
	if (g_state.port < 1 || g_state.port > 65535) {
		RSS_WARN("invalid port %d, using 554", g_state.port);
		g_state.port = 554;
	}
	g_state.max_clients = rss_config_get_int(dctx.cfg, "rtsp", "max_clients", RSD555_MAX_CLIENTS);
	if (g_state.max_clients < 1)
		g_state.max_clients = 1;
	if (g_state.max_clients > RSD555_MAX_CLIENTS)
		g_state.max_clients = RSD555_MAX_CLIENTS;
	g_state.session_timeout = rss_config_get_int(dctx.cfg, "rtsp", "session_timeout", 60);
	if (g_state.session_timeout < 10)
		g_state.session_timeout = 10;

	const char *user = rss_config_get_str(dctx.cfg, "rtsp", "username", "");
	const char *pass = rss_config_get_str(dctx.cfg, "rtsp", "password", "");
	rss_strlcpy(g_state.username, user, sizeof(g_state.username));
	rss_strlcpy(g_state.password, pass, sizeof(g_state.password));
	rss_strlcpy(g_state.session_name,
		    rss_config_get_str(dctx.cfg, "rtsp", "session_name", "Raptor Live"),
		    sizeof(g_state.session_name));
	rss_strlcpy(g_state.session_info,
		    rss_config_get_str(dctx.cfg, "rtsp", "session_info", ""),
		    sizeof(g_state.session_info));

	g_state.backchannel = rss_config_get_bool(dctx.cfg, "rtsp", "backchannel", false);

	/* Load endpoint aliases (same config keys as RSD) */
	static const char *const endpoint_keys[RSD555_STREAM_COUNT] = {
		"endpoint_main", "endpoint_sub", "endpoint_s1_main",
		"endpoint_s1_sub", "endpoint_s2_main", "endpoint_s2_sub",
	};
	for (int s = 0; s < RSD555_STREAM_COUNT; s++) {
		rss_strlcpy(g_state.endpoints[s],
			    rss_config_get_str(dctx.cfg, "rtsp", endpoint_keys[s], ""),
			    sizeof(g_state.endpoints[s]));
	}

	/* Probe rings for stream info, then close — reader threads re-open
	 * and handle buffer allocation + reconnection properly. */
	for (int s = 0; s < RSD555_STREAM_COUNT; s++) {
		g_state.video[s].idx = s;
		g_state.video[s].ring_name = rsd555_ring_names[s];
		g_state.video[s].running = g_state.running;
		g_state.video[s].state = &g_state;
		pthread_mutex_init(&g_state.video[s].sources_lock, NULL);

		rss_ring_t *probe = rss_ring_open(rsd555_ring_names[s]);
		if (probe) {
			const rss_ring_header_t *hdr = rss_ring_get_header(probe);
			g_state.video[s].codec = hdr->codec;
			g_state.video[s].width = hdr->width;
			g_state.video[s].height = hdr->height;
			g_state.video[s].fps_num = hdr->fps_num;
			g_state.video[s].fps_den = hdr->fps_den;
			g_state.video[s].profile = hdr->profile;
			g_state.video[s].level = hdr->level;
			rss_ring_close(probe);
			RSS_INFO("stream %d (%s): %s %ux%u", s, rsd555_ring_names[s],
				 g_state.video[s].codec == 0 ? "H.264" : "H.265",
				 g_state.video[s].width, g_state.video[s].height);
		}
	}

	/* Brief wait for main ring — covers init script race where RVD
	 * is still starting. 2s max, exit early on signal or success. */
	if (g_state.video[0].width == 0) {
		RSS_INFO("waiting for main video ring...");
		for (int i = 0; i < 20 && rss_running(g_state.running); i++) {
			usleep(100000);
			rss_ring_t *probe = rss_ring_open("main");
			if (probe) {
				const rss_ring_header_t *hdr = rss_ring_get_header(probe);
				g_state.video[0].codec = hdr->codec;
				g_state.video[0].width = hdr->width;
				g_state.video[0].height = hdr->height;
				g_state.video[0].fps_num = hdr->fps_num;
				g_state.video[0].fps_den = hdr->fps_den;
				g_state.video[0].profile = hdr->profile;
				g_state.video[0].level = hdr->level;
				rss_ring_close(probe);
				RSS_INFO("stream 0 (main): %s %ux%u",
					 g_state.video[0].codec == 0 ? "H.264" : "H.265",
					 g_state.video[0].width, g_state.video[0].height);
				break;
			}
		}
		/* Also retry sub during the window */
		if (g_state.video[1].width == 0) {
			rss_ring_t *probe = rss_ring_open("sub");
			if (probe) {
				const rss_ring_header_t *hdr = rss_ring_get_header(probe);
				g_state.video[1].codec = hdr->codec;
				g_state.video[1].width = hdr->width;
				g_state.video[1].height = hdr->height;
				g_state.video[1].fps_num = hdr->fps_num;
				g_state.video[1].fps_den = hdr->fps_den;
				g_state.video[1].profile = hdr->profile;
				g_state.video[1].level = hdr->level;
				rss_ring_close(probe);
				RSS_INFO("stream 1 (sub): %s %ux%u",
					 g_state.video[1].codec == 0 ? "H.264" : "H.265",
					 g_state.video[1].width, g_state.video[1].height);
			}
		}
		if (g_state.video[0].width == 0)
			RSS_WARN("main video ring not available (is RVD running?)");
	}

	/* Audio ring — probe and close */
	g_state.audio.running = g_state.running;
	g_state.audio.state = &g_state;
	pthread_mutex_init(&g_state.audio.sources_lock, NULL);
	{
		rss_ring_t *probe = rss_ring_open("audio");
		if (probe) {
			const rss_ring_header_t *ahdr = rss_ring_get_header(probe);
			g_state.audio.codec = ahdr->codec;
			g_state.audio.sample_rate = ahdr->fps_num;
			g_state.has_audio = true;
			rss_ring_close(probe);
			RSS_INFO("audio ring: codec=%u rate=%u", g_state.audio.codec, g_state.audio.sample_rate);
		}
	}

	/* Control socket */
	rss_mkdir_p(RSS_RUN_DIR);
	g_state.ctrl = rss_ctrl_listen(RSS_RUN_DIR "/rsd-555.sock");

	/* Start reader threads for main+sub always (retry ring_open every
	 * 200ms in background — DEBUG level, silent in production). Other
	 * streams only if detected at probe. Handles RVD restart. */
	for (int s = 0; s < RSD555_STREAM_COUNT; s++) {
		if (s >= 2 && g_state.video[s].width == 0)
			continue;
		pthread_attr_t attr;
		pthread_attr_init(&attr);
		pthread_attr_setstacksize(&attr, 128 * 1024);
		if (pthread_create(&g_state.video_tids[s], &attr,
				   rsd555_video_reader_thread, &g_state.video[s]) == 0)
			g_state.video_started[s] = true;
		pthread_attr_destroy(&attr);
	}
	{
		pthread_attr_t attr;
		pthread_attr_init(&attr);
		pthread_attr_setstacksize(&attr, 128 * 1024);
		if (pthread_create(&g_state.audio_tid, &attr,
				   rsd555_audio_reader_thread, &g_state.audio) == 0)
			g_state.audio_started = true;
		pthread_attr_destroy(&attr);
	}

	/* live555 setup */
	OutPacketBuffer::maxSize = rss_config_get_int(dctx.cfg, "rtsp",
						      "out_buffer_size", 256 * 1024);
	TaskScheduler *scheduler = BasicTaskScheduler::createNew();
	UsageEnvironment *env = BasicUsageEnvironment::createNew(*scheduler);
	g_env = env;

	UserAuthenticationDatabase *authDB = NULL;
	RTSPServer *rtspServer = NULL;
	g_rtspServer = NULL;

	if (g_state.username[0] && g_state.password[0]) {
		authDB = new UserAuthenticationDatabase;
		authDB->addUserRecord(g_state.username, g_state.password);
		RSS_INFO("RTSP Digest auth enabled");
	}

	rtspServer = RTSPServer::createNew(*env, (Port)g_state.port, authDB,
					   g_state.session_timeout);
	if (!rtspServer) {
		RSS_FATAL("failed to create RTSP server on port %d: %s",
			  g_state.port, env->getResultMsg());
		goto cleanup;
	}
	g_rtspServer = rtspServer;
	RSS_DEBUG("RTSPServer created on port %d", g_state.port);

	/* Create endpoints for main+sub always, others only if detected.
	 * Video subsession requires width > 0 (live555 SDP is immutable).
	 * Audio subsession requires sample_rate > 0.
	 * Store stream names so check_streams can rebuild sessions later
	 * when readers discover rings that weren't available at startup. */
	for (int s = 0; s < RSD555_STREAM_COUNT; s++) {
		if (s >= 2 && g_state.video[s].width == 0)
			continue;

		const char *ep = g_state.endpoints[s];
		if (ep[0])
			rss_strlcpy(g_state.stream_names[s], ep, sizeof(g_state.stream_names[s]));
		else
			snprintf(g_state.stream_names[s], sizeof(g_state.stream_names[s]),
				 "stream%d", s);

		ServerMediaSession *sms = create_stream_session(
			*env, g_state.stream_names[s], g_state.session_name,
			&g_state.video[s], &g_state.audio, g_state.has_audio,
			g_state.backchannel);
		if (!sms)
			continue;
		rtspServer->addServerMediaSession(sms);

		g_state.video_added[s] = (g_state.video[s].width > 0);
		g_state.audio_added = (g_state.has_audio && g_state.audio.sample_rate > 0);

		char *url = rtspServer->rtspURL(sms);
		RSS_INFO("stream %d: %s", s, url);
		delete[] url;
	}

	/* Register control socket with live555 scheduler */
	if (g_state.ctrl) {
		int ctrl_fd = rss_ctrl_get_fd(g_state.ctrl);
		if (ctrl_fd >= 0)
			scheduler->turnOnBackgroundReadHandling(ctrl_fd, ctrl_socket_handler,
								&g_state);
	}

	/* Schedule periodic check: running flag + stream discovery */
	env->taskScheduler().scheduleDelayedTask(500000, check_streams, NULL);

	RSS_INFO("rsd-555 listening on port %d", g_state.port);

	/* Run live555 event loop */
	env->taskScheduler().doEventLoop(&g_watch);

	RSS_INFO("rsd-555 shutting down");

cleanup:
	/* Close RTSP server first — destroys all sessions and sources,
	 * which unregisters from the fan-out (snapshot lock is fast now). */
	if (rtspServer)
		Medium::close(rtspServer);

	/* Join reader threads — they'll exit within 100ms (ring_wait timeout) */
	for (int s = 0; s < RSD555_STREAM_COUNT; s++) {
		if (g_state.video_started[s])
			pthread_join(g_state.video_tids[s], NULL);
	}
	if (g_state.audio_started)
		pthread_join(g_state.audio_tid, NULL);

	env->reclaim();
	delete scheduler;
	delete authDB;

	/* Clean up mutexes */
	for (int s = 0; s < RSD555_STREAM_COUNT; s++)
		pthread_mutex_destroy(&g_state.video[s].sources_lock);
	pthread_mutex_destroy(&g_state.audio.sources_lock);

	if (g_state.ctrl)
		rss_ctrl_destroy(g_state.ctrl);

	rss_config_free(dctx.cfg);
	rss_daemon_cleanup("rsd-555");

	return 0;
}
