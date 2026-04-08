/*
 * rlatency.c -- RTSP end-to-end latency measurement tool
 *
 * Connects to an RTSP server over UDP, receives RTP video packets and
 * RTCP Sender Reports, then computes per-frame latency by mapping RTP
 * timestamps to NTP wall-clock time via the SR correlation.
 *
 * Requires NTP-synchronized clocks on both camera and measurement host.
 *
 * Usage:
 *   rlatency rtsp://camera/stream0
 *   rlatency rtsp://camera/stream0 -n 200
 *   rlatency rtsp://camera/stream0 -v         (verbose: per-frame output)
 *
 * Build:
 *   cc -O2 -Wall -Wextra -o rlatency rlatency.c
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <inttypes.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <math.h>
#include <getopt.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <poll.h>

/* ── NTP time helpers ── */

/* NTP epoch: 1900-01-01 00:00:00 UTC.
 * Unix epoch offset: 70 years of seconds. */
#define NTP_EPOCH_OFFSET 2208988800ULL

/* Convert NTP 64-bit timestamp (32.32 fixed point) to microseconds since Unix epoch */
static int64_t ntp_to_us(uint32_t ntp_sec, uint32_t ntp_frac)
{
	int64_t sec = (int64_t)ntp_sec - (int64_t)NTP_EPOCH_OFFSET;
	int64_t frac_us = ((int64_t)ntp_frac * 1000000LL) >> 32;
	return sec * 1000000LL + frac_us;
}

/* Get current time as microseconds since Unix epoch */
static int64_t wall_clock_us(void)
{
	struct timeval tv;
	gettimeofday(&tv, NULL);
	return (int64_t)tv.tv_sec * 1000000LL + tv.tv_usec;
}

/* ── RTCP Sender Report parsing ── */

typedef struct {
	int64_t ntp_us;	 /* NTP timestamp converted to Unix µs */
	uint32_t rtp_ts; /* corresponding RTP timestamp */
	bool valid;
} sr_info_t;

/*
 * Parse RTCP Sender Report from raw packet.
 * Returns true if a valid SR was found.
 *
 * RTCP SR format (RFC 3550 §6.4.1):
 *   byte 0:    V=2, P, RC (5 bits)
 *   byte 1:    PT = 200
 *   bytes 2-3: length (32-bit words - 1)
 *   bytes 4-7: SSRC
 *   bytes 8-11:  NTP seconds
 *   bytes 12-15: NTP fraction
 *   bytes 16-19: RTP timestamp
 */
static bool parse_rtcp_sr(const uint8_t *data, size_t len, sr_info_t *sr)
{
	if (len < 20)
		return false;

	/* Version must be 2, PT must be 200 (SR) */
	uint8_t version = (data[0] >> 6) & 0x03;
	uint8_t pt = data[1];
	if (version != 2 || pt != 200)
		return false;

	uint32_t ntp_sec = (uint32_t)data[8] << 24 | (uint32_t)data[9] << 16 |
			   (uint32_t)data[10] << 8 | (uint32_t)data[11];
	uint32_t ntp_frac = (uint32_t)data[12] << 24 | (uint32_t)data[13] << 16 |
			    (uint32_t)data[14] << 8 | (uint32_t)data[15];
	uint32_t rtp_ts = (uint32_t)data[16] << 24 | (uint32_t)data[17] << 16 |
			  (uint32_t)data[18] << 8 | (uint32_t)data[19];

	/* Sanity: NTP timestamp should be after 2020 */
	if (ntp_sec < NTP_EPOCH_OFFSET + 1577836800ULL)
		return false;

	sr->ntp_us = ntp_to_us(ntp_sec, ntp_frac);
	sr->rtp_ts = rtp_ts;
	sr->valid = true;
	return true;
}

/* ── RTP header parsing ── */

typedef struct {
	uint32_t ts;
	uint16_t seq;
	uint8_t pt;
	bool marker;
} rtp_header_t;

/*
 * Parse RTP header. Returns payload offset, or -1 on error.
 *
 * RTP header (RFC 3550 §5.1):
 *   byte 0:    V=2, P, X, CC (4 bits)
 *   byte 1:    M, PT (7 bits)
 *   bytes 2-3: sequence number
 *   bytes 4-7: timestamp
 *   bytes 8-11: SSRC
 *   + CSRC list (CC * 4 bytes)
 *   + extension header (if X)
 */
