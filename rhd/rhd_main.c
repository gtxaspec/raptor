/*
 * rhd_main.c -- Raptor HTTP Daemon
 *
 * Minimal HTTP server for JPEG snapshots and MJPEG streaming.
 *
 * Endpoints:
 *   /snap.jpg   — latest JPEG snapshot (from JPEG ring)
 *   /mjpeg      — MJPEG stream (from jpeg ring)
 *
 * Dual-stack IPv6 (serves both IPv4 and IPv6 clients).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <time.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <poll.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <stdatomic.h>

#include "rhd.h"

/* Index page — loaded from file on first request, cached */
static char *index_html;
static int index_html_len;

/* Base64 and HTTP auth moved to raptor-common (rss_http.h) */

/* ── Snapshot handler — serve latest JPEG from ring ── */

/*
 * Capture time of a JPEG frame: producer UTC mapping when published
 * (ring v4), wall clock otherwise — a snapshot is "now" anyway.
 */
static uint64_t jpeg_frame_utc(rss_ring_t *ring, const rss_ring_slot_t *meta)
{
	int64_t off;
	uint8_t status;
	if (rss_ring_get_utc(ring, &off, &status) == 0)
		return (uint64_t)(meta->timestamp + off);
	struct timespec ts;
	clock_gettime(CLOCK_REALTIME, &ts);
	return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000;
}

/* JPEG ring names: sensor 0 = jpeg0/jpeg1, sensor N = sN_jpeg0/sN_jpeg1 */
static const char *jpeg_ring_names[RHD_MAX_JPEG] = {"jpeg0",	"jpeg1",    "s1_jpeg0",
						    "s1_jpeg1", "s2_jpeg0", "s2_jpeg1"};

/*
 * Open a JPEG ring slot and size the shared frame buffers for it.
 * Rings come and go with encoder idle management, so every consumer
 * path opens on demand instead of waiting for the periodic sweep: a
 * request that lands while a slot is closed costs one open, not a
 * 404. Safe to call with the slot already open.
 */
static rss_ring_t *jpeg_ring_open_slot(rhd_server_t *srv, int j)
{
	if (srv->jpeg_rings[j])
		return srv->jpeg_rings[j];

	rss_ring_t *ring = rss_ring_open(jpeg_ring_names[j]);
	if (!ring)
		return NULL;

	rss_ring_check_version(ring, jpeg_ring_names[j]);
	srv->jpeg_rings[j] = ring;
	srv->jpeg_read_seqs[j] = 0;

	uint32_t mfs = rss_ring_max_frame_size(ring);
	uint32_t cap = mfs + RSS_JPEG_EXIF_MAX + RSS_JPEG_SIG_SEGMENT;
	if (mfs > srv->frame_buf_size || !srv->frame_buf) {
		free(srv->frame_buf);
		srv->frame_buf_size = mfs;
		srv->frame_buf_cap = cap;
		srv->frame_buf = malloc(cap);
		if (!srv->frame_buf) {
			RSS_WARN("failed to allocate frame buffer (%u bytes)", cap);
			srv->frame_buf_size = 0;
			srv->frame_buf_cap = 0;
		}
	}
	if (cap > srv->snap_buf_size) {
		free(srv->snap_buf);
		srv->snap_buf_size = cap;
		srv->snap_buf = malloc(cap);
		if (!srv->snap_buf) {
			RSS_WARN("failed to allocate snapshot buffer (%u bytes)", cap);
			srv->snap_buf_size = 0;
		}
	}
	if (j + 1 > srv->jpeg_ring_count)
		srv->jpeg_ring_count = j + 1;

	RSS_DEBUG("jpeg ring open (%s, %u byte frames)", jpeg_ring_names[j], mfs);
	return ring;
}

/*
 * Park a /snap request. Demand on the ring is not signalled here: the main
 * loop derives it from client state, so a client that disconnects while
 * parked releases the encoder without a separate cleanup path.
 */
static bool handle_snapshot(rhd_client_t *c, int stream, rss_ring_t *ring)
{
	if (!ring) {
		http_error(c, "503 Service Unavailable", "JPEG ring not available");
		return false;
	}

	/*
	 * Start the cursor past what the ring already holds, so the reply is
	 * a frame produced for this request rather than whatever the encoder
	 * left behind the last time something watched it.
	 */
	const rss_ring_header_t *hdr = rss_ring_get_header(ring);
	c->snap_seq = atomic_load(&hdr->write_seq);
	c->snap_stream = stream;
	c->snap_pending = true;

	/*
	 * Drop the request text now that it has been acted on. It still ends
	 * in the terminator the reader matches on, so leaving it would let the
	 * next byte from the client re-run this handler against the same
	 * request and park it again on a fresh deadline.
	 */
	c->recv_len = 0;

	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	c->snap_deadline = ts.tv_sec * 1000 + ts.tv_nsec / 1000000 + RHD_SNAP_TIMEOUT_MS;

	return true; /* the main loop completes it or times it out */
}

/* ── Per-client send queue ── */

static int rhd_sendq_init(rhd_sendq_t *q)
{
	memset(q, 0, sizeof(*q));
	if (pthread_mutex_init(&q->lock, NULL) != 0)
		return -1;
	if (pthread_cond_init(&q->cond, NULL) != 0) {
		pthread_mutex_destroy(&q->lock);
		return -1;
	}
	return 0;
}

