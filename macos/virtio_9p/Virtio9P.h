/*
 * Virtio9P: QEMU's legacy virtio 9P transport (virtio-9p-pci, 1af4:1009) as a filesystem, for Mac
 * OS X 10.0 to 10.13, on PowerPC and on Intel (i386 and x86_64).
 *
 * One kext, two halves. This one, the IOKit driver, claims the PCI device, reads its mount tag,
 * sets up its one virtqueue and carries 9P messages: a request buffer and a reply buffer, each
 * physically contiguous, chained into one descriptor pair per message. The other half,
 * p9fs_*.c, is the filesystem ("9p", mounted with mount_9p), which speaks 9P2000.L to QEMU's
 * server over p9_transport.h.
 *
 * Built twice from one source like VirtioConsole -- GCC 2.95 for 10.0 and 10.1, GCC 3 for 10.2
 * on -- so the same restrictions hold: no templates, exceptions, RTTI or C++ library, and only
 * what 10.0's kernel and IOPCIFamily export.
 *
 * Built a third time for the Intel releases, against the KPI of 10.4 on (ABI=kpi,
 * guest/drivers/virtio9p/README.md's "Intel"), where the same restrictions are kept so that this
 * file stays one source.
 *
 * Legacy (0.9.5) virtio-pci, no feature but the mount tag negotiated, rings in the guest's own
 * byte order -- big-endian on PowerPC, little-endian on Intel -- so that nothing in them is ever
 * swapped. Interrupt-driven where the device's interrupt can be had (0.2.0 on): a request is
 * published on the driver's work loop and its caller sleeps on the command gate until the
 * interrupt, or a watchdog that finds a reply no interrupt announced, wakes it; a watchdog that
 * keeps finding them turns polling on. Polled otherwise, or with Polled true in the personality: a
 * short spin, then millisecond sleeps, and the rings ask the device for no interrupt. The
 * interrupt is a plain MSI vector where the device offers MSI and no MSI-X (0.3.0 on), else the
 * legacy line.
 */

#ifndef _GR_STEI_VIRTIO9P_H
#define _GR_STEI_VIRTIO9P_H

#include <IOKit/IOService.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IOCommandGate.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/IOFilterInterruptEventSource.h>
#include <IOKit/pci/IOPCIDevice.h>

/* A split virtqueue, legacy layout; see VirtioConsole.h. The address is one 64-bit field in the
 * guest's order: VirtioConsole's addrHigh and addrLow are that field's halves on a big-endian
 * guest only. unsigned long long, not UInt64, which some of these releases' headers define as a
 * pair of words. */
struct Virtio9PDesc {
    unsigned long long addr;
    UInt32 len;
    UInt16 flags;
    UInt16 next;
};

struct Virtio9PUsedElem {
    UInt32 id;
    UInt32 len;
};

class io_virtio_Virtio9P : public IOService
{
    OSDeclareDefaultStructors(io_virtio_Virtio9P)

public:
    virtual bool start(IOService *provider);
    virtual void stop(IOService *provider);
    virtual void free();
    virtual bool terminate(IOOptionBits options = 0);

    /* For p9_transport.h. */
    bool     hasTag(const char *tag);
    int      claim(UInt8 **tbuf, UInt8 **rbuf, UInt32 *size);
    void     unclaim();
    int      rpc(UInt32 tlen, UInt32 *rlen);

private:
    int      rpcPolled(UInt32 tlen, UInt32 *rlen);
    int      rpcGated(UInt32 tlen, UInt32 *rlen);
    void     publish(UInt32 tlen);
    void     takeReply(UInt32 *rlen);
    void     interruptOccurred();
    void     timeoutOccurred();
    static IOReturn rpcAction(OSObject *owner, void *arg0, void *arg1, void *arg2, void *arg3);
    static bool     interruptFilter(OSObject *owner, IOFilterInterruptEventSource *src);
    static void     interruptAction(OSObject *owner, IOInterruptEventSource *src, int count);
    static void     timeoutAction(OSObject *owner, IOTimerEventSource *src);

    bool     deviceStart();
    void     deviceReset();
    bool     readTag();

    UInt16   rd16(UInt16 off);
    UInt32   rd32(UInt16 off);
    UInt8    rd8(UInt16 off);
    void     wr8(UInt16 off, UInt8 v);
    void     wr16(UInt16 off, UInt16 v);
    void     wr32(UInt16 off, UInt32 v);

    IOPCIDevice        *fPCI;
    IOMemoryMap        *fIO;
    UInt16              fIOBase;        /* the I/O BAR's port, used when it could not be mapped */

    UInt16              fQueueNum;      /* the request queue's size, as the device reports it */
    UInt8              *fRing;          /* descriptors, available ring, used ring */
    IOPhysicalAddress   fRingPhys;
    vm_size_t           fRingSize;
    volatile Virtio9PDesc     *fDesc;
    volatile UInt16           *fAvailFlags;
    volatile UInt16           *fAvailIdx;
    volatile UInt16           *fAvailRing;
    volatile UInt16           *fUsedIdx;
    volatile Virtio9PUsedElem *fUsedRing;
    UInt16              fLastUsed;
    UInt16              fNextAvail;

    UInt8              *fTBuf;          /* the request buffer */
    IOPhysicalAddress   fTBufPhys;
    UInt8              *fRBuf;          /* the reply buffer */
    IOPhysicalAddress   fRBufPhys;
    UInt32              fBufSize;

    IOWorkLoop                   *fWorkLoop;
    IOCommandGate                *fGate;
    IOTimerEventSource           *fTimer;
    IOFilterInterruptEventSource *fInterrupt;   /* 0 when polled from the start */
    bool                fPolled;        /* replies are watched for, not announced */
    bool                fWaiting;       /* a caller sleeps on the gate for a reply */
    volatile UInt32     fInterrupts;    /* interrupts taken, */
    UInt32              fInterruptsSeen;    /* as of the last watchdog tick */
    UInt32              fQuietTicks;    /* ticks that found a reply no interrupt had announced */

    char                fTag[64];       /* the mount tag, NUL-terminated */
    UInt32              fClaimed;       /* a mount is using the device */
    bool                fDeviceUp;
    volatile bool       fStopping;
};

#endif /* !_GR_STEI_VIRTIO9P_H */
