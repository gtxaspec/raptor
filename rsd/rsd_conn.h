/*
 * rsd_conn.h -- taking a connection's write_lock from the event loop
 */

#ifndef RSD_CONN_H
#define RSD_CONN_H

#include <poll.h>
#include <pthread.h>
#include <stdbool.h>

/* True when a write of a small control message would not wait. POLLERR and
 * POLLHUP count: the write fails at once, which is not waiting either. */
static inline bool rsd_fd_writable(int fd)
{
	struct pollfd p = {.fd = fd, .events = POLLOUT};
	return poll(&p, 1, 0) > 0 && (p.revents & (POLLOUT | POLLERR | POLLHUP)) != 0;
}

/* The event loop must never wait on a client: the lock's owner can be the
 * send thread parked in a write to a player that stopped reading, and a
 * socket whose buffer is full would park the loop itself in the response.
 * False means "not now"; the caller keeps the request and tries again. */
static inline bool rsd_control_lock(pthread_mutex_t *lock, int fd)
{
	if (pthread_mutex_trylock(lock) != 0)
		return false;
	if (!rsd_fd_writable(fd)) {
		pthread_mutex_unlock(lock);
		return false;
	}
	return true;
}

#endif
