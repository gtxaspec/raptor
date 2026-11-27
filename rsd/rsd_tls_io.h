/*
 * rsd_tls_io.h -- deciding when a TLS read cannot wait
 *
 * compy's TLS read blocks on the socket for the rest of a record, and
 * mbedTLS keeps reading when a record turns out to carry no data. A peer
 * that sends half a record therefore parks whoever called it. The event
 * loop reads only when the socket already holds the whole next record.
 */

#ifndef RSD_TLS_IO_H
#define RSD_TLS_IO_H

#include <errno.h>
#include <stddef.h>
#include <sys/ioctl.h>
#include <sys/socket.h>

/* Largest record body a peer may send (RFC 8446 5.2: 2^14 plus expansion) */
#define RSD_TLS_MAX_BODY (16384 + 2048)

/* Bytes of the record starting at hdr (its 5-byte header included), or -1
 * when the header is not a TLS record header. */
static inline int rsd_tls_record_size(const unsigned char hdr[5])
{
	if (hdr[0] < 20 || hdr[0] > 24 || hdr[1] != 3)
		return -1;
	size_t body = ((size_t)hdr[3] << 8) | hdr[4];
	if (body > RSD_TLS_MAX_BODY)
		return -1;
	return (int)(5 + body);
}

/* 1: the next whole record is queued on fd, so a read will not wait.
 * 0: it is not here yet. -1: what is queued is not a TLS record, or the peer
 * has gone. */
static inline int rsd_tls_record_ready(int fd)
{
	int avail = 0;
	unsigned char hdr[5];

	if (ioctl(fd, FIONREAD, &avail) < 0)
		return -1;
	if (avail < 5) {
		if (avail > 0)
			return 0;
		/* nothing queued: still open, or finished? */
		ssize_t n = recv(fd, hdr, 1, MSG_PEEK | MSG_DONTWAIT);
		return (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) ? -1 : 0;
	}
	if (recv(fd, hdr, sizeof(hdr), MSG_PEEK | MSG_DONTWAIT) != (ssize_t)sizeof(hdr))
		return 0;
	int need = rsd_tls_record_size(hdr);
	if (need < 0)
		return -1;
	return avail >= need;
}

#endif
