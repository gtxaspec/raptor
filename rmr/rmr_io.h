/*
 * rmr_io.h -- recording file I/O on a writer thread
 *
 * The recorder's main loop reads frames from the ring; a write, sync
 * or close that stalls on the storage (an SD card erasing, an NFS
 * server) must not stall that loop, or the ring overflows and frames
 * are lost. Operations queue in order behind a preallocated byte ring
 * and run on one writer thread: data reaches each file in the order
 * it was queued, and a sync or close covers everything queued before
 * it. A full queue blocks the caller -- the ring then absorbs the
 * stall for as long as its own depth allows.
 */

#ifndef RMR_IO_H
#define RMR_IO_H

#include <stdint.h>

typedef struct rmr_io rmr_io_t;

/* buffer_bytes: the most data in flight before writers block */
rmr_io_t *rmr_io_create(uint32_t buffer_bytes);

/* Drains the queue, then stops the thread. */
void rmr_io_destroy(rmr_io_t *io);

/* Queue a copy of buf for write(fd). Returns -1 when fd has failed. */
int rmr_io_write(rmr_io_t *io, int fd, const void *buf, uint32_t len);

/* Queue a copy of buf for pwrite(fd) at off. */
int rmr_io_pwrite(rmr_io_t *io, int fd, const void *buf, uint32_t len, uint64_t off);

/* Queue fdatasync(fd) / fsync(fd) + close(fd). */
void rmr_io_sync(rmr_io_t *io, int fd);
void rmr_io_close(rmr_io_t *io, int fd);

/* Wait until everything queued so far has run. */
void rmr_io_drain(rmr_io_t *io);

/* errno of the first failed operation on fd since it was opened, else
 * 0. Cleared when the close runs. */
int rmr_io_error(rmr_io_t *io, int fd);

/* Bytes queued and not yet written; the high-water mark since create. */
uint32_t rmr_io_pending(rmr_io_t *io);
uint32_t rmr_io_peak(rmr_io_t *io);

#endif /* RMR_IO_H */
