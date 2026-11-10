/*
 * libvsock: stream sockets over VirtioVsock.kext, for Mac OS X 10.0 and later on PowerPC.
 *
 * There is no AF_VSOCK in these kernels (nor a way to add a family from a kext), so this is a small
 * socket-like API of its own on the driver's IOUserClient: handles are ints, as file descriptors
 * are, but not file descriptors -- they work with these functions only, not read(2) or select(2).
 * Calls block unless asked not to, by polling the driver from user space: a signal ends the wait
 * with EINTR, and a process that is killed dies. There is no timeout but connect's. Failures return -1 with errno set.
 *
 * A CID is 64 bits, passed as two halves. The host is (0, VSOCK_CID_HOST); a guest's own is
 * vsock_local_cid().
 *
 * Link with: -framework IOKit -framework CoreFoundation
 */

#ifndef _VSOCK_H
#define _VSOCK_H

#include <stddef.h>
#include "VirtioVsockUser.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Opens the driver. Called by the rest as needed; fails with ENXIO where there is no device. */
int vsock_open(void);

/* Our own CID. */
int vsock_local_cid(unsigned int *hi, unsigned int *lo);

/* Listens on a port; backlog is up to 8. Returns a handle. */
int vsock_listen(unsigned int port, int backlog);

/* Accepts a connection; the peer's address is returned through the pointers, any of them null.
 * With nonblock, EAGAIN when none is waiting. Returns a handle. */
int vsock_accept(int handle, unsigned int *peer_hi, unsigned int *peer_lo, unsigned int *peer_port,
                 int nonblock);

/* Connects to a CID and port; timeout_ms 0 is ten seconds. ECONNREFUSED if nothing listens there,
 * ETIMEDOUT if nothing answers. Returns a handle. */
int vsock_connect(unsigned int cid_hi, unsigned int cid_lo, unsigned int port, unsigned int timeout_ms);

/* Reads up to n bytes: how many were read, 0 at the end of the stream. */
int vsock_read(int handle, void *buf, size_t n, int nonblock);

/* Writes all n bytes (or, with nonblock, as many as there is room for); returns how many. EPIPE
 * where the peer takes no more. */
int vsock_write(int handle, const void *buf, size_t n, int nonblock);

/* how: 1 no more reading, 2 no more writing, 3 both. The peer sees the end of the stream. */
int vsock_shutdown(int handle, int how);

/* Closes the handle at once; a connection goes on closing in the background. */
int vsock_close(int handle);

/* Bytes readable, bytes writable and VSOCK_P_* flags. */
int vsock_poll(int handle, unsigned int *readable, unsigned int *writable, unsigned int *flags);

#ifdef __cplusplus
}
#endif

#endif /* !_VSOCK_H */