static void rhd_sendq_destroy(rhd_sendq_t *q)
{
	while (q->count > 0) {
		free(q->entries[q->tail].data);
		q->tail = (q->tail + 1) % RHD_SENDQ_SLOTS;
		q->count--;
	}
	pthread_cond_destroy(&q->cond);
	pthread_mutex_destroy(&q->lock);
}

static void rhd_sendq_flush_locked(rhd_sendq_t *q)
{
	while (q->count > 0) {
		free(q->entries[q->tail].data);
		q->tail = (q->tail + 1) % RHD_SENDQ_SLOTS;
		q->count--;
	}
	q->head = 0;
	q->tail = 0;
}

static int rhd_sendq_push(rhd_sendq_t *q, uint8_t type, const uint8_t *data, uint32_t len,
			  int codec, int sample_rate)
{
	pthread_mutex_lock(&q->lock);
	if (q->shutdown) {
		pthread_mutex_unlock(&q->lock);
		return -1;
	}

	if (q->count >= RHD_SENDQ_SLOTS) {
		rhd_sendq_flush_locked(q);
		/* For MJPEG, dropping is fine — each frame is independent */
	}

	uint8_t *copy = malloc(len);
	if (!copy) {
		pthread_mutex_unlock(&q->lock);
		return -1;
	}
	memcpy(copy, data, len);

	rhd_sendq_entry_t *slot = &q->entries[q->head];
	slot->data = copy;
	slot->len = len;
	slot->type = type;
	slot->codec = codec;
	slot->sample_rate = sample_rate;

	q->head = (q->head + 1) % RHD_SENDQ_SLOTS;
	q->count++;

	pthread_cond_signal(&q->cond);
	pthread_mutex_unlock(&q->lock);
	return RHD_SENDQ_OK;
}

/* Per-client send thread — drains sendq through blocking HTTP writes.
 * On send error, sets q->shutdown so the main loop removes the client. */
static void *rhd_client_send_thread(void *arg)
{
	rhd_client_t *c = arg;
	rhd_sendq_t *q = &c->sendq;

	while (1) {
		pthread_mutex_lock(&q->lock);
		while (q->count == 0 && !q->shutdown)
			pthread_cond_wait(&q->cond, &q->lock);

		if (q->shutdown) {
			pthread_mutex_unlock(&q->lock);
			break;
		}

		rhd_sendq_entry_t entry = q->entries[q->tail];
		q->entries[q->tail].data = NULL;
		q->tail = (q->tail + 1) % RHD_SENDQ_SLOTS;
		q->count--;
		pthread_mutex_unlock(&q->lock);

		int ret = 0;
		if (entry.type == RHD_FRAME_MJPEG) {
			ret = http_send_mjpeg_frame(c, entry.data, entry.len);
		} else {
			ret = rhd_audio_send_frame(c, entry.codec, entry.sample_rate, entry.data,
						   entry.len, c->audio_page_seq, c->audio_granule);
			if (ret >= 0) {
				c->audio_page_seq++;
				if (entry.codec == RHD_CODEC_OPUS)
					c->audio_granule += 960;
				else if (entry.codec == RHD_CODEC_AAC)
					c->audio_granule += 1024;
				else
					c->audio_granule += entry.len / 2;
			}
		}

		free(entry.data);

		if (ret < 0) {
			/* Send failed — mark for shutdown, main loop will remove.
			 * Don't try to send more frames on a dead connection. */
			pthread_mutex_lock(&q->lock);
			q->shutdown = true;
			pthread_mutex_unlock(&q->lock);
			break;
		}
	}

	return NULL;
}

static void rhd_start_send_thread(rhd_client_t *c)
{
	if (rhd_sendq_init(&c->sendq) != 0)
		return;
	pthread_attr_t sa;
	pthread_attr_init(&sa);
	pthread_attr_setstacksize(&sa, 128 * 1024);
	if (pthread_create(&c->send_tid, &sa, rhd_client_send_thread, c) == 0) {
		c->send_thread_running = true;
	} else {
		rhd_sendq_destroy(&c->sendq);
	}
	pthread_attr_destroy(&sa);
}

/* ── Client management ── */

static void remove_client(rhd_server_t *srv, int idx)
{
	rhd_client_t *c = srv->clients[idx];
	char addrstr[INET6_ADDRSTRLEN];
	RSS_INFO("client %s:%u disconnected%s", client_addr_str(&c->addr, addrstr, sizeof(addrstr)),
		 client_port(&c->addr), c->is_mjpeg ? " (mjpeg)" : "");

	/* Stop send thread before closing fd/TLS */
	if (c->send_thread_running) {
		pthread_mutex_lock(&c->sendq.lock);
		c->sendq.shutdown = true;
		pthread_cond_signal(&c->sendq.cond);
		pthread_mutex_unlock(&c->sendq.lock);
		shutdown(c->fd, SHUT_WR);
		pthread_join(c->send_tid, NULL);
		c->send_thread_running = false;
		rhd_sendq_destroy(&c->sendq);
	}

	epoll_ctl(srv->epoll_fd, EPOLL_CTL_DEL, c->fd, NULL);
#ifdef RSS_HAS_TLS
	rss_tls_close(c->tls);
#endif
	close(c->fd);
	free(c->send_buf);
	free(c);
	srv->clients[idx] = srv->clients[--srv->client_count];
}

