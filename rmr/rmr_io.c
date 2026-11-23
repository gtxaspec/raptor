/*
 * rmr_io.c -- recording file I/O on a writer thread
 */

#include "rmr_io.h"

#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/uio.h>
#include <unistd.h>

#define IO_OPS	   1024 /* queued operations (power of 2) */
#define IO_ERR_FDS 8	/* files tracked for errors at once */

enum { OP_WRITE, OP_PWRITE, OP_SYNC, OP_CLOSE };

typedef struct {
	int fd;
	int kind;
	uint32_t len;
	uint64_t off;
} io_op_t;

struct rmr_io {
	pthread_t tid;
	pthread_mutex_t lock;
	pthread_cond_t not_empty;
	pthread_cond_t not_full;
	pthread_cond_t idle;
	bool stop;
	bool busy; /* an operation is running outside the lock */

	io_op_t ops[IO_OPS];
	uint32_t op_rd, op_wr; /* monotonic */

	uint8_t *buf;
	uint32_t size;
	uint64_t rd, wr; /* monotonic byte positions */
	uint32_t peak;

	struct {
		int fd;
		int err;
	} errs[IO_ERR_FDS];
};

static int err_get(rmr_io_t *io, int fd)
{
	for (int i = 0; i < IO_ERR_FDS; i++)
		if (io->errs[i].err && io->errs[i].fd == fd)
			return io->errs[i].err;
	return 0;
}

static void err_set(rmr_io_t *io, int fd, int err)
{
	if (err_get(io, fd))
		return;
	for (int i = 0; i < IO_ERR_FDS; i++) {
		if (!io->errs[i].err) {
			io->errs[i].fd = fd;
			io->errs[i].err = err;
			return;
		}
	}
	io->errs[0].fd = fd; /* more failing files than slots: keep the newest */
	io->errs[0].err = err;
}

static void err_clear(rmr_io_t *io, int fd)
{
	for (int i = 0; i < IO_ERR_FDS; i++)
		if (io->errs[i].fd == fd)
			io->errs[i].err = 0;
}

static int write_all(int fd, struct iovec *iov, int cnt, uint64_t off, bool positional)
{
	while (cnt > 0) {
		ssize_t n = positional ? pwritev(fd, iov, cnt, (off_t)off) : writev(fd, iov, cnt);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			return errno;
		}
		if (n == 0)
			return EIO;
		off += (uint64_t)n;
		while (cnt > 0 && (size_t)n >= iov->iov_len) {
			n -= (ssize_t)iov->iov_len;
			iov++;
			cnt--;
		}
		if (cnt > 0) {
			iov->iov_base = (uint8_t *)iov->iov_base + n;
			iov->iov_len -= (size_t)n;
		}
	}
	return 0;
}

static void *writer(void *arg)
{
	rmr_io_t *io = arg;

	pthread_mutex_lock(&io->lock);
	for (;;) {
		while (io->op_rd == io->op_wr && !io->stop)
			pthread_cond_wait(&io->not_empty, &io->lock);
		if (io->op_rd == io->op_wr)
			break;

		io_op_t op = io->ops[io->op_rd & (IO_OPS - 1)];
		uint32_t at = (uint32_t)(io->rd % io->size);
		bool failed = err_get(io, op.fd) != 0;
		io->busy = true;
		pthread_mutex_unlock(&io->lock);

		/* The producer never writes past rd, so these bytes are ours */
		int err = 0;
		if ((op.kind == OP_WRITE || op.kind == OP_PWRITE) && !failed) {
			struct iovec iov[2];
			uint32_t first = io->size - at < op.len ? io->size - at : op.len;
			iov[0] = (struct iovec){io->buf + at, first};
			iov[1] = (struct iovec){io->buf, op.len - first};
			err = write_all(op.fd, iov, op.len > first ? 2 : 1, op.off,
					op.kind == OP_PWRITE);
		} else if (op.kind == OP_SYNC && !failed) {
			if (fdatasync(op.fd) < 0)
				err = errno;
		} else if (op.kind == OP_CLOSE) {
			if (!failed)
				fsync(op.fd);
			close(op.fd);
		}

		pthread_mutex_lock(&io->lock);
		io->busy = false;
		if (op.kind == OP_CLOSE)
			err_clear(io, op.fd);
		else if (err)
			err_set(io, op.fd, err);
		io->rd += op.len;
		io->op_rd++;
		pthread_cond_broadcast(&io->not_full);
		if (io->op_rd == io->op_wr)
			pthread_cond_broadcast(&io->idle);
	}
	pthread_mutex_unlock(&io->lock);
	return NULL;
}

