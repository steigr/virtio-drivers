/*
 * Virtio9P: see Virtio9P.h for what this is and why it is written the way it is. The virtio side
 * is written from the virtio 0.9.5 (legacy PCI) specification and follows VirtioConsole.cpp; the
 * 9P device's configuration (a 16-bit tag length, then the tag) is the virtio specification's
 * "9P Transport" device (device ID 9).
 */

#include "Virtio9P.h"
#include "p9_transport.h"

#include <IOKit/IOLib.h>
#ifdef VIRTIO9P_TRACE
#include <libkern/libkern.h>     /* printf, for P9_TRACE; not in every release's headers */
#endif
#include <libkern/OSAtomic.h>
#include <sys/errno.h>

/* Mach's wait states, which not every release's headers reach from IOKit's. */
#ifndef THREAD_UNINT
#define THREAD_UNINT 0
#endif

#define super IOService
OSDefineMetaClassAndStructors(io_virtio_Virtio9P, IOService)

/* Legacy virtio PCI: the common header in the I/O BAR, then -- with MSI-X off, as it is here --
 * the device's own configuration at 20. IOPCIDevice's ioRead/ioWrite present PCI's
 * little-endian registers in the host's order. */
enum {
    kVirtioHostFeatures  = 0x00,    /* 32 */
    kVirtioGuestFeatures = 0x04,    /* 32 */
    kVirtioQueuePFN      = 0x08,    /* 32 */
    kVirtioQueueNum      = 0x0C,    /* 16 */
    kVirtioQueueSel      = 0x0E,    /* 16 */
    kVirtioQueueNotify   = 0x10,    /* 16 */
    kVirtioStatus        = 0x12,    /*  8 */
    kVirtioISR           = 0x13,    /*  8 */
    kVirtioConfig        = 0x14
};

enum {
    kStatusAcknowledge = 0x01,
    kStatusDriver      = 0x02,
    kStatusDriverOK    = 0x04,
    kStatusFailed      = 0x80
};

enum {
    kFeatureMountTag   = 0x00000001     /* VIRTIO_9P_MOUNT_TAG */
};

enum {
    kDescNext          = 1,
    kDescWrite         = 2,
    kAvailNoInterrupt  = 1
};

enum {
    kPage              = 4096,
    kBufSize           = 65536,     /* each of the request and the reply buffer: the 9P msize */
    kBufSizeMin        = 8192,      /* what is tried when 64 KiB is not to be had contiguous */
    kSpinUS            = 10,        /* a reply is looked for this often at first, */
    kSpins             = 200,       /* this many times (2 ms), then every millisecond */
    kGatedSpins        = 50,        /* interrupt-driven, a reply is spun for this long first (0.5 ms), */
    kWatchdogMS        = 50,        /* then slept for, with this watchdog; */
    kPollMS            = 1,         /* polled after the watchdog gave up on the interrupt, */
    kQuietTicksMax     = 2          /* which it does after this many ticks with a silent reply */
};

enum {
    kISRQueue          = 0x01
};

enum {
    kConfigCommand     = 0x04,
    kConfigBAR0        = 0x10,
    kCommandIO         = 0x0001,
    kCommandBusMaster  = 0x0004
};

static inline UInt32 roundPage(UInt32 n) { return (n + kPage - 1) & ~(UInt32)(kPage - 1); }

/* Orders memory the device reads against the register write that tells it to; see
 * VirtioConsole.cpp. On Intel, stores are not reordered with other stores and an I/O port
 * write waits for them, so only the compiler has to be stopped. */
#if defined(__ppc__)
static inline void barrier() { __asm__ __volatile__ ("sync" : : : "memory"); }
#else
static inline void barrier() { __asm__ __volatile__ ("" : : : "memory"); }
#endif

/* A store and a later load ordered against each other, which x86 does not do by itself: the
 * interrupt asked for (a store the device reads) before the used index looked at again (a load the
 * device writes), or the device may see no request for an interrupt and the driver no reply. */