/*
 * Complete every parked snapshot that has a frame waiting, and fail the ones
 * that ran out of time. Runs once per main-loop pass, after ring demand has
 * been applied so an encoder woken by the request has had a chance to produce.
 */
static void snap_poll(rhd_server_t *srv)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	int64_t now = ts.tv_sec * 1000 + ts.tv_nsec / 1000000;

	for (int i = srv->client_count - 1; i >= 0; i--) {
		rhd_client_t *c = srv->clients[i];
		if (!c->snap_pending)
			continue;

		rss_ring_t *ring = srv->jpeg_rings[c->snap_stream];
		if (!ring) {
			c->snap_pending = false;
			http_error(c, "503 Service Unavailable", "JPEG ring not available");
			remove_client(srv, i);
			continue;
		}

		/*
		 * Read only when there is somewhere to read into. The deadline
		 * below still runs without a buffer, so a failed allocation
		 * fails the request rather than parking it forever.
		 */
		uint32_t len = 0;
		rss_ring_slot_t meta;
		int ret = srv->snap_buf ? rss_ring_read(ring, &c->snap_seq, srv->snap_buf,
							srv->snap_buf_size, &len, &meta)
					: -1;

		/* Lapped by the writer: resync onto the newest frame and retry. */
		if (ret == RSS_EOVERFLOW) {
			const rss_ring_header_t *hdr = rss_ring_get_header(ring);
			uint64_t w = atomic_load(&hdr->write_seq);
			c->snap_seq = w > 0 ? w - 1 : 0;
			continue;
		}

		if (ret == 0 && len >= 2 && srv->snap_buf[0] == 0xFF && srv->snap_buf[1] == 0xD8) {
			c->snap_pending = false;
			if (srv->exif_timestamp) {
				int n = rss_jpeg_insert_exif(srv->snap_buf, srv->snap_buf_size, len,
							     jpeg_frame_utc(ring, &meta));
				if (n > 0)
					len = (uint32_t)n;
			}
			if (srv->sign_ok) {
				int n = rss_jpeg_sign(srv->snap_buf, srv->snap_buf_size, len,
						      &srv->sign_key);
				if (n > 0)
					len = (uint32_t)n;
			}
			if (http_send_async(c, srv->epoll_fd, "image/jpeg", srv->snap_buf, len) <
			    0) {
				http_error(c, "500 Internal Server Error", "Out of memory");
				remove_client(srv, i);
			}
			continue;
		}

		if (now >= c->snap_deadline) {
			c->snap_pending = false;
			http_error(c, "503 Service Unavailable", "No snapshot available yet");
			remove_client(srv, i);
		}
	}
}

/* True while any client has a snapshot parked on this JPEG ring. */
static bool snap_waiting_on(const rhd_server_t *srv, int stream)
{
	for (int i = 0; i < srv->client_count; i++)
		if (srv->clients[i]->snap_pending && srv->clients[i]->snap_stream == stream)
			return true;
	return false;
}

static int find_client(rhd_server_t *srv, int fd)
{
	for (int i = 0; i < srv->client_count; i++)
		if (srv->clients[i]->fd == fd)
			return i;
	return -1;
}

/* Parse ?stream=N query parameter, default 0 */
static int parse_stream_param(const char *path)
{
	const char *p = strstr(path, "stream=");
	if (p) {
		char *end;
		int v = (int)strtol(p + 7, &end, 10);
		if (end != p + 7 && v >= 0 && v < RHD_MAX_JPEG)
			return v;
	}
	return 0;
}

/* ── Request parsing ── */

