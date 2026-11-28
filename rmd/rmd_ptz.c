/*
 * rmd_ptz.c -- the PTZ head for rmd's tracker: thingino-motors' daemon socket
 *
 * motors-daemon takes one request a connection on its socket (/dev/md by default,
 * [track] motors_socket): a struct request, answered by a
 * struct motor_message for the status commands and by nothing for a move. The two structs
 * below are its wire format.
 */

#include <errno.h>
#include <limits.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>

#include "rmd.h"

struct motors_request {
	char command; /* 'd' move, 'b' busy */
	char type;    /* with 'd': 'g' relative, 'b' go back */
	int x;
	int got_x;
	int y;
	int got_y;
	int speed; /* 0 = the daemon's current speed */
};

struct motors_message {
	int x;
	int y;
	int status; /* 0 stopped, 1 running */
	int speed;
	unsigned int x_max_steps;
	unsigned int y_max_steps;
	unsigned int inversion_state;
};

static int motors_send(const char *sock, const struct motors_request *req,
		       struct motors_message *reply)
{
	int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (fd < 0)
		return -1;
	struct timeval tv = {.tv_sec = 1};
	setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

	struct sockaddr_un addr = {.sun_family = AF_UNIX};
	rss_strlcpy(addr.sun_path, sock, sizeof(addr.sun_path));
	int ret = -1;
	if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0 &&
	    write(fd, req, sizeof(*req)) == (ssize_t)sizeof(*req)) {
		if (!reply)
			ret = 0;
		else if (read(fd, reply, sizeof(*reply)) == (ssize_t)sizeof(*reply))
			ret = 0;
	}
	close(fd);
	return ret;
}

int rmd_ptz_move(const char *sock, int dx, int dy, int speed)
{
	struct motors_request req = {.command = 'd',
				     .type = 'g',
				     .x = dx,
				     .got_x = 1,
				     .y = dy,
				     .got_y = 1,
				     .speed = speed};
	int ret = motors_send(sock, &req, NULL);
	if (ret < 0)
		RSS_WARN("track: move %d,%d not sent (motors-daemon: %s)", dx, dy, strerror(errno));
	return ret;
}

int rmd_ptz_busy(const char *sock)
{
	struct motors_request req = {.command = 'b'};
	struct motors_message msg = {0};
	if (motors_send(sock, &req, &msg) < 0)
		return -1;
	return msg.status == 1;
}

int rmd_ptz_limits(const char *sock, int *x_max, int *y_max)
{
	struct motors_request req = {.command = 'b'};
	struct motors_message msg = {0};
	if (motors_send(sock, &req, &msg) < 0)
		return -1;
	*x_max = msg.x_max_steps > (unsigned)INT_MAX ? INT_MAX : (int)msg.x_max_steps;
	*y_max = msg.y_max_steps > (unsigned)INT_MAX ? INT_MAX : (int)msg.y_max_steps;
	return 0;
}

int rmd_ptz_go_back(const char *sock)
{
	struct motors_request req = {.command = 'd', .type = 'b'};
	int ret = motors_send(sock, &req, NULL);
	if (ret < 0)
		RSS_WARN("track: go back not sent (motors-daemon: %s)", strerror(errno));
	return ret;
}