#if defined(__ppc__)
static inline void fullBarrier() { __asm__ __volatile__ ("sync" : : : "memory"); }
#else
static inline void fullBarrier() { __asm__ __volatile__ ("mfence" : : : "memory"); }
#endif

/* With the BAR mapped, register offsets are relative to it; without, they are absolute I/O
 * ports, the BAR's own address plus the register. */
UInt8  io_virtio_Virtio9P::rd8(UInt16 off)            { return fPCI->ioRead8((fIO ? 0 : fIOBase) + off, fIO); }
UInt16 io_virtio_Virtio9P::rd16(UInt16 off)           { return fPCI->ioRead16((fIO ? 0 : fIOBase) + off, fIO); }
UInt32 io_virtio_Virtio9P::rd32(UInt16 off)           { return fPCI->ioRead32((fIO ? 0 : fIOBase) + off, fIO); }
void   io_virtio_Virtio9P::wr8(UInt16 off, UInt8 v)   { fPCI->ioWrite8((fIO ? 0 : fIOBase) + off, v, fIO); }
void   io_virtio_Virtio9P::wr16(UInt16 off, UInt16 v) { fPCI->ioWrite16((fIO ? 0 : fIOBase) + off, v, fIO); }
void   io_virtio_Virtio9P::wr32(UInt16 off, UInt32 v) { fPCI->ioWrite32((fIO ? 0 : fIOBase) + off, v, fIO); }

/* ------------------------------------------------------------------------------------------ */
/* the device                                                                                  */
/* ------------------------------------------------------------------------------------------ */

void io_virtio_Virtio9P::deviceReset()
{
    wr8(kVirtioStatus, 0);
    fDeviceUp = false;
}

/* The mount tag: a 16-bit length and that many bytes, not NUL-terminated. The length is read a
 * byte at a time and taken in the guest's order, which is what a legacy device keeps its
 * configuration in; a length that makes no sense the other way round is swapped. */
bool io_virtio_Virtio9P::readTag()
{
    UInt16 len, i;

    len = (UInt16) ((rd8(kVirtioConfig) << 8) | rd8(kVirtioConfig + 1));
    if (len > 255)
        len = (UInt16) ((len >> 8) | (len << 8));
    if (len == 0 || len > 255) {
        IOLog("Virtio9P: the device has no mount tag (length %d)\n", len);
        return false;
    }
    if (len > sizeof(fTag) - 1) {
        IOLog("Virtio9P: the mount tag is %d bytes, longer than %d\n", len, (int) sizeof(fTag) - 1);
        return false;
    }
    for (i = 0; i < len; i++)
        fTag[i] = (char) rd8(kVirtioConfig + 2 + i);
    fTag[len] = 0;
    return true;
}