static void handle_request(rhd_server_t *srv, rhd_client_t *c)
{
	/* Find request line */
	char *end = strstr(c->recv_buf, "\r\n\r\n");
	if (!end)
		return; /* incomplete */

	/* Parse method and path */
	char method[8] = {0}, path[256] = {0};
	sscanf(c->recv_buf, "%7s %255s", method, path);

	char addrstr[INET6_ADDRSTRLEN];
	RSS_INFO("%s %s from %s:%u", method, path,
		 client_addr_str(&c->addr, addrstr, sizeof(addrstr)), client_port(&c->addr));

	if (strcmp(method, "GET") != 0) {
		http_error(c, "405 Method Not Allowed", "GET only");
		return;
	}

	if (!http_check_auth(srv, c->recv_buf)) {
		http_401(c);
		return;
	}

	if (strncmp(path, "/snap", 5) == 0) {
		int si = parse_stream_param(path);
		rss_ring_t *ring = jpeg_ring_open_slot(srv, si);
		if (ring) {
			if (handle_snapshot(c, si, ring))
				return; /* keep alive — parked for the main loop */
		} else {
			http_error(c, "404 Not Found", "Stream not available");
		}
	} else if (strncmp(path, "/mjpeg", 6) == 0 || strncmp(path, "/mjpg", 5) == 0) {
		int si = parse_stream_param(path);
		if (!jpeg_ring_open_slot(srv, si)) {
			http_error(c, "404 Not Found", "Stream not available");
			return;
		}
		http_send_mjpeg_header(c);
		c->is_mjpeg = true;
		c->mjpeg_stream = si;
		rhd_start_send_thread(c);
		return; /* keep alive */
	} else if (strcmp(path, "/audio") == 0) {
		if (!srv->audio_ring) {
			srv->audio_ring = rss_ring_open("audio");
			if (srv->audio_ring) {
				const rss_ring_header_t *ahdr =
					rss_ring_get_header(srv->audio_ring);
				srv->audio_codec = ahdr->codec;
				srv->audio_sample_rate = ahdr->fps_num;
				srv->audio_adts_rate = (ahdr->profile == 5) ? (int)ahdr->fps_num / 2
									    : (int)ahdr->fps_num;
			}
		}
		if (!srv->audio_ring) {
			http_error(c, "404 Not Found", "Audio not available");
		} else {
			if (rhd_audio_send_header(c, srv->audio_codec,
						  srv->audio_codec == RHD_CODEC_AAC
							  ? srv->audio_adts_rate
							  : srv->audio_sample_rate) < 0)
				return;
			c->is_audio = true;
			rhd_start_send_thread(c);
			return; /* keep alive */
		}
	} else if (strcmp(path, "/") == 0) {
		if (!index_html) {
			index_html = rss_read_file(RHD_INDEX_PATH, &index_html_len);
			if (index_html)
				RSS_DEBUG("loaded %s (%d bytes)", RHD_INDEX_PATH, index_html_len);
			else
				RSS_WARN("%s not found", RHD_INDEX_PATH);
		}
		if (index_html)
			http_send(c, "200 OK", "text/html", index_html, index_html_len);
		else
			http_error(c, "404 Not Found", "index page not installed");
	} else {
		http_error(c, "404 Not Found", "Not found");
	}
}

/* ── MJPEG streaming ── */

static void stream_mjpeg_frame(rhd_server_t *srv, int stream, const uint8_t *data, uint32_t len)
{
	for (int i = srv->client_count - 1; i >= 0; i--) {
		rhd_client_t *c = srv->clients[i];
		if (!c->is_mjpeg || c->mjpeg_stream != stream)
			continue;
		if (!c->send_thread_running) {
			remove_client(srv, i);
			continue;
		}
		/* Push returns -1 if shutdown — send thread flagged an error */
		if (rhd_sendq_push(&c->sendq, RHD_FRAME_MJPEG, data, len, 0, 0) < 0)
			remove_client(srv, i);
	}
}

/* ── Server init ── */

/* ── Control socket ── */

static int rhd_ctrl_handler(const char *cmd_json, char *resp_buf, int resp_buf_size, void *userdata)
{
	rhd_server_t *srv = userdata;

	int rc = rss_ctrl_handle_common(cmd_json, resp_buf, resp_buf_size, srv->cfg,
					srv->config_path);
	if (rc >= 0)
		return rc;

	char cmd[64];
	if (rss_json_get_str(cmd_json, "cmd", cmd, sizeof(cmd)) != 0)
		return rss_ctrl_resp_error(resp_buf, resp_buf_size, "missing cmd");

	if (strcmp(cmd, "clients") == 0) {
		cJSON *r = cJSON_CreateObject();
		cJSON_AddStringToObject(r, "status", "ok");
		cJSON_AddNumberToObject(r, "count", srv->client_count);
		cJSON_AddNumberToObject(r, "max_clients", srv->max_clients);
		cJSON *arr = cJSON_AddArrayToObject(r, "clients");
		for (int i = 0; i < srv->client_count; i++) {
			rhd_client_t *c = srv->clients[i];
			char addr[INET6_ADDRSTRLEN];
			client_addr_str(&c->addr, addr, sizeof(addr));
			const char *type = c->is_mjpeg	 ? "mjpeg"
					   : c->is_audio ? "audio"
							 : "snapshot";
			cJSON *item = cJSON_CreateObject();
			cJSON_AddStringToObject(item, "ip", addr);
			cJSON_AddStringToObject(item, "type", type);
			cJSON_AddItemToArray(arr, item);
		}
		return rss_ctrl_resp_json(resp_buf, resp_buf_size, r);
	}

	/* Default: status */
	int mjpeg = 0, audio = 0;
	for (int i = 0; i < srv->client_count; i++) {
		if (srv->clients[i]->is_mjpeg)
			mjpeg++;
		if (srv->clients[i]->is_audio)
			audio++;
	}
	cJSON *r = cJSON_CreateObject();
	cJSON_AddStringToObject(r, "status", "ok");
	cJSON_AddNumberToObject(r, "clients", srv->client_count);
	cJSON_AddNumberToObject(r, "mjpeg", mjpeg);
	cJSON_AddNumberToObject(r, "audio", audio);
	cJSON_AddNumberToObject(r, "port", srv->port);
	cJSON_AddNumberToObject(r, "jpeg_rings", srv->jpeg_ring_count);
	cJSON_AddBoolToObject(r, "exif_timestamp", srv->exif_timestamp);
	cJSON_AddBoolToObject(r, "sign_snapshots", srv->sign_ok);
#ifdef RSS_HAS_TLS
	cJSON_AddBoolToObject(r, "tls", srv->tls != NULL);
#else
	cJSON_AddBoolToObject(r, "tls", false);
#endif
	return rss_ctrl_resp_json(resp_buf, resp_buf_size, r);
}

