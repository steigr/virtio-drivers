/*
 * VirtioVsock: QEMU's virtio socket device (vhost-vsock-pci, 1af4:1053) for Mac OS X 10.0 and later
 * on PowerPC: stream sockets between the guest and the host (or other guests), by CID and port,
 * with no network in between.
 *
 * Unlike the other drivers here, this device has no legacy interface: QEMU makes it modern-only
 * whatever `disable-modern` says (checked live: 1af4:1053, one 64-bit memory BAR, 2026-10-02). So
 * the transport is virtio 1.x's PCI one -- capabilities in configuration space pointing into the
 * BAR, VIRTIO_F_VERSION_1, FEATURES_OK, a split ring whose three parts are placed separately --
 * and, the guest being big-endian, everything the device reads or writes is little-endian and is
 * swapped on the way (as elliotnunn/classicvirtio does for Mac OS 9, which has no vsock).
 *
 * Mac OS X 10.0 to 10.3 have no kernel interface for adding a socket family from a kext, so the
 * sockets are not BSD sockets: the driver is an IOService with an IOUserClient, which libvsock
 * (vsock.h) wraps in a socket-like C API. See VirtioVsockUser.h for the calls.
 *
 * Built twice from this one source, as VirtioConsole is: GCC 2.95 for 10.0 and 10.1, GCC 3 for
 * 10.2 on. So no templates, exceptions, RTTI or C++ library, no 64-bit arithmetic (CIDs are two
 * 32-bit halves), and only what 10.0's kernel and IOPCIFamily export.
 *
 * The protocol is virtio 1.x's "Socket Device" chapter: queue 0 receives, queue 1 transmits, queue
 * 2 carries events from the device; every packet is a 44-byte header and up to a slot's payload,
 * with credit-based flow control (each side says how much it can buffer and how much it has
 * consumed, and sends no more than the difference). A connection is a REQUEST answered by a
 * RESPONSE or an RST, closed by SHUTDOWN and the peer's RST.
 *
 * All state is touched on the driver's own work loop only: the user client's calls enter it
 * through a command gate. Nothing sleeps there (see VirtioVsockUser.h for why); a call that
 * cannot proceed says so and is retried by its caller.
 */

#ifndef _GR_STEI_VIRTIOVSOCK_H
#define _GR_STEI_VIRTIOVSOCK_H

#include <IOKit/IOService.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IOCommandGate.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/IOFilterInterruptEventSource.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IOLocks.h>
#include <IOKit/pci/IOPCIDevice.h>

#include "VirtioVsockUser.h"

/* A split virtqueue, as VirtioNet's but little-endian in memory; one descriptor per slot. */
struct VsockDesc {
    UInt8  addr[8];
    UInt8  len[4];
    UInt8  flags[2];
    UInt8  next[2];
};

struct VsockQueue {
    UInt16              index;      /* the queue's number on the device */
    UInt16              num;        /* its size */
    UInt16              slots;      /* the buffers: descriptor n is slot n's */
    UInt16              notifyOff;  /* queue_notify_off */
    UInt8              *mem;        /* the rings */
    IOPhysicalAddress   memPhys;
    vm_size_t           memSize;
    UInt32              availOff, usedOff;
    UInt8             **pages;      /* the slots' pages, two slots to a page */
    IOPhysicalAddress  *pagesPhys;
    UInt16              pageCount;
    UInt16              lastUsed;   /* the used index consumed up to */
    UInt16              nextAvail;  /* the available index published up to */
};

/* Where a virtio structure sits: a capability's bar, offset and length, and its mapping. */
struct VsockRegion {
    UInt8               bar;
    UInt32              offset;
    UInt32              length;
    UInt8              *virt;       /* 0 where only configuration-space access works */
    UInt16              capOff;     /* the capability's place, for the PCI_CFG window */
};

