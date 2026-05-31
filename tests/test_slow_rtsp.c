/*
 * Slow/stalled RTSP client for testing RSD under backpressure.
 *
 * Performs a TCP-interleaved RTSP handshake, then reads at a controlled
 * rate (or stops reading entirely) to exercise RSD's sendq overflow,
 * client timeout, and multi-client isolation paths.
 *
 * Modes:
 *   slow <bps>   Read at <bps> bytes/sec (e.g. 1000 = 1KB/s)
 *   stall <sec>  Read normally for <sec> seconds, then stop reading
 *   drop  <sec>  Read normally for <sec> seconds, then close() abruptly
 *
 * Usage:
 *   test_slow_rtsp [-p port] [-s stream] <mode> [arg]
 *
 * Exit codes:
 *   0  Server disconnected us (expected for stall/slow)
 *   1  Error (handshake failed, connect refused, etc.)
 *   2  Timed out (no server disconnect within deadline)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <time.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <poll.h>

static volatile sig_atomic_t running = 1;

static void on_signal(int sig)
{
	(void)sig;
	running = 0;
}

static int64_t now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int tcp_connect(const char *host, int port)
{
	int fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0)
		return -1;

	struct sockaddr_in addr = {
		.sin_family = AF_INET,
		.sin_port = htons(port),
	};
	inet_pton(AF_INET, host, &addr.sin_addr);

	int one = 1;
	setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

	struct timeval tv = { .tv_sec = 5 };
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

	if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		close(fd);
		return -1;
	}
	return fd;
}

static int rtsp_send(int fd, const char *req)
{
	size_t len = strlen(req);
	ssize_t n = send(fd, req, len, 0);
	return (n == (ssize_t)len) ? 0 : -1;
}

static int rtsp_recv(int fd, char *buf, size_t size)
{
	size_t total = 0;
	while (total < size - 1) {
		ssize_t n = recv(fd, buf + total, size - 1 - total, 0);
		if (n <= 0)
			return (total > 0) ? (int)total : -1;
		total += n;
		buf[total] = '\0';
		if (strstr(buf, "\r\n\r\n"))
			return (int)total;
	}
	return (int)total;
}

static int rtsp_handshake(int fd, int port, const char *stream)
{
	char req[512], resp[4096];
	int cseq = 1;

	snprintf(req, sizeof(req),
		"OPTIONS rtsp://127.0.0.1:%d/%s RTSP/1.0\r\n"
		"CSeq: %d\r\n\r\n", port, stream, cseq++);
	if (rtsp_send(fd, req) < 0)
		return -1;
	if (rtsp_recv(fd, resp, sizeof(resp)) < 0)
		return -1;
	if (!strstr(resp, "200"))
		return -1;

	snprintf(req, sizeof(req),
		"DESCRIBE rtsp://127.0.0.1:%d/%s RTSP/1.0\r\n"
		"CSeq: %d\r\n"
		"Accept: application/sdp\r\n\r\n", port, stream, cseq++);
	if (rtsp_send(fd, req) < 0)
		return -1;
	if (rtsp_recv(fd, resp, sizeof(resp)) < 0)
		return -1;
	if (!strstr(resp, "200"))
		return -1;

	/* Extract Content-Length and drain SDP body */
	char *cl = strstr(resp, "Content-Length:");
	if (cl) {
		int sdp_len = atoi(cl + 15);
		char *body = strstr(resp, "\r\n\r\n");
		if (body) {
			body += 4;
			int have = (int)strlen(body);
			while (have < sdp_len) {
				ssize_t n = recv(fd, resp, sizeof(resp) - 1, 0);
				if (n <= 0)
					break;
				have += n;
			}
		}
	}

	snprintf(req, sizeof(req),
		"SETUP rtsp://127.0.0.1:%d/%s/trackID=0 RTSP/1.0\r\n"
		"CSeq: %d\r\n"
		"Transport: RTP/AVP/TCP;unicast;interleaved=0-1\r\n\r\n",
		port, stream, cseq++);
	if (rtsp_send(fd, req) < 0)
		return -1;
	if (rtsp_recv(fd, resp, sizeof(resp)) < 0)
		return -1;
	if (!strstr(resp, "200"))
		return -1;

	char *sess = strstr(resp, "Session:");
	char session_id[64] = "";
	if (sess) {
		sess += 8;
		while (*sess == ' ')
			sess++;
		int i = 0;
		while (*sess && *sess != ';' && *sess != '\r' && i < 63)
			session_id[i++] = *sess++;
		session_id[i] = '\0';
	}

	snprintf(req, sizeof(req),
		"PLAY rtsp://127.0.0.1:%d/%s RTSP/1.0\r\n"
		"CSeq: %d\r\n"
		"Session: %s\r\n"
		"Range: npt=0.000-\r\n\r\n",
		port, stream, cseq++, session_id);
	if (rtsp_send(fd, req) < 0)
		return -1;
	if (rtsp_recv(fd, resp, sizeof(resp)) < 0)
		return -1;
	if (!strstr(resp, "200"))
		return -1;

	return 0;
}

