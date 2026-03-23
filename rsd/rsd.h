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
	uint64_t             video_read_seq;
	bool                 waiting_keyframe;
	bool                 active;

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

	/* Ring */
	rss_ring_t          *ring_main;
	uint64_t             ring_read_seq;

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
void rsd_send_video_frame(rsd_client_t *c, const uint8_t *data,
			  uint32_t len, int64_t timestamp, bool is_key);

#endif /* RSD_H */
