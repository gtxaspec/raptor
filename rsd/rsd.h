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

#define RSD_MAX_CLIENTS 8
#define RSD_VIDEO_PT	96
#define RSD_VIDEO_CLOCK 90000
#define RSD_BUF_SIZE	4096

/* Audio codec IDs (matches RAD ring codec field) */
#define RSD_CODEC_PCMU	 0
#define RSD_CODEC_PCMA	 8
#define RSD_CODEC_L16	 11
#define RSD_AUDIO_PT_L16 97 /* dynamic PT for L16 */

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
	struct sockaddr_in addr;
	uint64_t session_id;
	rsd_stream_t video;
	rsd_stream_t audio;
	uint64_t video_read_seq;
	bool waiting_keyframe;
	bool active;
	bool is_tcp;
	int stream_idx;	      /* RSD_STREAM_MAIN or RSD_STREAM_SUB */
	uint32_t video_codec; /* RSS_CODEC_H264 or RSS_CODEC_H265 */

	/* UDP socket fds (for cleanup) */
	int udp_rtp_fd;
	int udp_rtcp_fd;

	/* RTSP parse buffer */
	char recv_buf[RSD_BUF_SIZE];
	size_t recv_len;
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