/* Called with the lock held; waits for room, then queues. */
static void enqueue(rmr_io_t *io, int fd, int kind, const void *data, uint32_t len, uint64_t off)
{
	while (io->op_wr - io->op_rd == IO_OPS || io->size - (io->wr - io->rd) < len)
		pthread_cond_wait(&io->not_full, &io->lock);

	if (len) {
		uint32_t at = (uint32_t)(io->wr % io->size);
		uint32_t first = io->size - at < len ? io->size - at : len;
		memcpy(io->buf + at, data, first);
		memcpy(io->buf, (const uint8_t *)data + first, len - first);
		io->wr += len;
		uint32_t used = (uint32_t)(io->wr - io->rd);
		if (used > io->peak)
			io->peak = used;
	}
	io->ops[io->op_wr & (IO_OPS - 1)] =
		(io_op_t){.fd = fd, .kind = kind, .len = len, .off = off};
	io->op_wr++;
	pthread_cond_signal(&io->not_empty);
}

static int queue(rmr_io_t *io, int fd, int kind, const void *data, uint32_t len, uint64_t off)
{
	if (fd < 0)
		return -1;
	if (len > io->size) { /* would never fit: split it */
		for (uint32_t done = 0; done < len;) {
			uint32_t n = len - done > io->size ? io->size : len - done;
			if (queue(io, fd, kind, (const uint8_t *)data + done, n, off + done) < 0)
				return -1;
			done += n;
		}
		return 0;
	}
	pthread_mutex_lock(&io->lock);
	int failed = err_get(io, fd);
	if (!failed)
		enqueue(io, fd, kind, data, len, off);
	pthread_mutex_unlock(&io->lock);
	return failed ? -1 : 0;
}

rmr_io_t *rmr_io_create(uint32_t buffer_bytes)
{
	if (buffer_bytes < 64 * 1024)
		buffer_bytes = 64 * 1024;
	rmr_io_t *io = calloc(1, sizeof(*io));
	if (!io)
		return NULL;
	io->buf = malloc(buffer_bytes);
	if (!io->buf) {
		free(io);
		return NULL;
	}
	io->size = buffer_bytes;
	pthread_mutex_init(&io->lock, NULL);
	pthread_cond_init(&io->not_empty, NULL);
	pthread_cond_init(&io->not_full, NULL);
	pthread_cond_init(&io->idle, NULL);
	if (pthread_create(&io->tid, NULL, writer, io) != 0) {
		pthread_cond_destroy(&io->idle);
		pthread_cond_destroy(&io->not_full);
		pthread_cond_destroy(&io->not_empty);
		pthread_mutex_destroy(&io->lock);
		free(io->buf);
		free(io);
		return NULL;
	}
	return io;
}

void rmr_io_destroy(rmr_io_t *io)
{
	if (!io)
		return;
	pthread_mutex_lock(&io->lock);
	io->stop = true;
	pthread_cond_signal(&io->not_empty);
	pthread_mutex_unlock(&io->lock);
	pthread_join(io->tid, NULL);
	pthread_cond_destroy(&io->idle);
	pthread_cond_destroy(&io->not_full);
	pthread_cond_destroy(&io->not_empty);
	pthread_mutex_destroy(&io->lock);
	free(io->buf);
	free(io);
}

int rmr_io_write(rmr_io_t *io, int fd, const void *buf, uint32_t len)
{
	if (len == 0)
		return 0;
	return queue(io, fd, OP_WRITE, buf, len, 0);
}

int rmr_io_pwrite(rmr_io_t *io, int fd, const void *buf, uint32_t len, uint64_t off)
{
	if (len == 0)
		return 0;
	return queue(io, fd, OP_PWRITE, buf, len, off);
}

void rmr_io_sync(rmr_io_t *io, int fd)
{
	queue(io, fd, OP_SYNC, NULL, 0, 0);
}

void rmr_io_close(rmr_io_t *io, int fd)
{
	if (fd < 0)
		return;
	pthread_mutex_lock(&io->lock);
	enqueue(io, fd, OP_CLOSE, NULL, 0, 0);
	pthread_mutex_unlock(&io->lock);
}

void rmr_io_drain(rmr_io_t *io)
{
	pthread_mutex_lock(&io->lock);
	while (io->op_rd != io->op_wr || io->busy)
		pthread_cond_wait(&io->idle, &io->lock);
	pthread_mutex_unlock(&io->lock);
}

int rmr_io_error(rmr_io_t *io, int fd)
{
	pthread_mutex_lock(&io->lock);
	int err = err_get(io, fd);
	pthread_mutex_unlock(&io->lock);
	return err;
}

uint32_t rmr_io_pending(rmr_io_t *io)
{
	pthread_mutex_lock(&io->lock);
	uint32_t n = (uint32_t)(io->wr - io->rd);
	pthread_mutex_unlock(&io->lock);
	return n;
}

uint32_t rmr_io_room(rmr_io_t *io)
{
	pthread_mutex_lock(&io->lock);
	uint32_t n = io->op_wr - io->op_rd >= IO_OPS ? 0 : io->size - (uint32_t)(io->wr - io->rd);
	pthread_mutex_unlock(&io->lock);
	return n;
}

uint32_t rmr_io_peak(rmr_io_t *io)
{
	pthread_mutex_lock(&io->lock);
	uint32_t n = io->peak;
	pthread_mutex_unlock(&io->lock);
	return n;
}
