/*
 * VirtioRand: QEMU's legacy virtio entropy device (virtio-rng-pci, 1af4:1005) as a source for the
 * kernel's /dev/random, for Mac OS X 10.0 and later on PowerPC.
 *
 * A mac99 guest has no hardware random number generator, and these releases seed Yarrow from timing
 * jitter and a seed file alone; this feeds it from the host's (QEMU's rng backend, /dev/urandom or
 * whatever the host chose) as well. The driver is a plain IOService with no client: once the device
 * is up it takes bytes from it and mixes them into the generator, first a burst to prime it, then a
 * few every few seconds.
 *
 * Built twice from this one source, as VirtioConsole is: GCC 2.95 (December 2001 Developer Tools)
 * for 10.0 and 10.1, GCC 3 (December 2002 Developer Tools) for 10.2 on. So no templates,
 * exceptions, RTTI or C++ library, and only what 10.0's kernel and IOPCIFamily export.
 *
 * The device side, from the virtio 0.9.5 (legacy PCI) specification and the virtio 1.x "Entropy
 * Device" chapter:
 *
 * - No feature bits are defined, so none are negotiated. One virtqueue, requestq (queue 0). The
 *   driver gives it a device-writable buffer; the device fills it with random bytes and returns it
 *   with the number it wrote, which may be fewer than asked for. There is no device configuration.
 * - Rings in the guest's (big-endian) byte order, as legacy devices keep them.
 * - Polled from a work-loop timer: the device is told not to interrupt (VRING_AVAIL_F_NO_INTERRUPT)
 *   and the timer looks for the buffer's return every few milliseconds while a request is out, and
 *   sleeps for the interval otherwise. Entropy is slow and the cost of a poll is a memory read, so
 *   nothing is gained by an interrupt.
 *
 * Where the bytes go: random_write(), the kernel's own write routine of /dev/random, which takes a
 * struct uio. A uio over kernel memory (UIO_SYSSPACE) makes it do for the driver what a write to
 * the device does for a process -- mix the bytes into Yarrow under its lock and force a reseed --
 * with no unexported symbol: the generator's state and mutex are static in the kernel, and
 * random_write is identical in 10.1's and 10.2's, bar its mutex symbol's name (read off both).
 * The bytes are credited at eight bits each, as every write to /dev/random is.
 */

#ifndef _GR_STEI_VIRTIORAND_H
#define _GR_STEI_VIRTIORAND_H

#include <IOKit/IOService.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/pci/IOPCIDevice.h>

/* A split virtqueue, legacy layout; see VirtioConsole.h. */
struct VirtioRandDesc {
    UInt32 addrHigh;
    UInt32 addrLow;
    UInt32 len;
    UInt16 flags;
    UInt16 next;
};

struct VirtioRandUsedElem {
    UInt32 id;
    UInt32 len;
};

class io_virtio_VirtioRand : public IOService
{
    OSDeclareDefaultStructors(io_virtio_VirtioRand)

public:
    virtual bool start(IOService *provider);
    virtual void stop(IOService *provider);
    virtual void free();

private:
    bool     deviceStart();
    void     deviceReset();
    void     request();
    void     tick();
    void     feed(UInt32 len);
    void     publishCounts();

    static void tickAction(OSObject *owner, IOTimerEventSource *src);

    UInt8    rd8(UInt16 off);
    UInt16   rd16(UInt16 off);
    UInt32   rd32(UInt16 off);
    void     wr8(UInt16 off, UInt8 v);
    void     wr16(UInt16 off, UInt16 v);
    void     wr32(UInt16 off, UInt32 v);

    IOPCIDevice                  *fPCI;
    IOMemoryMap                  *fIO;
    UInt16                        fIOBase;      /* the I/O BAR's port, used when it could not be mapped */
    IOWorkLoop                   *fWorkLoop;
    IOTimerEventSource           *fTimer;

    UInt16                        fNum;         /* requestq's size, as the device reports it */
    UInt8                        *fRing;        /* the rings, */
    IOPhysicalAddress             fRingPhys;
    vm_size_t                     fRingSize;
    UInt8                        *fBuf;         /* and the one buffer the device fills */
    IOPhysicalAddress             fBufPhys;
    volatile VirtioRandDesc      *fDesc;
    volatile UInt16              *fAvailFlags;
    volatile UInt16              *fAvailIdx;
    volatile UInt16              *fAvailRing;
    volatile UInt16              *fUsedIdx;
    volatile VirtioRandUsedElem  *fUsedRing;
    UInt16                        fLastUsed;
    UInt16                        fNextAvail;

    UInt32                        fIntervalMS;  /* between requests, once primed */
    UInt32                        fPrimed;      /* bytes taken so far in the opening burst */
    bool                          fPending;     /* the buffer is the device's */
    bool                          fStopping;
    bool                          fFeedFailed;  /* random_write refused; said once */
    UInt32                        fWaitTicks;   /* ticks the pending request has waited */
    UInt32                        fRequests;    /* completed requests, */
    UInt32                        fBytes;       /* and the bytes they gave the generator */
};

#endif /* !_GR_STEI_VIRTIORAND_H */