bool io_virtio_Virtio9P::deviceStart()
{
    UInt16 cmd;
    UInt32 availOff, usedOff, features;

    /* The I/O BAR, mapped; failing that, its port number, for IOPCIDevice's own I/O mapping. */
    P9_TRACE("device: map");
    fIO = fPCI->mapDeviceMemoryWithRegister(kConfigBAR0);
    fIOBase = (UInt16) (fPCI->configRead32(kConfigBAR0) & ~(UInt32) 3);
    cmd = fPCI->configRead16(kConfigCommand);
    fPCI->configWrite16(kConfigCommand, cmd | kCommandIO | kCommandBusMaster);

    P9_TRACE("device: reset");
    deviceReset();
    wr8(kVirtioStatus, kStatusAcknowledge);
    wr8(kVirtioStatus, kStatusAcknowledge | kStatusDriver);
    features = rd32(kVirtioHostFeatures);
    wr32(kVirtioGuestFeatures, features & kFeatureMountTag);
    if (!(features & kFeatureMountTag) || !readTag())
        goto fail;

    P9_TRACE("device: queue");
    /* The one queue, requestq. */
    wr16(kVirtioQueueSel, 0);
    fQueueNum = rd16(kVirtioQueueNum);
    if (fQueueNum < 2) {
        IOLog("Virtio9P: the request queue has %d entries\n", fQueueNum);
        goto fail;
    }
    if (rd32(kVirtioQueuePFN) != 0) {
        IOLog("Virtio9P: the request queue is already in use\n");
        goto fail;
    }
    availOff = 16 * fQueueNum;
    usedOff = roundPage(availOff + 6 + 2 * fQueueNum);
    fRingSize = usedOff + roundPage(6 + 8 * fQueueNum);
    P9_TRACE("device: ring");
    fRing = (UInt8 *) IOMallocContiguous(fRingSize, kPage, &fRingPhys);
    if (!fRing)
        goto fail;
    bzero(fRing, fRingSize);
    fDesc       = (volatile Virtio9PDesc *) fRing;
    fAvailFlags = (volatile UInt16 *) (fRing + availOff);
    fAvailIdx   = fAvailFlags + 1;
    fAvailRing  = fAvailFlags + 2;
    fUsedIdx    = (volatile UInt16 *) (fRing + usedOff) + 1;
    fUsedRing   = (volatile Virtio9PUsedElem *) (fRing + usedOff + 4);
    fLastUsed   = 0;
    fNextAvail  = 0;
    *fAvailFlags = kAvailNoInterrupt;   /* polled */

    P9_TRACE("device: buffers");
    /* The two message buffers, and the one descriptor chain that always carries them. */
    for (fBufSize = kBufSize; fBufSize >= kBufSizeMin; fBufSize /= 2) {
        fTBuf = (UInt8 *) IOMallocContiguous(fBufSize, kPage, &fTBufPhys);
        fRBuf = (UInt8 *) IOMallocContiguous(fBufSize, kPage, &fRBufPhys);
        if (fTBuf && fRBuf)
            break;
        if (fTBuf)
            IOFreeContiguous(fTBuf, fBufSize);
        if (fRBuf)
            IOFreeContiguous(fRBuf, fBufSize);
        fTBuf = fRBuf = 0;
    }
    if (!fTBuf)
        goto fail;
    fDesc[0].addr     = fTBufPhys;
    fDesc[0].flags    = kDescNext;
    fDesc[0].next     = 1;
    fDesc[1].addr     = fRBufPhys;
    fDesc[1].len      = fBufSize;
    fDesc[1].flags    = kDescWrite;
    fDesc[1].next     = 0;

    P9_TRACE("device: pfn");
    barrier();
    wr32(kVirtioQueuePFN, fRingPhys / kPage);
    wr8(kVirtioStatus, kStatusAcknowledge | kStatusDriver | kStatusDriverOK);
    fDeviceUp = true;
    IOLog("Virtio9P: mount tag \"%s\", I/O at 0x%x%s, %d-entry queue, %d-byte messages\n",
          fTag, fIOBase, fIO ? "" : " (through the bridge)", fQueueNum, (int) fBufSize);
    return true;

fail:
    wr8(kVirtioStatus, kStatusFailed);
    return false;
}

/* One message out and its reply back. The chain is always descriptors 0 (the request) and 1
 * (the reply), and only one message is ever with the device: the filesystem holds its channel
 * for the length of a call. */
int io_virtio_Virtio9P::rpc(UInt32 tlen, UInt32 *rlen)
{
    if (fStopping || !fDeviceUp)
        return ENXIO;
    if (tlen > fBufSize)
        return EMSGSIZE;
    if (!fInterrupt)
        return rpcPolled(tlen, rlen);
    return (int) fGate->runAction(rpcAction, (void *) (unsigned long) tlen, (void *) rlen);
}

