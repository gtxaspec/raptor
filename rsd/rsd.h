/*
 * rsd.h -- RSD internal shared state
 */

#ifndef RSD_H
#define RSD_H

#include <compy.h>
#include <rss_ipc.h>
#include <rss_common.h>

#include <pthread.h>
#include <stdatomic.h>
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
#define RSD_AUDIO_PT_L16   98  /* dynamic PT for L16 */
#define RSD_AUDIO_PT_AAC   97  /* dynamic PT for AAC (RFC 3640) */
#define RSD_AUDIO_PT_OPUS  111 /* dynamic PT for Opus (RFC 7587) */
#define RSD_BACKCHANNEL_PT 110 /* backchannel audio PT (PCMU default) */

/* Stream index for per-ring state */
#define RSD_STREAM_MAIN	 0
#define RSD_STREAM_SUB	 1
#define RSD_STREAM_COUNT 6 /* main+sub per sensor, up to 3 sensors */

/* Per-client stream state */
typedef struct {
	Compy_RtpTransport *rtp;
	Compy_NalTransport *nal; /* video only */
	Compy_Rtcp *rtcp;
	int64_t last_rtcp;
	atomic_bool playing;
} rsd_stream_t;

/* Per-client send queue — decouples ring reader from network I/O.
 * Every entry holds a malloc'd copy of the frame payload so the
 * reader can overwrite frame_buf with the next ring frame without
 * waiting for any send thread to finish. The memcpy cost is small
 * next to the send-latency hit we'd otherwise take from a barrier
 * wait, especially on slow single-core SoCs. */
#define RSD_SENDQ_SLOTS	  8
#define RSD_FRAME_VIDEO	  0
#define RSD_FRAME_AUDIO	  1
#define RSD_SENDQ_OK	  0
#define RSD_SENDQ_DROPPED 1

typedef struct {
	const uint8_t *data; /* malloc'd copy or rmem pointer (zerocopy) */
	uint32_t len;
	uint32_t rtp_ts;
	uint8_t type;	  /* RSD_FRAME_VIDEO or RSD_FRAME_AUDIO */
	uint32_t codec;	  /* audio codec (RSD_FRAME_AUDIO only) */
	bool zerocopy;	  /* true = rmem pointer, don't free */
	uint8_t buf_idx;  /* refmode: encoder buffer index */
	uint32_t buf_gen; /* refmode: generation at peek time */
} rsd_sendq_entry_t;

typedef struct {
	rsd_sendq_entry_t entries[RSD_SENDQ_SLOTS];
	int head;
	int tail;
	int count;
	pthread_mutex_t lock;
	pthread_cond_t cond;
	bool shutdown;
} rsd_sendq_t;

/* Per-client state */
typedef struct rsd_client {
	struct rsd_server *srv; /* back-pointer to server (set once at accept) */
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
	uint32_t video_ts_offset;      /* subtracted from global RTP ts for per-client rebase */
	uint32_t video_ts_rand;	       /* random initial offset (declared in RTP-Info rtptime) */
	bool video_ts_base_set;	       /* true after first keyframe sets the offset */
	uint32_t last_video_client_ts; /* per-client monotonic enforcement */
	bool has_last_video_client_ts;
	uint32_t audio_ts_offset;
	uint32_t audio_ts_rand;
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

	/* Deferred PLAY — set inside compy callback, applied after
	 * write_lock is released to avoid lock-order inversion with
	 * clients_lock (reader threads take clients_lock → write_lock). */
	bool play_pending;

	/* RTSP parse buffer */
	char recv_buf[RSD_BUF_SIZE];
	size_t recv_len;

	/* Connection tracking */
	int64_t last_activity; /* monotonic timestamp (us) */

	/* Write mutex — serializes RTP data (send thread) and RTSP
	 * responses (server thread) on TCP interleaved connections. */
	pthread_mutex_t write_lock;

	/* Send queue (per-client, decouples reader from I/O) */
	rsd_sendq_t sendq;
	pthread_t send_tid;
	bool send_thread_running;
} rsd_client_t;

/*
 * Per-ring reader state.
 * ring pointer: written only by the reader thread (open/close), read
 * by the session thread during DESCRIBE/SETUP via a local snapshot.
 * Pointer-sized loads are naturally atomic on MIPS32; the snapshot
 * pattern handles TOCTOU. Not _Atomic to avoid seq_cst overhead on
 * the ~15 hot-path accesses in the reader thread.
 */
typedef struct {
	struct rsd_server *srv; /* back-pointer to server */
	rss_ring_t *ring;
	uint64_t read_seq;
	uint8_t *frame_buf;
	uint32_t frame_buf_size;
	int idx;
	const char *ring_name; /* for reconnection after RVD restart */

	/* Cached stream info — written by reader thread on ring open,
	 * read by session thread during DESCRIBE. Atomic to avoid
	 * TSAN races between reader and session threads. */
	_Atomic uint32_t last_codec;
	_Atomic uint32_t last_width;
	_Atomic uint32_t last_height;
	_Atomic uint32_t last_fps_num;
	_Atomic uint32_t last_fps_den;
	_Atomic uint8_t last_profile;
	_Atomic uint8_t last_level;

	/* Cached SPS/PPS for SDP sprop-parameter-sets.
	 * Written by the reader thread (release), read by session thread
	 * during DESCRIBE (acquire). Lengths are atomic to prevent torn
	 * reads of the buffer data. */
	uint8_t sps[256];
	uint8_t pps[64];
	_Atomic uint16_t sps_len;
	_Atomic uint16_t pps_len;
} rsd_ring_ctx_t;

/* Server state */
typedef struct rsd_server {
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

	/* Audio ring — same cross-thread access pattern as video ring pointers */
	rss_ring_t *ring_audio;
	uint64_t audio_read_seq;
	bool has_audio;

	volatile sig_atomic_t *running;

	int port;
	int max_clients;     /* runtime limit (≤ RSD_MAX_CLIENTS) */
	int session_timeout; /* RTSP session timeout in seconds */
	int tcp_sndbuf;	     /* TCP send buffer size (bytes) */
	bool rtcp_sr;	     /* send RTCP Sender Reports (default false) */

	/* Digest auth (NULL = no auth required) */
	Compy_Auth *auth;

	/* Custom endpoint aliases, indexed by stream number.
	 *   [0] sensor 0 main — config key: endpoint_main
	 *   [1] sensor 0 sub  — config key: endpoint_sub
	 *   [2] sensor 1 main — config key: endpoint_s1_main
	 *   [3] sensor 1 sub  — config key: endpoint_s1_sub
	 *   [4] sensor 2 main — config key: endpoint_s2_main
	 *   [5] sensor 2 sub  — config key: endpoint_s2_sub
	 * Blank = use default /streamN. Setting an alias disables the default
	 * path for that stream (per-stream security gate). Validated and
	 * populated by rsd_endpoints_load(). */
	char endpoints[RSD_STREAM_COUNT][64];

	/* SDP session name and info */
	char session_name[64];
	char session_info[128];

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
void rsd_handle_rtsp_data(rsd_client_t *client, const char *data, size_t len);
void rsd_endpoints_load(rsd_server_t *srv, rss_config_t *cfg);

/* rsd_ring_reader.c */
void *rsd_video_reader_thread(void *arg);
void *rsd_audio_reader_thread(void *arg);
int rsd_sendq_init(rsd_sendq_t *q);
void rsd_sendq_destroy(rsd_sendq_t *q);
void *rsd_client_send_thread(void *arg);

#endif /* RSD_H */