static int server_init(rhd_server_t *srv)
{
	srv->listen_fd = rss_listen_tcp(srv->port, 8);
	if (srv->listen_fd < 0) {
		RSS_FATAL("listen on port %d: %s", srv->port, strerror(errno));
		return -1;
	}

	srv->epoll_fd = epoll_create1(EPOLL_CLOEXEC);
	struct epoll_event ev = {.events = EPOLLIN, .data.fd = srv->listen_fd};
	if (epoll_ctl(srv->epoll_fd, EPOLL_CTL_ADD, srv->listen_fd, &ev) < 0)
		RSS_ERROR("epoll_ctl add listen_fd: %s", strerror(errno));

	/* Control socket */
	rss_mkdir_p(RSS_RUN_DIR);
	srv->ctrl = rss_ctrl_listen(RSS_RUN_DIR "/rhd.sock");
	if (srv->ctrl) {
		int ctrl_fd = rss_ctrl_get_fd(srv->ctrl);
		if (ctrl_fd >= 0) {
			ev = (struct epoll_event){.events = EPOLLIN, .data.fd = ctrl_fd};
			if (epoll_ctl(srv->epoll_fd, EPOLL_CTL_ADD, ctrl_fd, &ev) < 0)
				RSS_ERROR("epoll_ctl add ctrl_fd: %s", strerror(errno));
		}
	}

	RSS_INFO("HTTP server listening on port %d (dual-stack)", srv->port);
	return 0;
}

/* ── Main loop ── */