static int parse_rtp_header(const uint8_t *data, size_t len, rtp_header_t *hdr)
{
	if (len < 12)
		return -1;

	uint8_t version = (data[0] >> 6) & 0x03;
	if (version != 2)
		return -1;

	uint8_t cc = data[0] & 0x0F;
	bool extension = (data[0] >> 4) & 0x01;

	hdr->marker = (data[1] >> 7) & 0x01;
	hdr->pt = data[1] & 0x7F;
	hdr->seq = (uint16_t)data[2] << 8 | data[3];
	hdr->ts = (uint32_t)data[4] << 24 | (uint32_t)data[5] << 16 | (uint32_t)data[6] << 8 |
		  data[7];

	int offset = 12 + cc * 4;
	if (extension && (size_t)offset + 4 <= len) {
		uint16_t ext_len = (uint16_t)data[offset + 2] << 8 | data[offset + 3];
		offset += 4 + ext_len * 4;
	}

	return ((size_t)offset <= len) ? offset : -1;
}

/* ── Simple RTSP client (just enough for DESCRIBE → SETUP → PLAY) ── */

typedef struct {
	int tcp_fd;   /* RTSP control connection */
	int rtp_fd;   /* UDP RTP socket */
	int rtcp_fd;  /* UDP RTCP socket */
	int rtp_port; /* local RTP port */
	int cseq;
	char session[128];
	uint32_t rtp_clock; /* from SDP (90000 for video) */
} rtsp_ctx_t;

/* Read full RTSP response into buffer. Returns bytes read, -1 on error. */
static int rtsp_recv(int fd, char *buf, size_t buflen, int timeout_ms)
{
	size_t total = 0;
	while (total < buflen - 1) {
		struct pollfd pfd = {.fd = fd, .events = POLLIN};
		int ret = poll(&pfd, 1, timeout_ms);
		if (ret <= 0)
			break;
		ssize_t n = read(fd, buf + total, buflen - 1 - total);
		if (n <= 0)
			break;
		total += (size_t)n;
		buf[total] = '\0';
		/* RTSP response ends with \r\n\r\n (no body for our methods) */
		if (strstr(buf, "\r\n\r\n"))
			break;
	}
	return (total > 0) ? (int)total : -1;
}

/* Send RTSP request. Returns 0 on success. */
static int rtsp_send(rtsp_ctx_t *ctx, const char *method, const char *url,
		     const char *extra_headers)
{
	char req[2048];
	int len = snprintf(req, sizeof(req),
			   "%s %s RTSP/1.0\r\n"
			   "CSeq: %d\r\n"
			   "%s%s"
			   "\r\n",
			   method, url, ++ctx->cseq, ctx->session[0] ? "Session: " : "",
			   ctx->session[0] ? ctx->session : "");
	if (extra_headers) {
		/* Insert extra headers before final \r\n */
		int pos = len - 2; /* before trailing \r\n */
		int elen = (int)strlen(extra_headers);
		if (pos + elen + 2 < (int)sizeof(req)) {
			memmove(req + pos + elen, req + pos, len - pos + 1);
			memcpy(req + pos, extra_headers, elen);
			len += elen;
		}
	}
	return (write(ctx->tcp_fd, req, len) == len) ? 0 : -1;
}

/* Parse "Session: <id>" from response */
static void parse_session(const char *resp, char *session, size_t session_size)
{
	const char *s = strstr(resp, "Session:");
	if (!s)
		s = strstr(resp, "session:");
	if (!s)
		return;
	s += 8;
	while (*s == ' ')
		s++;
	size_t i = 0;
	while (i < session_size - 1 && *s && *s != '\r' && *s != '\n' && *s != ';')
		session[i++] = *s++;
	session[i] = '\0';
}

/* Parse response status code */
static int parse_status(const char *resp)
{
	int code = 0;
	if (sscanf(resp, "RTSP/1.0 %d", &code) == 1)
		return code;
	return -1;
}