void io_virtio_Virtio9P::publish(UInt32 tlen)
{
    fDesc[0].len = tlen;
    fAvailRing[fNextAvail % fQueueNum] = 0;
    barrier();
    fNextAvail++;
    *fAvailIdx = fNextAvail;
    barrier();
    wr16(kVirtioQueueNotify, 0);
}

void io_virtio_Virtio9P::takeReply(UInt32 *rlen)
{
    volatile Virtio9PUsedElem *e;

    barrier();
    e = &fUsedRing[fLastUsed % fQueueNum];
    *rlen = e->len;
    fLastUsed++;
    if (*rlen > fBufSize)
        *rlen = fBufSize;
}

/* Without an interrupt: a short spin for the quick reply, then millisecond sleeps -- which are a
 * scheduler tick, 10 ms, on the older kernels, and so what a reply slower than the spin waited for
 * before 0.2.0. */
int io_virtio_Virtio9P::rpcPolled(UInt32 tlen, UInt32 *rlen)
{
    UInt32 spins = 0;

    publish(tlen);
    while (*fUsedIdx == fLastUsed) {
        if (fStopping)
            return ENXIO;
        if (spins < kSpins) {
            IODelay(kSpinUS);
            spins++;
        } else
            IOSleep(1);
    }
    takeReply(rlen);
    return 0;
}

IOReturn io_virtio_Virtio9P::rpcAction(OSObject *owner, void *arg0, void *arg1, void *, void *)
{
    return (IOReturn) ((io_virtio_Virtio9P *) owner)->rpcGated((UInt32) (unsigned long) arg0, (UInt32 *) arg1);
}

/* On the work loop: a short spin first, with the device asked for no interrupt, since QEMU's
 * server answers most messages in microseconds; then the caller sleeps on the gate, which lets the
 * interrupt's action and the watchdog in, until one of them sees the reply. Slept for from the
 * start, every message paid a wakeup: on Tiger 10.4.11 a 64 MB read took 0.26 s where the old
 * spin-then-sleep took 0.19 s, a 32 MB write 0.20 s where it took 0.13 (2026-10-14). The spin is
 * what kept those numbers; the sleep replaces the scheduler-tick sleeps a slower reply waited out.
 * Uninterruptible: a caller woken by a signal with its message still with the device would leave
 * it there for the next one's reply. */
int io_virtio_Virtio9P::rpcGated(UInt32 tlen, UInt32 *rlen)
{
    UInt32 spins;

    if (!fPolled)
        *fAvailFlags = kAvailNoInterrupt;
    publish(tlen);
    for (spins = 0; spins < kGatedSpins && *fUsedIdx == fLastUsed; spins++)
        IODelay(kSpinUS);
    if (*fUsedIdx != fLastUsed) {
        takeReply(rlen);
        return 0;
    }
    if (!fPolled) {
        /* Asked for the interrupt now, and looked again: a reply that came in between was told
         * not to raise one. */
        *fAvailFlags = 0;
        fullBarrier();
        if (*fUsedIdx != fLastUsed) {
            takeReply(rlen);
            return 0;
        }
    }
    fWaiting = true;
    fInterruptsSeen = fInterrupts;
    fTimer->setTimeoutMS(fPolled ? kPollMS : kWatchdogMS);
    while (*fUsedIdx == fLastUsed && !fStopping)
        fGate->commandSleep((void *) &fLastUsed, THREAD_UNINT);
    fWaiting = false;
    fTimer->cancelTimeout();
    if (*fUsedIdx == fLastUsed)
        return ENXIO;
    takeReply(rlen);
    return 0;
}

/* At primary interrupt time: reading the ISR lowers the device's interrupt line. */
bool io_virtio_Virtio9P::interruptFilter(OSObject *owner, IOFilterInterruptEventSource *)
{
    io_virtio_Virtio9P *me = (io_virtio_Virtio9P *) owner;

    if (!(me->rd8(kVirtioISR) & kISRQueue))
        return false;   /* someone else's, on a shared line */
    OSIncrementAtomic((SInt32 *) &me->fInterrupts);
    return true;
}