enum {
    kConnFree,
    kConnListen,
    kConnConnecting,    /* REQUEST sent */
    kConnConnected,     /* includes a half-closed one; see the shutdown flags */
    kConnClosing,       /* closed by the user, SHUTDOWN sent, waiting for the peer's RST */
    kConnReset          /* the peer reset it, or the device did; the user has yet to close */
};

enum {
    kPendRequest   = 0x01,      /* the control packets a connection owes the peer, */
    kPendResponse  = 0x02,      /* sent as transmit slots allow */
    kPendRst       = 0x04,
    kPendShutdown  = 0x08,
    kPendCredit    = 0x10,
    kPendCreditReq = 0x20
};

#define kAcceptMax  8

struct VsockConn {
    UInt8               state;
    UInt8               pending;
    UInt8               shutdownMine;   /* VIRTIO_VSOCK_SHUTDOWN_* we have sent */
    UInt8               shutdownPeer;   /* and the peer has */
    UInt8               refused;        /* connecting: the peer answered RST */
    UInt8               acceptCount;
    UInt16              acceptq[kAcceptMax];    /* a listener's connections to accept, by index */
    UInt32              localPort, peerPort;
    UInt32              peerCidHi, peerCidLo;
    void               *owner;          /* the user client */
    UInt32              ticks;          /* the deadline's clock: counted down by the timer */
    UInt32              listener;       /* the index of the listener that has this one queued, +1 */

    /* flow control, in bytes, wrapping */
    UInt32              txCnt;          /* sent */
    UInt32              peerBufAlloc;
    UInt32              peerFwdCnt;
    UInt32              fwdCnt;         /* consumed by the user */
    UInt32              fwdSent;        /* what fwdCnt the peer was last told */
    UInt32              recvCnt;        /* received into rxBuf, for a plausibility check */

    UInt8              *rxBuf;
    UInt32              rxHead, rxLen;
};

#define kMaxConns   32

class io_virtio_VirtioVsock : public IOService
{
    OSDeclareDefaultStructors(io_virtio_VirtioVsock)

public:
    virtual bool       start(IOService *provider);
    virtual void       stop(IOService *provider);
    virtual void       free();
    virtual IOWorkLoop *getWorkLoop() const;

    /* from the user client, any thread: runs the command on the work loop */
    IOReturn   command(int op, void *client, UInt32 a0, UInt32 a1, UInt32 a2, UInt32 a3,
                       UInt8 *data, UInt32 dataLen, UInt32 *out);
    /* the user client is gone: its sockets go with it */
    void       clientGone(void *client);
    void       closeFor(VsockConn *c);

private:
    /* the transport */
    bool     deviceProbe();
    bool     deviceStart();
    void     deviceReset();
    bool     findCaps();
    bool     queueAlloc(VsockQueue *q, UInt16 index, UInt16 maxSlots);
    bool     queueSetup(VsockQueue *q);
    void     queueFree(VsockQueue *q);
    UInt8   *slotAddress(VsockQueue *q, UInt16 slot);
    void     queuePublish(VsockQueue *q, UInt16 slot);
    void     queueNotify(VsockQueue *q);

    UInt32   regRead(VsockRegion *r, UInt32 off, int size);
    void     regWrite(VsockRegion *r, UInt32 off, int size, UInt32 value);
    UInt8    cfgRead8(UInt32 off);
    UInt16   cfgRead16(UInt32 off);
    UInt32   cfgRead32(UInt32 off);
    void     cfgWrite8(UInt32 off, UInt8 v);
    void     cfgWrite16(UInt32 off, UInt16 v);
    void     cfgWrite32(UInt32 off, UInt32 v);
    bool     mapBar(UInt8 bar);

    /* packets */
    bool     sendPacket(UInt32 dstHi, UInt32 dstLo, UInt32 srcPort, UInt32 dstPort, UInt16 op,
                        UInt32 flags, UInt32 fwdCnt, const UInt8 *data, UInt32 len, bool reserve);
    bool     sendConn(VsockConn *c, UInt16 op, UInt32 flags, const UInt8 *data, UInt32 len, bool reserve);
    void     receive(UInt8 *pkt, UInt32 total);
    void     flushPending();

