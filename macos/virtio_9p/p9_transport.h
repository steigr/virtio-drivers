/*
 * The seam between the two halves of Virtio9P.kext: the IOKit driver (Virtio9P.cpp, C++), which
 * owns the virtio device, and the filesystem (p9fs_*.c, C, BSD kernel), which speaks 9P over it.
 * Plain C types only, so that this header can be included on both sides.
 *
 * A device carries one 9P session, so it serves one mount at a time: p9t_open claims it by its
 * mount tag and p9t_close gives it back. The device has one request buffer and one reply buffer,
 * physically contiguous, and p9t_rpc sends the first tlen bytes of one and waits for the other.
 * The caller serialises its requests; nothing here does.
 */

#ifndef _P9_TRANSPORT_H
#define _P9_TRANSPORT_H

#ifdef __cplusplus
extern "C" {
#endif

/* Claims the device whose mount tag is tag. 0, or ENXIO (no such device) or EBUSY (mounted). */
int  p9t_open(const char *tag, void **handle, unsigned char **tbuf, unsigned char **rbuf,
              unsigned int *bufsize);
void p9t_close(void *handle);

/* Sends tbuf[0, tlen) and waits for the reply, whose length goes to *rlen. 0, or ENXIO once the
 * device has gone. */
int  p9t_rpc(void *handle, unsigned int tlen, unsigned int *rlen);

#ifdef __cplusplus
}
#endif

#endif /* !_P9_TRANSPORT_H */