void io_virtio_Virtio9P::interruptAction(OSObject *owner, IOInterruptEventSource *, int)
{
    ((io_virtio_Virtio9P *) owner)->interruptOccurred();
}

void io_virtio_Virtio9P::interruptOccurred()
{
    if (fWaiting && *fUsedIdx != fLastUsed)
        fGate->commandWakeup((void *) &fLastUsed, false);
}

void io_virtio_Virtio9P::timeoutAction(OSObject *owner, IOTimerEventSource *)
{
    ((io_virtio_Virtio9P *) owner)->timeoutOccurred();
}

/* While a caller waits: a reply that is there and that no interrupt announced -- the platform did
 * not route the line -- wakes it, and twice in a row turns polling on for good. */
void io_virtio_Virtio9P::timeoutOccurred()
{
    if (!fWaiting)
        return;
    if (*fUsedIdx != fLastUsed) {
        if (!fPolled && fInterrupts == fInterruptsSeen && ++fQuietTicks >= kQuietTicksMax) {
            IOLog("Virtio9P: replies raise no interrupt; polling every %d ms instead\n", kPollMS);
            fInterrupt->disable();
            *fAvailFlags = kAvailNoInterrupt;
            fPolled = true;
        }
        fGate->commandWakeup((void *) &fLastUsed, false);
        return;
    }
    fQuietTicks = 0;
    fInterruptsSeen = fInterrupts;
    if (fStopping)
        fGate->commandWakeup((void *) &fLastUsed, false);
    else
        fTimer->setTimeoutMS(fPolled ? kPollMS : kWatchdogMS);
}

/* ------------------------------------------------------------------------------------------ */
/* the service                                                                                 */
/* ------------------------------------------------------------------------------------------ */

bool io_virtio_Virtio9P::start(IOService *provider)
{
    if (!super::start(provider))
        return false;

    P9_TRACE("start: the device");
    fPCI = OSDynamicCast(IOPCIDevice, provider);
    if (!fPCI)
        return false;
    fPCI->retain();
    if (!fPCI->open(this))
        goto fail;
    if (!deviceStart())
        goto fail;

    /* The interrupt, unless the personality says Polled: a caller then sleeps until its reply is
     * announced, where it spun and then slept a scheduler tick at a time. */
    {
        OSBoolean *polled = OSDynamicCast(OSBoolean, getProperty("Polled"));

        fWorkLoop = IOWorkLoop::workLoop();
        if (fWorkLoop && !(polled && polled->isTrue())) {
            fGate = IOCommandGate::commandGate(this);
            fTimer = IOTimerEventSource::timerEventSource(this, timeoutAction);
            fInterrupt = IOFilterInterruptEventSource::filterInterruptEventSource(this, interruptAction,
                                                                                  interruptFilter, provider, 0);
            if (!fGate || fWorkLoop->addEventSource(fGate) != kIOReturnSuccess ||
                !fTimer || fWorkLoop->addEventSource(fTimer) != kIOReturnSuccess ||
                !fInterrupt || fWorkLoop->addEventSource(fInterrupt) != kIOReturnSuccess) {
                IOLog("Virtio9P: the device's interrupt cannot be had; polled\n");
                if (fInterrupt) {
                    fWorkLoop->removeEventSource(fInterrupt);
                    fInterrupt->release();
                }
                fInterrupt = 0;
            } else {
                *fAvailFlags = 0;
                fInterrupt->enable();
            }
        }
        IOLog("Virtio9P: %s\n", fInterrupt ? "interrupt-driven" : "polled");
    }

    /* Found by its tag when mount_9p asks for it (p9t_open), and shown in the registry. */
    setProperty("mount_tag", fTag);
    registerService();
    return true;

fail:
    IOLog("Virtio9P: could not start\n");
    stop(provider);
    return false;
}