static void server_run(rhd_server_t *srv)
{
	uint64_t jpeg_last_ws[RHD_MAX_JPEG] = {0};
	int jpeg_idle[RHD_MAX_JPEG] = {0};
	int jpeg_reconnect_tick = 0;

	/* Audio ring state */
	uint64_t audio_read_seq = 0;
	uint64_t audio_last_write_seq = 0;
	int audio_idle_count = 0;
	uint8_t audio_buf[4096];

	/* Try to open JPEG rings (non-blocking, late producers are opened on demand) */
	for (int j = 0; j < RHD_MAX_JPEG; j++)
		jpeg_ring_open_slot(srv, j);

	/* Try to open audio ring */
	srv->audio_ring = rss_ring_open("audio");
	if (srv->audio_ring) {
		rss_ring_check_version(srv->audio_ring, "audio");
		const rss_ring_header_t *ahdr = rss_ring_get_header(srv->audio_ring);
		srv->audio_codec = ahdr->codec;
		srv->audio_sample_rate = ahdr->fps_num;
		srv->audio_adts_rate =
			(ahdr->profile == 5) ? (int)ahdr->fps_num / 2 : (int)ahdr->fps_num;
		RSS_INFO("audio ring available (codec=%d rate=%d)", srv->audio_codec,
			 srv->audio_sample_rate);
	}

	if (srv->jpeg_ring_count == 0 && !srv->audio_ring)
		RSS_INFO("no rings available at startup, waiting for producers...");

	if (srv->jpeg_ring_count > 0 && (!srv->frame_buf || !srv->snap_buf)) {
		RSS_FATAL("failed to allocate frame buffers");
		return;
	}

	struct epoll_event events[16];
	int ctrl_fd = srv->ctrl ? rss_ctrl_get_fd(srv->ctrl) : -1;

	bool ring_acquired[RHD_MAX_JPEG] = {false};

	while (rss_running(srv->running)) {
		/* Per-ring demand: acquire only the rings that streaming
		 * clients actually watch — each held ring runs a JPEG
		 * encoder, which costs H.264 throughput on old SoCs. */
		bool ring_wanted[RHD_MAX_JPEG] = {false};
		bool has_mjpeg_clients = false;
		bool has_snap_pending = false;
		for (int i = 0; i < srv->client_count; i++) {
			rhd_client_t *mc = srv->clients[i];
			if (mc->is_mjpeg && mc->mjpeg_stream >= 0 &&
			    mc->mjpeg_stream < RHD_MAX_JPEG) {
				ring_wanted[mc->mjpeg_stream] = true;
				has_mjpeg_clients = true;
			}
			/* A parked snapshot is demand too — it is what starts an
			 * idle encoder, and it keeps it running until served. */
			if (mc->snap_pending && mc->snap_stream >= 0 &&
			    mc->snap_stream < RHD_MAX_JPEG) {
				ring_wanted[mc->snap_stream] = true;
				has_snap_pending = true;
			}
		}

		for (int j = 0; j < RHD_MAX_JPEG; j++) {
			if (!srv->jpeg_rings[j])
				continue;
			if (ring_wanted[j] && !ring_acquired[j]) {
				rss_ring_acquire(srv->jpeg_rings[j]);
				ring_acquired[j] = true;
			} else if (!ring_wanted[j] && ring_acquired[j]) {
				rss_ring_release(srv->jpeg_rings[j]);
				ring_acquired[j] = false;
			}
		}

		if (has_mjpeg_clients && srv->frame_buf) {
			for (int j = 0; j < RHD_MAX_JPEG; j++) {
				if (!srv->jpeg_rings[j] || !ring_wanted[j])
					continue;
				uint32_t len;
				rss_ring_slot_t meta;
				int ret = rss_ring_read(srv->jpeg_rings[j], &srv->jpeg_read_seqs[j],
							srv->frame_buf, srv->frame_buf_size, &len,
							&meta);
				if (ret == RSS_EOVERFLOW && srv->jpeg_read_seqs[j] > 0) {
					srv->jpeg_read_seqs[j]--;
					ret = rss_ring_read(srv->jpeg_rings[j],
							    &srv->jpeg_read_seqs[j], srv->frame_buf,
							    srv->frame_buf_size, &len, &meta);
				}
				if (ret == 0 && len >= 2 && srv->frame_buf[0] == 0xFF &&
				    srv->frame_buf[1] == 0xD8) {
					if (srv->exif_timestamp) {
						int n = rss_jpeg_insert_exif(
							srv->frame_buf, srv->frame_buf_cap, len,
							jpeg_frame_utc(srv->jpeg_rings[j], &meta));
						if (n > 0)
							len = (uint32_t)n;
					}
					stream_mjpeg_frame(srv, j, srv->frame_buf, len);
				}
			}
		}

		snap_poll(srv);

		/* Stream audio frames to audio clients */
		bool has_audio_clients = false;
		for (int i = 0; i < srv->client_count; i++)
			if (srv->clients[i]->is_audio) {
				has_audio_clients = true;
				break;
			}

		if (has_audio_clients && !srv->audio_ring) {
			srv->audio_ring = rss_ring_open("audio");
			if (srv->audio_ring) {
				rss_ring_check_version(srv->audio_ring, "audio");
				const rss_ring_header_t *ahdr =
					rss_ring_get_header(srv->audio_ring);
				srv->audio_codec = ahdr->codec;
				srv->audio_sample_rate = ahdr->fps_num;
				srv->audio_adts_rate = (ahdr->profile == 5) ? (int)ahdr->fps_num / 2
									    : (int)ahdr->fps_num;
				audio_read_seq = ahdr->write_seq;
				audio_last_write_seq = 0;
				audio_idle_count = 0;
				RSS_INFO("audio ring reconnected (codec=%d rate=%d)",
					 srv->audio_codec, srv->audio_sample_rate);
			}
		}

		if (has_audio_clients && srv->audio_ring) {
			/* Detect stale ring (RAD restarted) */
			const rss_ring_header_t *ahdr = rss_ring_get_header(srv->audio_ring);
			if (ahdr->write_seq == audio_last_write_seq)
				audio_idle_count++;
			else
				audio_idle_count = 0;
			audio_last_write_seq = ahdr->write_seq;

			if (audio_idle_count >= 40) {
				RSS_INFO("audio ring idle, closing for reconnect");
				rss_ring_close(srv->audio_ring);
				srv->audio_ring = NULL;
				audio_idle_count = 0;
			}

			/* Drain all available audio frames */
			for (int af = 0; af < 20 && srv->audio_ring; af++) {
				uint32_t alen;
				rss_ring_slot_t meta;
				int ret = rss_ring_read(srv->audio_ring, &audio_read_seq, audio_buf,
							sizeof(audio_buf), &alen, &meta);
				if (ret == RSS_EOVERFLOW) {
					audio_read_seq =
						ahdr->write_seq > 0 ? ahdr->write_seq - 1 : 0;
					continue;
				}
				if (ret != 0 || alen == 0)
					break;

				for (int i = srv->client_count - 1; i >= 0; i--) {
					rhd_client_t *ac = srv->clients[i];
					if (!ac->is_audio)
						continue;
					if (!ac->send_thread_running ||
					    rhd_sendq_push(&ac->sendq, RHD_FRAME_AUDIO, audio_buf,
							   alen, srv->audio_codec,
							   srv->audio_codec == RHD_CODEC_AAC
								   ? srv->audio_adts_rate
								   : srv->audio_sample_rate) < 0)
						remove_client(srv, i);
				}
			}
		}

		int timeout =
			(has_mjpeg_clients || has_audio_clients || has_snap_pending) ? 50 : 500;
		int n = epoll_wait(srv->epoll_fd, events, 16, timeout);

		for (int i = 0; i < n; i++) {
			int fd = events[i].data.fd;

			if (fd == ctrl_fd) {
				rss_ctrl_accept_and_handle(srv->ctrl, rhd_ctrl_handler, srv);
				continue;
			}

			if (fd == srv->listen_fd) {
				/* Accept new client */
				struct sockaddr_storage sa;
				socklen_t salen = sizeof(sa);
				int cfd = accept(srv->listen_fd, (struct sockaddr *)&sa, &salen);
				if (cfd < 0)
					continue;

				if (srv->client_count >= srv->max_clients) {
					char addrstr[INET6_ADDRSTRLEN];
					RSS_WARN("rejected %s:%u (max clients %d)",
						 client_addr_str(&sa, addrstr, sizeof(addrstr)),
						 client_port(&sa), srv->max_clients);
					http_send_fd(cfd, "503 Service Unavailable", "text/plain",
						     "Too many clients", 16);
					close(cfd);
					continue;
				}

				int one = 1;
				setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
				int flags = fcntl(cfd, F_GETFL);
				if (flags >= 0)
					fcntl(cfd, F_SETFL, flags | O_NONBLOCK);

				rhd_client_t *c = calloc(1, sizeof(*c));
				if (!c) {
					RSS_ERROR("client alloc failed");
					close(cfd);
					continue;
				}
				c->fd = cfd;
				memcpy(&c->addr, &sa, sizeof(c->addr));
#ifdef RSS_HAS_TLS
				c->srv_tls = srv->tls;
#endif
				srv->clients[srv->client_count++] = c;

				char addrstr[INET6_ADDRSTRLEN];
				RSS_INFO("client %s:%u connected (%d/%d)",
					 client_addr_str(&sa, addrstr, sizeof(addrstr)),
					 client_port(&sa), srv->client_count, srv->max_clients);

				struct epoll_event cev = {.events = EPOLLIN, .data.fd = cfd};
				if (epoll_ctl(srv->epoll_fd, EPOLL_CTL_ADD, cfd, &cev) < 0) {
					RSS_ERROR("epoll_ctl add client fd: %s", strerror(errno));
					srv->client_count--;
					free(c);
					close(cfd);
				}
				continue;
			}

			/* Client data */
			int ci = find_client(srv, fd);
			if (ci < 0)
				continue;

			rhd_client_t *c = srv->clients[ci];

			if (events[i].events & (EPOLLHUP | EPOLLERR)) {
				remove_client(srv, ci);
				continue;
			}

			/* Async send in progress — drain via EPOLLOUT */
			if (c->send_buf && (events[i].events & EPOLLOUT)) {
				uint32_t remain = c->send_len - c->send_off;
				ssize_t nw = rhd_write(c, c->send_buf + c->send_off, remain);
				if (nw > 0) {
					c->send_off += (uint32_t)nw;
					if (c->send_off >= c->send_len) {
						/* Done — close connection */
						remove_client(srv, ci);
					}
				} else if (nw < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
					remove_client(srv, ci);
				}
				continue;
			}

			size_t space = sizeof(c->recv_buf) - c->recv_len - 1;
			if (space == 0) {
				/* Request too large — reject */
				http_error(c, "414 URI Too Long", "Request too large");
				remove_client(srv, ci);
				continue;
			}
			ssize_t nr = rhd_read(c, c->recv_buf + c->recv_len, space);
			if (nr <= 0) {
				remove_client(srv, ci);
				continue;
			}
			c->recv_len += nr;
			c->recv_buf[c->recv_len] = '\0';

			/*
			 * Check for complete HTTP request. A client with a
			 * snapshot already parked is not served a second one on
			 * the same connection: the reply it is waiting for closes
			 * the connection, so anything arriving now is either a
			 * pipelined request that cannot be answered or noise, and
			 * acting on it would re-park with a new deadline.
			 */
			if (strstr(c->recv_buf, "\r\n\r\n") && !c->snap_pending) {
				handle_request(srv, c);
				/* Close non-streaming, non-async connections. A
				 * parked snapshot has neither a send buffer nor a
				 * stream yet, and must outlive this pass. */
				if (!c->is_mjpeg && !c->is_audio && !c->send_buf &&
				    !c->snap_pending)
					remove_client(srv, ci);
			}
		}

		/* Reap stalled async sends */
		{
			struct timespec ts;
			clock_gettime(CLOCK_MONOTONIC, &ts);
			int64_t now = ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
			for (int i = srv->client_count - 1; i >= 0; i--) {
				rhd_client_t *c = srv->clients[i];
				if (c->send_buf && (now - c->send_start) > RHD_SEND_TIMEOUT_MS)
					remove_client(srv, i);
			}
		}

		/* Wait for next frame from active rings */
		if (has_audio_clients && srv->audio_ring)
			rss_ring_wait(srv->audio_ring, 20);
		else if (has_mjpeg_clients && srv->jpeg_rings[0])
			rss_ring_wait(srv->jpeg_rings[0], 100);

		/*
		 * Periodic sweep for rings that appeared with nothing asking
		 * for them (a producer starting while no client is connected).
		 * Requests open their ring on demand, so the sweep cadence
		 * only bounds idle discovery, not request latency.
		 */
		if (++jpeg_reconnect_tick >= 20) {
			jpeg_reconnect_tick = 0;
			for (int j = 0; j < RHD_MAX_JPEG; j++) {
				if (!srv->jpeg_rings[j]) {
					if (jpeg_ring_open_slot(srv, j) && ring_wanted[j]) {
						rss_ring_acquire(srv->jpeg_rings[j]);
						ring_acquired[j] = true;
					}
					continue;
				}
				const rss_ring_header_t *hdr =
					rss_ring_get_header(srv->jpeg_rings[j]);
				uint64_t ws = hdr->write_seq;
				if (ws == jpeg_last_ws[j])
					jpeg_idle[j]++;
				else
					jpeg_idle[j] = 0;
				jpeg_last_ws[j] = ws;
				/*
				 * A parked snapshot holds the ring open. The
				 * idle counter is already at its threshold when
				 * such a request arrives — that is what made the
				 * encoder idle in the first place — so without
				 * this the very next tick closes the ring out
				 * from under a request that is about to be
				 * served. The parked request has its own
				 * deadline, so this defers the close by seconds
				 * at most.
				 */
				if (jpeg_idle[j] >= 10 &&
				    !snap_waiting_on(srv, j)) { /* ~20s (10 ticks * 2s/tick) */
					RSS_DEBUG("jpeg ring idle, closing (%s)",
						  jpeg_ring_names[j]);
					if (ring_acquired[j]) {
						rss_ring_release(srv->jpeg_rings[j]);
						ring_acquired[j] = false;
					}
					rss_ring_close(srv->jpeg_rings[j]);
					srv->jpeg_rings[j] = NULL;
					jpeg_idle[j] = 0;
				}
			}
		}
	}

	/* Cleanup */
	for (int i = srv->client_count - 1; i >= 0; i--)
		remove_client(srv, i);

	free(srv->frame_buf);
	free(srv->snap_buf);
	for (int j = 0; j < RHD_MAX_JPEG; j++) {
		if (srv->jpeg_rings[j]) {
			if (ring_acquired[j])
				rss_ring_release(srv->jpeg_rings[j]);
			rss_ring_close(srv->jpeg_rings[j]);
		}
	}
	if (srv->audio_ring)
		rss_ring_close(srv->audio_ring);
	if (srv->ctrl)
		rss_ctrl_destroy(srv->ctrl);
	close(srv->listen_fd);
	close(srv->epoll_fd);
}