    /* connections */
    VsockConn *allocConn();
    void     freeConn(VsockConn *c);
    void     reap();
    VsockConn *findConn(UInt32 hi, UInt32 lo, UInt32 peerPort, UInt32 localPort);
    VsockConn *findListener(UInt32 port);
    VsockConn *handleToConn(void *client, UInt32 handle);
    UInt32   writeCredit(VsockConn *c);
    void     resetAll();
    IOReturn runCommand(void *cmd);
    void     shutdownRelease(VsockConn *c, UInt8 how);

    /* the work, on the work loop */
    void     service();
    void     serviceRx();
    void     serviceTx();
    void     serviceEvents();
    void     interruptOccurred();
    void     timeoutOccurred();

    static IOReturn commandAction(OSObject *owner, void *cmd, void *, void *, void *);
    static bool interruptFilter(OSObject *owner, IOFilterInterruptEventSource *src);
    static void interruptAction(OSObject *owner, IOInterruptEventSource *src, int count);
    static void timeoutAction(OSObject *owner, IOTimerEventSource *src);

    IOPCIDevice                  *fPCI;
    IOMemoryMap                  *fMaps[6];
    IOWorkLoop                   *fWorkLoop;
    IOCommandGate                *fGate;
    IOFilterInterruptEventSource *fInterrupt;   /* 0 when polled */
    IOTimerEventSource           *fTimer;

    VsockRegion                   fCommon, fNotify, fISRReg, fDevice;
    UInt32                        fNotifyMult;
    UInt16                        fCfgWindowOff;    /* the PCI_CFG capability, 0 if none */
    IOSimpleLock                 *fWindowLock;      /* one window access at a time (regRead) */

    VsockQueue                    fRx, fTx, fEv;
    UInt16                       *fTxFree;
    UInt16                        fTxFreeCount;
    UInt32                        fCidHi, fCidLo;
    UInt32                        fFeatures;

    VsockConn                     fConns[kMaxConns];
    UInt32                        fNextPort;
    bool                          fRunning;
    bool                          fPolled;
    bool                          fDead;
    volatile UInt32               fISR;
    volatile UInt32               fInterrupts;
    UInt32                        fInterruptsSeen;
    UInt32                        fQuietTicks;
};

class io_virtio_VirtioVsockClient : public IOUserClient
{
    OSDeclareDefaultStructors(io_virtio_VirtioVsockClient)

public:
    virtual bool       initWithTask(task_t owningTask, void *securityID, UInt32 type);
    virtual bool       start(IOService *provider);
    virtual IOReturn   clientClose();
    virtual IOReturn   clientDied();
    virtual IOExternalMethod *getExternalMethodForIndex(UInt32 index);

    IOReturn   listen(UInt32 port, UInt32 backlog, UInt32 *handle);
    IOReturn   accept(UInt32 handle, UInt32 nonblock, UInt32 *newHandle, UInt32 *cidHi, UInt32 *cidLo, UInt32 *port);
    IOReturn   connect(UInt32 cidHi, UInt32 cidLo, UInt32 port, UInt32 timeoutMS, UInt32 *handle);
    IOReturn   read(VsockReadIn *in, VsockReadOut *out, IOByteCount inSize, IOByteCount *outSize);
    IOReturn   write(VsockWriteIn *in, VsockWriteOut *out, IOByteCount inSize, IOByteCount *outSize);
    IOReturn   shutdown(UInt32 handle, UInt32 how);
    IOReturn   close(UInt32 handle);
    IOReturn   info(UInt32 *cidHi, UInt32 *cidLo, UInt32 *count);
    IOReturn   poll(UInt32 handle, UInt32 *readable, UInt32 *writable, UInt32 *flags);

private:
    io_virtio_VirtioVsock *fDriver;
    IOExternalMethod     fMethods[kVsockMethodCount];
};

#endif /* !_GR_STEI_VIRTIOVSOCK_H */