void io_virtio_Virtio9P::stop(IOService *provider)
{
    fStopping = true;
    if (fInterrupt)
        fInterrupt->disable();
    if (fGate)
        fGate->commandWakeup((void *) &fLastUsed, false);
    if (fPCI && (fIO || fIOBase))
        deviceReset();
    if (fPCI && fPCI->isOpen(this))
        fPCI->close(this);
    super::stop(provider);
}

/* Refused while a share is mounted. kextunload terminates a module's drivers before it unloads
 * the module and gives up at the first that refuses (IOCatalogue's _terminateDrivers, xnu-123
 * to xnu-517); without this it took the device from under the mount, which then answered
 * ENXIO ("Device not configured"), and only the filesystem's own refusal kept the module
 * (live, 10.1.5, 2026-10-01). */
bool io_virtio_Virtio9P::terminate(IOOptionBits options)
{
    if (fClaimed) {
        IOLog("Virtio9P: \"%s\" is mounted; unmount it first\n", fTag);
        return false;
    }
    return super::terminate(options);
}

/* The rings and buffers stay until the last reference goes, which a mount holds: a filesystem
 * whose device has stopped gets ENXIO, not freed memory. */
void io_virtio_Virtio9P::free()
{
    if (fWorkLoop) {
        if (fInterrupt)
            fWorkLoop->removeEventSource(fInterrupt);
        if (fTimer)
            fWorkLoop->removeEventSource(fTimer);
        if (fGate)
            fWorkLoop->removeEventSource(fGate);
    }
    if (fInterrupt)
        fInterrupt->release();
    if (fTimer)
        fTimer->release();
    if (fGate)
        fGate->release();
    if (fWorkLoop)
        fWorkLoop->release();
    if (fRing)
        IOFreeContiguous(fRing, fRingSize);
    if (fTBuf)
        IOFreeContiguous(fTBuf, fBufSize);
    if (fRBuf)
        IOFreeContiguous(fRBuf, fBufSize);
    if (fIO)
        fIO->release();
    if (fPCI)
        fPCI->release();
    super::free();
}

bool io_virtio_Virtio9P::hasTag(const char *tag)
{
    const char *a = fTag;

    if (!fDeviceUp || fStopping)
        return false;
    while (*a && *a == *tag) {
        a++;
        tag++;
    }
    return *a == *tag;
}

int io_virtio_Virtio9P::claim(UInt8 **tbuf, UInt8 **rbuf, UInt32 *size)
{
    if (!OSCompareAndSwap(0, 1, &fClaimed))
        return EBUSY;
    *tbuf = fTBuf;
    *rbuf = fRBuf;
    *size = fBufSize;
    return 0;
}

void io_virtio_Virtio9P::unclaim()
{
    fClaimed = 0;
}

/* ------------------------------------------------------------------------------------------ */
/* p9_transport.h                                                                              */
/* ------------------------------------------------------------------------------------------ */

extern "C" int p9t_open(const char *tag, void **handle, unsigned char **tbuf, unsigned char **rbuf,
                        unsigned int *bufsize)
{
    OSDictionary *match;
    OSIterator *it;
    OSObject *o;
    int err = ENXIO;

    match = IOService::serviceMatching("io_virtio_Virtio9P");
    if (!match)
        return ENOMEM;
    it = IOService::getMatchingServices(match);
    match->release();
    if (!it)
        return ENXIO;
    while ((o = it->getNextObject()) != 0) {
        io_virtio_Virtio9P *dev = OSDynamicCast(io_virtio_Virtio9P, o);
        UInt32 size;
        if (!dev || !dev->hasTag(tag))
            continue;
        err = dev->claim(tbuf, rbuf, &size);
        if (!err) {
            dev->retain();
            *bufsize = size;
            *handle = dev;
        }
        break;
    }
    it->release();
    return err;
}