enum mode { MODE_SLOW, MODE_STALL, MODE_DROP };

static void drain_at_rate(int fd, int bytes_per_sec, int deadline_sec)
{
	char buf[4096];
	int64_t start = now_ms();
	int64_t deadline = start + (int64_t)deadline_sec * 1000;
	int64_t total = 0;
	int chunk = (bytes_per_sec < (int)sizeof(buf)) ? bytes_per_sec : (int)sizeof(buf);
	if (chunk < 1)
		chunk = 1;

	while (running && now_ms() < deadline) {
		struct pollfd pfd = { .fd = fd, .events = POLLIN };
		int pr = poll(&pfd, 1, 200);
		if (pr < 0)
			break;
		if (pr == 0)
			continue;
		if (pfd.revents & (POLLERR | POLLHUP)) {
			fprintf(stderr, "server disconnected (poll)\n");
			return;
		}
		ssize_t n = recv(fd, buf, chunk, 0);
		if (n <= 0) {
			fprintf(stderr, "server disconnected (recv=%zd errno=%d)\n", n, errno);
			return;
		}
		total += n;

		/* Throttle: sleep to maintain target rate */
		int64_t elapsed = now_ms() - start;
		if (elapsed > 0) {
			int64_t expected_ms = (total * 1000) / bytes_per_sec;
			int64_t ahead = expected_ms - elapsed;
			if (ahead > 10) {
				struct timespec ts = {
					.tv_sec = ahead / 1000,
					.tv_nsec = (ahead % 1000) * 1000000,
				};
				nanosleep(&ts, NULL);
			}
		}
	}
	fprintf(stderr, "deadline reached, total=%lld bytes\n", (long long)total);
}

static void drain_then_stall(int fd, int active_sec, int deadline_sec)
{
	char buf[8192];
	int64_t start = now_ms();
	int64_t active_end = start + (int64_t)active_sec * 1000;
	int64_t deadline = start + (int64_t)deadline_sec * 1000;
	int64_t total = 0;

	/* Phase 1: read normally */
	while (running && now_ms() < active_end) {
		struct pollfd pfd = { .fd = fd, .events = POLLIN };
		int pr = poll(&pfd, 1, 200);
		if (pr <= 0)
			continue;
		if (pfd.revents & (POLLERR | POLLHUP)) {
			fprintf(stderr, "server disconnected during active phase\n");
			return;
		}
		ssize_t n = recv(fd, buf, sizeof(buf), 0);
		if (n <= 0) {
			fprintf(stderr, "server disconnected during active phase\n");
			return;
		}
		total += n;
	}
	fprintf(stderr, "active phase done, read %lld bytes in %ds, now stalling\n",
		(long long)total, active_sec);

	/* Phase 2: stop reading, wait for server to disconnect or deadline */
	while (running && now_ms() < deadline) {
		struct pollfd pfd = { .fd = fd, .events = POLLIN };
		int pr = poll(&pfd, 1, 500);
		if (pr < 0)
			break;
		if (pr > 0 && (pfd.revents & (POLLERR | POLLHUP))) {
			fprintf(stderr, "server disconnected after stall\n");
			return;
		}
		/* Don't recv -- that's the point */
	}
	fprintf(stderr, "deadline reached, server did NOT disconnect\n");
}