/* ── Entry point ── */

int main(int argc, char **argv)
{
	rss_daemon_ctx_t ctx;
	int ret = rss_daemon_init(&ctx, "rhd", argc, argv, NULL);
	if (ret != 0)
		return ret < 0 ? 1 : 0;
	rss_base64_init();

	if (!rss_config_get_bool(ctx.cfg, "http", "enabled", true)) {
		RSS_INFO("HTTP disabled in config");
		rss_config_free(ctx.cfg);
		rss_daemon_cleanup("rhd");
		return 0;
	}

	rhd_server_t srv = {0};
	srv.cfg = ctx.cfg;
	srv.config_path = ctx.config_path;
	srv.running = ctx.running;
	srv.port = rss_config_get_int(ctx.cfg, "http", "port", 8080);
	srv.max_clients = rss_config_get_int(ctx.cfg, "http", "max_clients", RHD_MAX_CLIENTS);
	if (srv.max_clients < 1)
		srv.max_clients = 1;
	if (srv.max_clients > RHD_MAX_CLIENTS)
		srv.max_clients = RHD_MAX_CLIENTS;

	/* JPEG capture-time EXIF + snapshot signing (device key shared with RMR) */
	srv.exif_timestamp = rss_config_get_bool(ctx.cfg, "http", "exif_timestamp", true);
	if (rss_config_get_bool(ctx.cfg, "http", "sign_snapshots", true)) {
		const char *key_path = rss_config_get_str(ctx.cfg, "recording", "sign_key",
							  "/etc/raptor/sign_ed25519.key");
		if (rss_sign_key_load(&srv.sign_key, key_path) < 0)
			RSS_ERROR("snapshot signing disabled: key unavailable");
		else
			srv.sign_ok = true;
	}

	/* Basic auth — enabled when both username and password are set */
	const char *http_user = rss_config_get_str(ctx.cfg, "http", "username", "");
	const char *http_pass = rss_config_get_str(ctx.cfg, "http", "password", "");
	if (http_user[0] && http_pass[0]) {
		rss_strlcpy(srv.auth_user, http_user, sizeof(srv.auth_user));
		rss_strlcpy(srv.auth_pass, http_pass, sizeof(srv.auth_pass));
		RSS_INFO("HTTP Basic auth enabled");
	}

#ifdef RSS_HAS_TLS
	bool https = rss_config_get_bool(ctx.cfg, "http", "https", false);
	if (!https && http_user[0])
		RSS_WARN("HTTP Basic auth without TLS -- credentials sent in plaintext");
	if (https) {
		const char *cert =
			rss_config_get_str(ctx.cfg, "http", "cert", "/etc/ssl/certs/uhttpd.crt");
		const char *key =
			rss_config_get_str(ctx.cfg, "http", "key", "/etc/ssl/private/uhttpd.key");
		srv.tls = rss_tls_init(cert, key);
		if (srv.tls)
			RSS_INFO("HTTPS enabled");
		else
			RSS_WARN("HTTPS init failed, falling back to HTTP");
	}
#else
	if (rss_config_get_bool(ctx.cfg, "http", "https", false))
		RSS_WARN("config requests https but rhd was built without TLS; serving plain HTTP");
	if (http_user[0])
		RSS_WARN("HTTP Basic auth without TLS -- credentials sent in plaintext");
#endif

	if (server_init(&srv) < 0)
		goto cleanup;

	server_run(&srv);

	RSS_INFO("rhd shutting down");

cleanup:
	free(index_html);
#ifdef RSS_HAS_TLS
	rss_tls_free(srv.tls);
#endif
	rss_config_free(ctx.cfg);
	rss_daemon_cleanup("rhd");
	return 0;
}