/* Create bound UDP socket pair for RTP + RTCP */
static int create_udp_pair(int *rtp_fd, int *rtcp_fd, int *rtp_port)
{
	/* Try ports in the range 50000-60000 */
	for (int port = 50000; port < 60000; port += 2) {
		int fd1 = socket(AF_INET, SOCK_DGRAM, 0);
		int fd2 = socket(AF_INET, SOCK_DGRAM, 0);
		if (fd1 < 0 || fd2 < 0) {
			if (fd1 >= 0)
				close(fd1);
			if (fd2 >= 0)
				close(fd2);
			continue;
		}

		struct sockaddr_in addr = {
			.sin_family = AF_INET,
			.sin_addr.s_addr = INADDR_ANY,
		};

		addr.sin_port = htons((uint16_t)port);
		if (bind(fd1, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
			close(fd1);
			close(fd2);
			continue;
		}
		addr.sin_port = htons((uint16_t)(port + 1));
		if (bind(fd2, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
			close(fd1);
			close(fd2);
			continue;
		}

		*rtp_fd = fd1;
		*rtcp_fd = fd2;
		*rtp_port = port;
		return 0;
	}
	return -1;
}

/* Connect to RTSP server and start receiving */
static int rtsp_connect(rtsp_ctx_t *ctx, const char *url)
{
	/* Parse URL: rtsp://host[:port]/path */
	char host[256] = {0};
	int port = 554;
	char path[512] = {0};

	const char *p = url;
	if (strncmp(p, "rtsp://", 7) != 0) {
		fprintf(stderr, "Invalid URL (must start with rtsp://)\n");
		return -1;
	}
	p += 7;

	/* Handle IPv6 [addr]:port */
	if (*p == '[') {
		const char *end = strchr(p, ']');
		if (!end)
			return -1;
		size_t hlen = (size_t)(end - p - 1);
		if (hlen >= sizeof(host))
			return -1;
		memcpy(host, p + 1, hlen);
		p = end + 1;
		if (*p == ':')
			port = (int)strtol(p + 1, (char **)&p, 10);
	} else {
		const char *colon = strchr(p, ':');
		const char *slash = strchr(p, '/');
		if (colon && (!slash || colon < slash)) {
			size_t hlen = (size_t)(colon - p);
			if (hlen >= sizeof(host))
				return -1;
			memcpy(host, p, hlen);
			port = (int)strtol(colon + 1, (char **)&p, 10);
		} else if (slash) {
			size_t hlen = (size_t)(slash - p);
			if (hlen >= sizeof(host))
				return -1;
			memcpy(host, p, hlen);
			p = slash;
		} else {
			snprintf(host, sizeof(host), "%s", p);
			p = p + strlen(p);
		}
	}
	if (*p == '/')
		snprintf(path, sizeof(path), "%s", p);
	else
		snprintf(path, sizeof(path), "/");

	/* Resolve and connect TCP */
	struct addrinfo hints = {.ai_family = AF_UNSPEC, .ai_socktype = SOCK_STREAM};
	struct addrinfo *res;
	char port_str[16];
	snprintf(port_str, sizeof(port_str), "%d", port);

	if (getaddrinfo(host, port_str, &hints, &res) != 0) {
		fprintf(stderr, "Cannot resolve %s\n", host);
		return -1;
	}

	ctx->tcp_fd = -1;
	for (struct addrinfo *rp = res; rp; rp = rp->ai_next) {
		ctx->tcp_fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
		if (ctx->tcp_fd < 0)
			continue;
		struct timeval tv = {.tv_sec = 5};
		setsockopt(ctx->tcp_fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
		setsockopt(ctx->tcp_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
		if (connect(ctx->tcp_fd, rp->ai_addr, rp->ai_addrlen) == 0)
			break;
		close(ctx->tcp_fd);
		ctx->tcp_fd = -1;
	}
	freeaddrinfo(res);

	if (ctx->tcp_fd < 0) {
		fprintf(stderr, "Cannot connect to %s:%d\n", host, port);
		return -1;
	}

	char resp[4096];

	/* OPTIONS */
	if (rtsp_send(ctx, "OPTIONS", url, NULL) < 0)
		return -1;
	if (rtsp_recv(ctx->tcp_fd, resp, sizeof(resp), 5000) < 0)
		return -1;

	/* DESCRIBE — has SDP body, need Content-Length-aware read */
	if (rtsp_send(ctx, "DESCRIBE", url, "Accept: application/sdp\r\n") < 0)
		return -1;
	{
		size_t total = 0;
		for (int i = 0; i < 50 && total < sizeof(resp) - 1; i++) {
			struct pollfd pfd = {.fd = ctx->tcp_fd, .events = POLLIN};
			if (poll(&pfd, 1, 3000) <= 0)
				break;
			ssize_t nr = read(ctx->tcp_fd, resp + total, sizeof(resp) - 1 - total);
			if (nr <= 0)
				break;
			total += (size_t)nr;
			resp[total] = '\0';
			const char *hdr_end = strstr(resp, "\r\n\r\n");
			if (hdr_end) {
				const char *cl = strstr(resp, "Content-Length:");
				if (!cl)
					break; /* no body */
				int clen = (int)strtol(cl + 15, NULL, 10);
				size_t body_start = (size_t)(hdr_end + 4 - resp);
				if ((int)(total - body_start) >= clen)
					break;
			}
		}
		if (total == 0)
			return -1;
	}

	if (parse_status(resp) != 200) {
		fprintf(stderr, "DESCRIBE failed: %.40s\n", resp);
		return -1;
	}

	/* Parse RTP clock from SDP (default 90000 for video) */
	ctx->rtp_clock = 90000;
	const char *rtpmap = strstr(resp, "a=rtpmap:");
	if (rtpmap) {
		const char *slash = strchr(rtpmap, '/');
		if (slash) {
			long clk = strtol(slash + 1, NULL, 10);
			if (clk > 0)
				ctx->rtp_clock = (uint32_t)clk;
		}
	}

	/* Create UDP pair */
	if (create_udp_pair(&ctx->rtp_fd, &ctx->rtcp_fd, &ctx->rtp_port) < 0) {
		fprintf(stderr, "Cannot create UDP sockets\n");
		return -1;
	}

	/* Build control URL — find the video m-section's a=control.
	 * Look for "m=video" then the next "a=control:" within that section. */
	char control_url[1024];
	const char *video_section = strstr(resp, "m=video");
	const char *control = NULL;
	if (video_section) {
		control = strstr(video_section, "a=control:");
		/* Don't go past the next m= line */
		const char *next_m = strstr(video_section + 1, "\nm=");
		if (next_m && control > next_m)
			control = NULL;
	}
	if (control) {
		control += 10;
		char ctrl_path[256];
		int ci = 0;
		while (control[ci] && control[ci] != '\r' && control[ci] != '\n' &&
		       ci < (int)sizeof(ctrl_path) - 1)
			ci++;
		memcpy(ctrl_path, control, ci);
		ctrl_path[ci] = '\0';
		if (strncmp(ctrl_path, "rtsp://", 7) == 0)
			snprintf(control_url, sizeof(control_url), "%s", ctrl_path);
		else
			snprintf(control_url, sizeof(control_url), "%s/%s", url, ctrl_path);
	} else {
		snprintf(control_url, sizeof(control_url), "%s", url);
	}

	/* SETUP (UDP) */
	char transport[128];
	snprintf(transport, sizeof(transport), "Transport: RTP/AVP;unicast;client_port=%d-%d\r\n",
		 ctx->rtp_port, ctx->rtp_port + 1);
	if (rtsp_send(ctx, "SETUP", control_url, transport) < 0)
		return -1;
	if (rtsp_recv(ctx->tcp_fd, resp, sizeof(resp), 5000) < 0)
		return -1;
	if (parse_status(resp) != 200) {
		fprintf(stderr, "SETUP failed: %.40s\n", resp);
		return -1;
	}
	parse_session(resp, ctx->session, sizeof(ctx->session));

	/* PLAY */
	if (rtsp_send(ctx, "PLAY", url, "Range: nop=now-\r\n") < 0)
		return -1;
	if (rtsp_recv(ctx->tcp_fd, resp, sizeof(resp), 5000) < 0)
		return -1;
	if (parse_status(resp) != 200) {
		fprintf(stderr, "PLAY failed: %.40s\n", resp);
		return -1;
	}

	return 0;
}

static void rtsp_teardown(rtsp_ctx_t *ctx, const char *url)
{
	if (ctx->tcp_fd >= 0) {
		rtsp_send(ctx, "TEARDOWN", url, NULL);
		close(ctx->tcp_fd);
	}
	if (ctx->rtp_fd >= 0)
		close(ctx->rtp_fd);
	if (ctx->rtcp_fd >= 0)
		close(ctx->rtcp_fd);
}

/* ── Latency statistics ── */

typedef struct {
	int64_t min;
	int64_t max;
	int64_t sum;
	int64_t sum_sq; /* for standard deviation */
	uint64_t count;
	int64_t *samples; /* for percentile calculation */
	size_t samples_cap;
} lat_stats_t;

static void stats_init(lat_stats_t *s, size_t max_samples)
{
	memset(s, 0, sizeof(*s));
	s->min = INT64_MAX;
	s->max = INT64_MIN;
	if (max_samples > 0) {
		s->samples = calloc(max_samples, sizeof(int64_t));
		s->samples_cap = s->samples ? max_samples : 0;
	}
}

static void stats_add(lat_stats_t *s, int64_t value)
{
	if (value < s->min)
		s->min = value;
	if (value > s->max)
		s->max = value;
	s->sum += value;
	s->sum_sq += value * value;
	if (s->samples && s->count < s->samples_cap)
		s->samples[s->count] = value;
	s->count++;
}

static int cmp_i64(const void *a, const void *b)
{
	int64_t va = *(const int64_t *)a;
	int64_t vb = *(const int64_t *)b;
	return (va > vb) - (va < vb);
}

static void stats_print(const lat_stats_t *s)
{
	if (s->count == 0) {
		fprintf(stderr, "No samples collected\n");
		return;
	}

	double avg = (double)s->sum / (double)s->count;
	double variance = (double)s->sum_sq / (double)s->count - avg * avg;
	double stddev = (variance > 0) ? sqrt(variance) : 0;

	fprintf(stderr,
		"\n--- latency (%" PRIu64 " frames) ---\n"
		"  Min:    %7.2f ms\n"
		"  Avg:    %7.2f ms\n"
		"  Max:    %7.2f ms\n"
		"  StdDev: %7.2f ms\n",
		s->count, (double)s->min / 1000.0, avg / 1000.0, (double)s->max / 1000.0,
		stddev / 1000.0);

	/* Percentiles */
	if (s->samples && s->count > 1) {
		size_t n = (s->count < s->samples_cap) ? (size_t)s->count : s->samples_cap;
		int64_t *sorted = malloc(n * sizeof(int64_t));
		if (sorted) {
			memcpy(sorted, s->samples, n * sizeof(int64_t));
			qsort(sorted, n, sizeof(int64_t), cmp_i64);
			fprintf(stderr,
				"  P50:    %7.2f ms\n"
				"  P95:    %7.2f ms\n"
				"  P99:    %7.2f ms\n",
				(double)sorted[n * 50 / 100] / 1000.0,
				(double)sorted[n * 95 / 100] / 1000.0,
				(double)sorted[n * 99 / 100] / 1000.0);
			free(sorted);
		}
	}
}

static void stats_free(lat_stats_t *s)
{
	free(s->samples);
}

/* ── Main ── */

static volatile sig_atomic_t g_running = 1;

static void sighandler(int sig)
{
	(void)sig;
	g_running = 0;
}

static void usage(const char *prog)
{
	fprintf(stderr,
		"rlatency -- RTSP end-to-end latency measurement\n\n"
		"Usage: %s <rtsp_url> [options]\n\n"
		"Options:\n"
		"  -n <count>   Stop after <count> frames (default: unlimited)\n"
		"  -v           Verbose: print per-frame latency\n"
		"  -q           Quiet: only print summary\n"
		"  -h           Show this help\n\n"
		"Requires NTP-synchronized clocks on camera and host.\n\n"
		"Examples:\n"
		"  %s rtsp://10.25.30.110/stream0\n"
		"  %s rtsp://camera/stream0 -n 500 -v\n",
		prog, prog, prog);
}

int main(int argc, char **argv)
{
	if (argc < 2 || strcmp(argv[1], "-h") == 0 || argv[1][0] == '-') {
		usage(argv[0]);
		return (argc < 2) ? 1 : 0;
	}

	const char *url = argv[1];
	int max_frames = 0;
	bool verbose = false;
	bool quiet = false;

	int opt;
	optind = 2;
	while ((opt = getopt(argc, argv, "n:vqh")) != -1) {
		switch (opt) {
		case 'n':
			max_frames = (int)strtol(optarg, NULL, 10);
			break;
		case 'v':
			verbose = true;
			break;
		case 'q':
			quiet = true;
			break;
		case 'h':
			usage(argv[0]);
			return 0;
		default:
			usage(argv[0]);
			return 1;
		}
	}

	signal(SIGINT, sighandler);
	signal(SIGTERM, sighandler);

	/* Connect to RTSP server */
	rtsp_ctx_t ctx = {.tcp_fd = -1, .rtp_fd = -1, .rtcp_fd = -1};
	if (!quiet)
		fprintf(stderr, "Connecting to %s ...\n", url);

	if (rtsp_connect(&ctx, url) != 0) {
		fprintf(stderr, "RTSP connection failed\n");
		return 1;
	}

	if (!quiet)
		fprintf(stderr,
			"Playing (RTP port %d, clock %u Hz)\n"
			"Waiting for RTCP Sender Report...\n",
			ctx.rtp_port, ctx.rtp_clock);

	/* Receive loop */
	sr_info_t sr = {0};
	lat_stats_t stats;
	stats_init(&stats, max_frames > 0 ? (size_t)max_frames : 10000);

	uint32_t last_rtp_ts = 0;
	bool first_frame = true;
	uint64_t frame_count = 0;
	uint64_t rtp_packet_count = 0;

	uint8_t buf[2048];

	while (g_running) {
		struct pollfd pfds[2] = {
			{.fd = ctx.rtp_fd, .events = POLLIN},
			{.fd = ctx.rtcp_fd, .events = POLLIN},
		};

		int ret = poll(pfds, 2, 1000);
		if (ret <= 0)
			continue;

		/* RTCP (Sender Report) */
		if (pfds[1].revents & POLLIN) {
			ssize_t n = recv(ctx.rtcp_fd, buf, sizeof(buf), 0);
			if (n > 0) {
				sr_info_t new_sr;
				if (parse_rtcp_sr(buf, (size_t)n, &new_sr)) {
					sr = new_sr;
					if (!quiet && stats.count == 0)
						fprintf(stderr,
							"SR received (NTP offset established)\n");
				}
			}
		}

		/* RTP */
		if (pfds[0].revents & POLLIN) {
			ssize_t n = recv(ctx.rtp_fd, buf, sizeof(buf), 0);
			if (n <= 0)
				continue;

			rtp_header_t rtp;
			int payload_off = parse_rtp_header(buf, (size_t)n, &rtp);
			if (payload_off < 0)
				continue;

			rtp_packet_count++;

			/* Only measure on timestamp change (= new frame) */
			if (!first_frame && rtp.ts == last_rtp_ts)
				continue;
			last_rtp_ts = rtp.ts;
			first_frame = false;

			/* Need at least one SR to compute latency */
			if (!sr.valid)
				continue;

			/* Map RTP timestamp to NTP time using SR correlation:
			 *   ntp_time = sr.ntp_us + (rtp_ts - sr.rtp_ts) / clock * 1e6
			 *
			 * Use signed 32-bit difference for RTP timestamp wrap. */
			int32_t ts_diff = (int32_t)(rtp.ts - sr.rtp_ts);
			int64_t frame_ntp_us =
				sr.ntp_us + (int64_t)ts_diff * 1000000LL / (int64_t)ctx.rtp_clock;

			int64_t now = wall_clock_us();
			int64_t latency_us = now - frame_ntp_us;

			stats_add(&stats, latency_us);
			frame_count++;

			if (verbose)
				fprintf(stderr,
					"#%-6" PRIu64
					" lat=%7.2f ms  rtp_ts=%-10u  size=%-6zu  M=%d\n",
					frame_count, (double)latency_us / 1000.0, rtp.ts, (size_t)n,
					rtp.marker ? 1 : 0);
			else if (!quiet && frame_count % 25 == 0)
				fprintf(stderr,
					"\r  %" PRIu64 " frames, avg %.2f ms, "
					"min %.2f ms, max %.2f ms   ",
					stats.count,
					(double)stats.sum / (double)stats.count / 1000.0,
					(double)stats.min / 1000.0, (double)stats.max / 1000.0);

			if (max_frames > 0 && (int)frame_count >= max_frames)
				break;
		}
	}

	if (!quiet)
		fprintf(stderr, "\n");

	stats_print(&stats);
	fprintf(stderr, "  RTP packets: %" PRIu64 "\n", rtp_packet_count);

	stats_free(&stats);
	rtsp_teardown(&ctx, url);
	return 0;
}