static void drain_then_drop(int fd, int active_sec)
{
	char buf[8192];
	int64_t start = now_ms();
	int64_t active_end = start + (int64_t)active_sec * 1000;
	int64_t total = 0;

	while (running && now_ms() < active_end) {
		struct pollfd pfd = { .fd = fd, .events = POLLIN };
		int pr = poll(&pfd, 1, 200);
		if (pr <= 0)
			continue;
		if (pfd.revents & (POLLERR | POLLHUP))
			return;
		ssize_t n = recv(fd, buf, sizeof(buf), 0);
		if (n <= 0)
			return;
		total += n;
	}
	fprintf(stderr, "dropping connection after %lld bytes in %ds\n",
		(long long)total, active_sec);
	/* Abrupt close -- no TEARDOWN, no FIN */
	struct linger lg = { .l_onoff = 1, .l_linger = 0 };
	setsockopt(fd, SOL_SOCKET, SO_LINGER, &lg, sizeof(lg));
}

static void usage(const char *prog)
{
	fprintf(stderr,
		"Usage: %s [-p port] [-s stream] [-d deadline] <mode> [arg]\n"
		"Modes:\n"
		"  slow <bps>   Read at <bps> bytes/sec\n"
		"  stall <sec>  Read normally for <sec>s, then stop\n"
		"  drop <sec>   Read normally for <sec>s, then RST\n",
		prog);
}

int main(int argc, char **argv)
{
	int port = 15554;
	const char *stream = "stream0";
	int deadline = 30;
	int opt;

	while ((opt = getopt(argc, argv, "p:s:d:h")) != -1) {
		switch (opt) {
		case 'p': port = atoi(optarg); break;
		case 's': stream = optarg; break;
		case 'd': deadline = atoi(optarg); break;
		default: usage(argv[0]); return 1;
		}
	}

	if (optind >= argc) {
		usage(argv[0]);
		return 1;
	}

	const char *mode_str = argv[optind];
	int mode_arg = (optind + 1 < argc) ? atoi(argv[optind + 1]) : 0;

	enum mode mode;
	if (strcmp(mode_str, "slow") == 0) {
		mode = MODE_SLOW;
		if (mode_arg <= 0)
			mode_arg = 1000;
	} else if (strcmp(mode_str, "stall") == 0) {
		mode = MODE_STALL;
		if (mode_arg <= 0)
			mode_arg = 2;
	} else if (strcmp(mode_str, "drop") == 0) {
		mode = MODE_DROP;
		if (mode_arg <= 0)
			mode_arg = 2;
	} else {
		usage(argv[0]);
		return 1;
	}

	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);
	signal(SIGPIPE, SIG_IGN);

	fprintf(stderr, "connecting to 127.0.0.1:%d/%s\n", port, stream);
	int fd = tcp_connect("127.0.0.1", port);
	if (fd < 0) {
		perror("connect");
		return 1;
	}

	fprintf(stderr, "RTSP handshake...\n");
	if (rtsp_handshake(fd, port, stream) < 0) {
		fprintf(stderr, "handshake failed\n");
		close(fd);
		return 1;
	}
	fprintf(stderr, "PLAY ok, mode=%s arg=%d deadline=%ds\n",
		mode_str, mode_arg, deadline);

	switch (mode) {
	case MODE_SLOW:
		drain_at_rate(fd, mode_arg, deadline);
		break;
	case MODE_STALL:
		drain_then_stall(fd, mode_arg, deadline);
		break;
	case MODE_DROP:
		drain_then_drop(fd, mode_arg);
		break;
	}

	close(fd);
	return 0;
}