extern "C" void p9t_close(void *handle)
{
    io_virtio_Virtio9P *dev = (io_virtio_Virtio9P *) handle;

    dev->unclaim();
    dev->release();
}

extern "C" int p9t_rpc(void *handle, unsigned int tlen, unsigned int *rlen)
{
    UInt32 n = 0;
    int err = ((io_virtio_Virtio9P *) handle)->rpc(tlen, &n);

    *rlen = n;
    return err;
}

#ifdef P9FS_KPI
/* ------------------------------------------------------------------------------------------ */
/* Adding the filesystem once BSD is up (Virtio9P_info.c)                                      */
/* ------------------------------------------------------------------------------------------ */

/*
 * The kmod's start may run before the kernel's VFS is: a kext that is prelinked -- injected by
 * OpenCore on 10.10 to 10.13, or in the prelinked kernel 10.7 on build from
 * /System/Library/Extensions -- is started from IOKit's first matching, and vfs_fsadd then
 * dereferenced NULL ("Kernel trap ... type 14=page fault" in _vfs_fsadd from _p9fs_register, from
 * OSKext::start from _IOConfigThread::main, on every boot of 10.10.5, live 2026-10-11). The kernel
 * publishes the IOBSD resource once BSD, VFS among it, is set up; the filesystem is added when it
 * is published, which is at once when the kext is loaded later (kextload).
 */
extern "C" int p9fs_register(void);
extern "C" int p9fs_unregister(void);

static IONotifier *gBSDNotifier;
/* Not volatile: 10.4's OSCompareAndSwap takes a plain UInt32 *, and every other access is a
 * single store. */
static UInt32 gRegistered;

static void p9fs_bsd_up(void)
{
    if (!OSCompareAndSwap(0, 1, &gRegistered))
        return;
    P9_TRACE("register: BSD is up");
    if (p9fs_register() != 0) {
        gRegistered = 0;
        IOLog("Virtio9P: the 9p filesystem could not be added\n");
    }
}

#if defined(__x86_64__)
static bool p9fs_bsd_published(void *target, void *ref, IOService *service, IONotifier *notifier)
{
    p9fs_bsd_up();
    return true;
}
#else
static bool p9fs_bsd_published(void *target, void *ref, IOService *service)
{
    p9fs_bsd_up();
    return true;
}
#endif

/* Asks for the filesystem to be added once BSD is up: 0, or an errno. */
extern "C" int p9fs_register_when_bsd(void)
{
    OSDictionary *match = IOService::resourceMatching("IOBSD");

    if (!match)
        return ENOMEM;
#if defined(__x86_64__)
    /* 10.6's x86_64 KPI: addNotification is gone, and addMatchingNotification does not consume
     * the dictionary. */
    gBSDNotifier = IOService::addMatchingNotification(gIOPublishNotification, match,
                                                      (IOServiceMatchingNotificationHandler) &p9fs_bsd_published,
                                                      0, 0, 0);
    match->release();
#else
    /* 10.4's KPI, which the i386 slice is built against: addNotification consumes it. */
    gBSDNotifier = IOService::addNotification(gIOPublishNotification, match,
                                              (IOServiceNotificationHandler) &p9fs_bsd_published, 0, 0, 0);
#endif
    return gBSDNotifier ? 0 : ENOMEM;
}

/* Takes the filesystem away, or the wish to add it: 0, or EBUSY while a 9p filesystem is mounted,
 * and then it stays. */
extern "C" int p9fs_unregister_when_bsd(void)
{
    if (gBSDNotifier) {
        gBSDNotifier->remove();
        gBSDNotifier = 0;
    }
    if (gRegistered) {
        if (p9fs_unregister() != 0) {
            (void) p9fs_register_when_bsd();
            return EBUSY;
        }
        gRegistered = 0;
    }
    return 0;
}
#endif /* P9FS_KPI */
