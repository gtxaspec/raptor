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

#define RSD_MAX_CLIENTS     8
#define RSD_VIDEO_PT        96
#define RSD_VIDEO_CLOCK     90000
#define RSD_AUDIO_PT        0       /* PCMU */
#define RSD_AUDIO_CLOCK     8000
#define RSD_BUF_SIZE        4096

/* Per-client stream state */
typedef struct {
	Compy_RtpTransport  *rtp;
	Compy_NalTransport  *nal;       /* video only */
	Compy_Rtcp          *rtcp;
	int64_t              last_rtcp;
	bool                 playing;
} rsd_stream_t;

/* Per-client state */
typedef struct rsd_client {
	int                  fd;
	struct sockaddr_in   addr;
	uint64_t             session_id;
	rsd_stream_t         video;
	rsd_stream_t         audio;
	uint64_t             video_read_seq;
	bool                 waiting_keyframe;
	bool                 active;
	bool                 is_tcp;

	/* UDP socket fds (for cleanup) */
	int                  udp_rtp_fd;
	int                  udp_rtcp_fd;

	/* RTSP parse buffer */
	char                 recv_buf[RSD_BUF_SIZE];
	size_t               recv_len;
} rsd_client_t;

/* Server state */
typedef struct {
	int                  listen_fd;
	int                  epoll_fd;
	rss_ctrl_t          *ctrl;
	rss_config_t        *cfg;

	/* Clients */
	rsd_client_t        *clients[RSD_MAX_CLIENTS];
	int                  client_count;
	pthread_mutex_t      clients_lock;

	/* Rings */
	rss_ring_t          *ring_main;
	rss_ring_t          *ring_audio;
	uint64_t             ring_read_seq;
	uint64_t             audio_read_seq;
	bool                 has_audio;

	/* Frame copy buffer (ring data is shared memory that can be
	 * overwritten by the producer; we copy before sending) */
	uint8_t             *frame_buf;
	uint32_t             frame_buf_size;

	volatile sig_atomic_t *running;

	int                  port;
} rsd_server_t;

/* rsd_server.c */
int  rsd_server_init(rsd_server_t *srv);
void rsd_server_run(rsd_server_t *srv);
void rsd_server_deinit(rsd_server_t *srv);

/* rsd_session.c */
void rsd_handle_rtsp_data(rsd_server_t *srv, rsd_client_t *client,
			  const char *data, size_t len);

/* rsd_ring_reader.c */
void *rsd_ring_reader_thread(void *arg);
void *rsd_audio_reader_thread(void *arg);

#endif /* RSD_H */
