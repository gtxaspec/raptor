/*
 * rsd.h -- RSD internal shared state
 */

#ifndef RSD_H
#define RSD_H

#include <compy.h>
#include <rss_ipc.h>
#include <rss_common.h>

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <netinet/in.h>

#define RSD_MAX_CLIENTS	     8
#define RSD_VIDEO_PT	     96
#define RSD_VIDEO_CLOCK	     90000
#define RSD_BUF_SIZE	     4096
#define RSD_IDLE_TIMEOUT_SEC 60 /* disconnect idle clients (slowloris protection) */

/* Audio codec IDs (matches RAD ring codec field) */
#define RSD_CODEC_PCMU 0
#define RSD_CODEC_PCMA 8
#define RSD_CODEC_L16  11
#define RSD_CODEC_AAC  97
#define RSD_CODEC_OPUS 111

/* RTP payload types for audio */
#define RSD_AUDIO_PT_L16   96  /* dynamic PT for L16 */
#define RSD_AUDIO_PT_AAC   97  /* dynamic PT for AAC (RFC 3640) */
#define RSD_AUDIO_PT_OPUS  111 /* dynamic PT for Opus (RFC 7587) */
#define RSD_BACKCHANNEL_PT 110 /* backchannel audio PT (PCMU default) */

/* Stream index for per-ring state */
#define RSD_STREAM_MAIN	 0
#define RSD_STREAM_SUB	 1
#define RSD_STREAM_COUNT 2

/* Per-client stream state */
typedef struct {
	Compy_RtpTransport *rtp;
	Compy_NalTransport *nal; /* video only */
	Compy_Rtcp *rtcp;
	int64_t last_rtcp;
	bool playing;
} rsd_stream_t;

/* Per-client state */
typedef struct rsd_client {
	int fd;
	struct sockaddr_storage addr;
#ifdef COMPY_HAS_TLS
	Compy_TlsConn *tls;
#endif
	uint64_t session_id;
	rsd_stream_t video;
	rsd_stream_t audio;
	uint64_t video_read_seq;
	bool waiting_keyframe;
	bool active;
	uint32_t video_ts_offset; /* subtracted from global RTP ts for per-client zero-base */
	bool video_ts_base_set;	  /* true after first keyframe sets the offset */
	uint32_t audio_ts_offset;
	bool audio_ts_base_set;
	bool is_tcp;
	int stream_idx;	      /* RSD_STREAM_MAIN or RSD_STREAM_SUB */
	uint32_t video_codec; /* RSS_CODEC_H264 or RSS_CODEC_H265 */

	/* Backchannel (client → server audio) */
	Compy_Backchannel *backchannel;
	rss_ring_t *speaker_ring; /* created on first backchannel packet */
	void *bc_recv;		  /* rsd_bc_recv_t, kept alive for callback */

	/* TCP interleaved channel numbers (for RTCP routing) */
	uint8_t video_rtcp_ch; /* RTCP channel for video (default 1) */
	uint8_t audio_rtcp_ch; /* RTCP channel for audio (default 3) */

	/* UDP socket fds (for cleanup) */
	int udp_rtp_fd;
	int udp_rtcp_fd;
	bool rtcp_in_epoll; /* true once udp_rtcp_fd is added to epoll */

	/* RTSP parse buffer */
	char recv_buf[RSD_BUF_SIZE];
	size_t recv_len;

	/* Connection tracking */
	int64_t last_activity; /* monotonic timestamp (us) */
} rsd_client_t;

/* Per-ring reader state */
typedef struct {
	rss_ring_t *ring;
	uint64_t read_seq;
	uint8_t *frame_buf;
	uint32_t frame_buf_size;
	int idx; /* RSD_STREAM_MAIN or RSD_STREAM_SUB */
} rsd_ring_ctx_t;

/* Server state */
typedef struct {
	int listen_fd;
	int epoll_fd;
	rss_ctrl_t *ctrl;
	rss_config_t *cfg;
	const char *config_path;

	/* Clients */
	rsd_client_t *clients[RSD_MAX_CLIENTS];
	int client_count;
	pthread_mutex_t clients_lock;

	/* Video rings */
	rsd_ring_ctx_t video[RSD_STREAM_COUNT];

	/* Audio ring */
	rss_ring_t *ring_audio;
	uint64_t audio_read_seq;
	bool has_audio;

	volatile sig_atomic_t *running;

	int port;
	int max_clients;      /* runtime limit (≤ RSD_MAX_CLIENTS) */
	int session_timeout;  /* RTSP session timeout in seconds */

	/* Digest auth (NULL = no auth required) */
	Compy_Auth *auth;

	/* Custom stream endpoints (empty = use defaults) */
	char endpoint_main[64];
	char endpoint_sub[64];

#ifdef COMPY_HAS_TLS
	/* TLS context for RTSPS (NULL = plain RTSP) */
	Compy_TlsContext *tls_ctx;
#endif
} rsd_server_t;

/* rsd_server.c */
int rsd_server_init(rsd_server_t *srv);
void rsd_server_run(rsd_server_t *srv);
void rsd_server_deinit(rsd_server_t *srv);

/* rsd_session.c */
void rsd_handle_rtsp_data(rsd_server_t *srv, rsd_client_t *client, const char *data, size_t len);

/* rsd_ring_reader.c */
void rsd_set_server_for_readers(rsd_server_t *srv);
void *rsd_video_reader_thread(void *arg);
void *rsd_audio_reader_thread(void *arg);

#endif /* RSD_H */
