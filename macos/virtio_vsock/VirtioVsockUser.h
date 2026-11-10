/*
 * The interface between VirtioVsock.kext and its users: the selectors and structures of the
 * IOUserClient connection (IOConnectMethod*), shared by the kext and libvsock.
 *
 * One connection to the driver (IOServiceOpen) is a process's context; sockets are small integer
 * handles within it, as file descriptors are. No call waits: one that cannot proceed returns
 * kIOReturnNotReady, and libvsock's blocking calls poll, in user space, where a signal can end
 * them. (The first version slept in the kernel, interruptibly as it thought; on 10.2 a signal,
 * SIGKILL included, neither woke the thread nor let the process die, and the guest hung shortly
 * after, live 2026-10-02.) Connection ids (CIDs) are 64 bits and travel as two 32-bit halves,
 * because scalar arguments are 32 bits wide.
 */

#ifndef _GR_STEI_VIRTIOVSOCK_USER_H
#define _GR_STEI_VIRTIOVSOCK_USER_H

#define kVirtioVsockClass       "io_virtio_VirtioVsock"

/* The CIDs the specification reserves. */
#define VSOCK_CID_HYPERVISOR    0
#define VSOCK_CID_HOST          2

/* The most that one read or write call moves. Structure arguments travel in-band, in 4096 bytes,
 * and 10.0's and 10.1's IOUserClient takes only structures of exactly the size the method declares
 * (kIOUCVariableStructureSize is later), so both read and write exchange the whole structure and
 * say in it how much of it counts. */
#define VSOCK_XFER_MAX          4000

enum {
    kVsockListen,       /* in: port, backlog                 out: handle */
    kVsockAccept,       /* in: handle                        out: handle, peer cid hi, lo, port */
    kVsockConnect,      /* in: cid hi, lo, port, unused      out: handle, which is connecting until
                           kVsockPoll says VSOCK_P_CONNECTED, or VSOCK_P_RESET for a refusal */
    kVsockRead,         /* in: VsockReadIn; out: VsockReadOut, no bytes at the end of the stream */
    kVsockWrite,        /* in: VsockWriteIn; out: VsockWriteOut */
    kVsockShutdown,     /* in: handle, how (1 receive, 2 send, 3 both) */
    kVsockClose,        /* in: handle */
    kVsockInfo,         /* out: our cid hi, lo; the number of sockets open */
    kVsockPoll,         /* in: handle; out: bytes readable, bytes writable, VSOCK_P_* flags */
    kVsockMethodCount
};

/* kVsockPoll's flags. */
#define VSOCK_P_CONNECTED   0x01    /* data may flow */
#define VSOCK_P_EOF         0x02    /* the peer will send no more; reads drain and then end */
#define VSOCK_P_RESET       0x04    /* the connection was reset or the device reset */
#define VSOCK_P_ACCEPT      0x08    /* a listener with a connection waiting */
#define VSOCK_P_NOSEND      0x10    /* the peer takes no more; writes fail */

typedef struct {
    int             handle;
    unsigned int    maxLength;
    unsigned int    nonblock;
} VsockReadIn;

typedef struct {
    unsigned int    length;
    unsigned char   data[VSOCK_XFER_MAX];
} VsockReadOut;

typedef struct {
    int             handle;
    unsigned int    nonblock;
    unsigned int    length;
    unsigned char   data[VSOCK_XFER_MAX];
} VsockWriteIn;

typedef struct {
    unsigned int    written;
} VsockWriteOut;

#endif /* !_GR_STEI_VIRTIOVSOCK_USER_H */
