/*
 * rsd_net.h -- socket calls the event loop may make
 */

#ifndef RSD_NET_H
#define RSD_NET_H

#include <sys/socket.h>
#include <sys/types.h>

/* Receive on the event loop's thread. An epoll event is a hint about an fd
 * number, not a promise about the socket behind it now: a handler earlier in
 * the same batch can close it and SETUP can hand the number to a new socket
 * that is still blocking. The loop must come back either way. */
static inline ssize_t rsd_recv_nowait(int fd, void *buf, size_t len)
{
	return recv(fd, buf, len, MSG_DONTWAIT);
}

#endif
