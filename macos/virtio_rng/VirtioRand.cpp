/*
 * VirtioRand: see VirtioRand.h for what this is and why it is written the way it is.
 *
 * The virtio side is written from the virtio 0.9.5 (legacy PCI) specification and follows
 * VirtioConsole.cpp.
 */

#include "VirtioRand.h"

#include <IOKit/IOLib.h>

extern "C" {
#include <sys/param.h>
#include <sys/types.h>
#include <sys/uio.h>

#ifdef VIRTIORAND_DEVICE
#include <sys/conf.h>
#include <sys/errno.h>
#include <miscfs/devfs/devfs.h>
/* 10.0's headers declare these for the kernel only. */
extern int uiomove(caddr_t cp, int n, struct uio *uio);
extern int seltrue(dev_t dev, int flag, struct proc *p);
#else
/* The kernel's write routine of /dev/random (bsd/dev/random/randomdev.c): mixes what the uio holds
 * into Yarrow, under its mutex, and forces a reseed. dev and ioflag are not looked at. */
extern int random_write(dev_t dev, struct uio *uio, int ioflag);
#endif
}

#define super IOService
OSDefineMetaClassAndStructors(io_virtio_VirtioRand, IOService)

/* Legacy virtio PCI: the common header in the I/O BAR. IOPCIDevice's ioRead/ioWrite present PCI's
 * little-endian registers in the host's order. */
enum {
    kVirtioHostFeatures  = 0x00,    /* 32 */
    kVirtioGuestFeatures = 0x04,    /* 32 */
    kVirtioQueuePFN      = 0x08,    /* 32 */
    kVirtioQueueNum      = 0x0C,    /* 16 */
    kVirtioQueueSel      = 0x0E,    /* 16 */
    kVirtioQueueNotify   = 0x10,    /* 16 */
    kVirtioStatus        = 0x12,    /*  8 */
    kVirtioISR           = 0x13     /*  8 */
};

enum {
    kStatusAcknowledge = 0x01,
    kStatusDriver      = 0x02,
    kStatusDriverOK    = 0x04,
    kStatusFailed      = 0x80
};

enum {
    kDescWrite         = 2,
    kAvailNoInterrupt  = 1
};

enum {
    kPage              = 4096,
    kBufferSize        = 64,        /* asked of the device at a time */
    kPrimeBytes        = 256,       /* taken back to back at start, before the interval applies */
    kPollMS            = 10,        /* while a request is out */
    kPrimeMS           = 1,         /* between the requests of the opening burst */
    kIntervalDefaultMS = 10000,
    kIntervalMinMS     = 100,
    kStuckTicks        = 500        /* polls, 5 s, after which an unanswered request is reported */
};

/* PCI configuration space, spelled out because 10.0's headers name only some of it. */
enum {
    kConfigCommand     = 0x04,
    kConfigBAR0        = 0x10,
    kCommandIO         = 0x0001,
    kCommandBusMaster  = 0x0004
};

static inline UInt32 roundPage(UInt32 n) { return (n + kPage - 1) & ~(UInt32)(kPage - 1); }

/* Orders memory the device reads against the register write that tells it to, and the used index
 * against the used element read after it. PowerPC's sync; GCC 2.95 and 3 both take this spelling. */
static inline void barrier() { __asm__ __volatile__ ("sync" : : : "memory"); }

/* With the BAR mapped, register offsets are relative to it; without, they are absolute I/O ports,
 * the BAR's own address plus the register. */
UInt8  io_virtio_VirtioRand::rd8(UInt16 off)            { return fPCI->ioRead8((fIO ? 0 : fIOBase) + off, fIO); }
UInt16 io_virtio_VirtioRand::rd16(UInt16 off)           { return fPCI->ioRead16((fIO ? 0 : fIOBase) + off, fIO); }
UInt32 io_virtio_VirtioRand::rd32(UInt16 off)           { return fPCI->ioRead32((fIO ? 0 : fIOBase) + off, fIO); }
void   io_virtio_VirtioRand::wr8(UInt16 off, UInt8 v)   { fPCI->ioWrite8((fIO ? 0 : fIOBase) + off, v, fIO); }
void   io_virtio_VirtioRand::wr16(UInt16 off, UInt16 v) { fPCI->ioWrite16((fIO ? 0 : fIOBase) + off, v, fIO); }
void   io_virtio_VirtioRand::wr32(UInt16 off, UInt32 v) { fPCI->ioWrite32((fIO ? 0 : fIOBase) + off, v, fIO); }

/* ------------------------------------------------------------------------------------------ */
/* the device                                                                                  */
/* ------------------------------------------------------------------------------------------ */

void io_virtio_VirtioRand::deviceReset()
{
    wr8(kVirtioStatus, 0);
}

bool io_virtio_VirtioRand::deviceStart()
{
    UInt32 availOff, usedOff;
    UInt16 cmd;

    /* The I/O BAR, mapped; failing that, its port number, for IOPCIDevice's own I/O mapping. */
    fIO = fPCI->mapDeviceMemoryWithRegister(kConfigBAR0);
    fIOBase = (UInt16) (fPCI->configRead32(kConfigBAR0) & ~(UInt32) 3);
    cmd = fPCI->configRead16(kConfigCommand);
    fPCI->configWrite16(kConfigCommand, cmd | kCommandIO | kCommandBusMaster);

    deviceReset();
    wr8(kVirtioStatus, kStatusAcknowledge);
    wr8(kVirtioStatus, kStatusAcknowledge | kStatusDriver);
    (void) rd32(kVirtioHostFeatures);
    wr32(kVirtioGuestFeatures, 0);   /* the device defines no features */

    wr16(kVirtioQueueSel, 0);
    fNum = rd16(kVirtioQueueNum);
    if (fNum == 0) {
        IOLog("VirtioRand: the device has no request queue\n");
        goto fail;
    }
    if (rd32(kVirtioQueuePFN) != 0) {
        IOLog("VirtioRand: the request queue is already in use\n");
        goto fail;
    }

    availOff = 16 * fNum;
    usedOff = roundPage(availOff + 6 + 2 * fNum);
    fRingSize = usedOff + roundPage(6 + 8 * fNum);
    fRing = (UInt8 *) IOMallocContiguous(fRingSize, kPage, &fRingPhys);
    fBuf = (UInt8 *) IOMallocContiguous(kPage, kPage, &fBufPhys);
    if (!fRing || !fBuf)
        goto fail;
    bzero(fRing, fRingSize);
    bzero(fBuf, kPage);

    fDesc       = (volatile VirtioRandDesc *) fRing;
    fAvailFlags = (volatile UInt16 *) (fRing + availOff);
    fAvailIdx   = fAvailFlags + 1;
    fAvailRing  = fAvailFlags + 2;
    fUsedIdx    = (volatile UInt16 *) (fRing + usedOff) + 1;
    fUsedRing   = (volatile VirtioRandUsedElem *) (fRing + usedOff + 4);
    *fAvailFlags = kAvailNoInterrupt;

    fDesc[0].addrHigh = 0;
    fDesc[0].addrLow  = fBufPhys;
    fDesc[0].len      = kBufferSize;
    fDesc[0].flags    = kDescWrite;
    fDesc[0].next     = 0;
    barrier();
    wr32(kVirtioQueuePFN, fRingPhys / kPage);
    wr8(kVirtioStatus, kStatusAcknowledge | kStatusDriver | kStatusDriverOK);
    return true;

fail:
    wr8(kVirtioStatus, kStatusFailed);
    return false;
}

/* Gives the buffer to the device and tells it so. */
void io_virtio_VirtioRand::request()
{
    fDesc[0].len = kBufferSize;
    fAvailRing[fNextAvail % fNum] = 0;
    barrier();
    fNextAvail++;
    *fAvailIdx = fNextAvail;
    barrier();
    wr16(kVirtioQueueNotify, 0);
    fPending = true;
    fWaitTicks = 0;
}

/* Mixes len bytes of the buffer into the kernel's generator, as a write to /dev/random would, and
 * wipes the buffer: nothing keeps the bytes around once they are in. */
#ifdef VIRTIORAND_DEVICE
/* Adds len bytes of the buffer to the pool, as far as there is room, and wipes the buffer. */
void io_virtio_VirtioRand::feed(UInt32 len)
{
    UInt32 k;

    IOSimpleLockLock(fPoolLock);
    for (k = 0; k < len && fPoolLen < kPoolSize; k++) {
        fPool[(fPoolHead + fPoolLen) % kPoolSize] = fBuf[k];
        fPoolLen++;
    }
    IOSimpleLockUnlock(fPoolLock);
    bzero(fBuf, kBufferSize);
    fBytes += k;
}

/* A read of /dev/random or /dev/urandom: bytes out of the pool, waiting for the device while it is
 * empty (the timer refills it every few milliseconds while it is not full). Bytes given out are
 * wiped from the pool. */
int io_virtio_VirtioRand::devRead(struct uio *uio)
{
    UInt8 tmp[64];
    int waited = 0, err;

    while (uio->uio_resid > 0) {
        UInt32 n = 0, want = uio->uio_resid > (int) sizeof(tmp) ? sizeof(tmp) : (UInt32) uio->uio_resid;

        IOSimpleLockLock(fPoolLock);
        while (n < want && fPoolLen > 0) {
            tmp[n++] = fPool[fPoolHead];
            fPool[fPoolHead] = 0;
            fPoolHead = (fPoolHead + 1) % kPoolSize;
            fPoolLen--;
        }
        IOSimpleLockUnlock(fPoolLock);
        if (n == 0) {
            if (fStopping || waited >= 10000)
                return EIO;         /* the device is gone, or has stopped answering */
            IOSleep(10);
            waited += 10;
            continue;
        }
        waited = 0;
        err = uiomove((caddr_t) tmp, n, uio);
        bzero(tmp, sizeof(tmp));
        if (err)
            return err;
    }
    return 0;
}

static io_virtio_VirtioRand *gRand;   /* the instance the nodes read from */

static int randOpenClose(dev_t, int, int, struct proc *) { return 0; }

static int randRead(dev_t, struct uio *uio, int)
{
    return gRand ? gRand->devRead(uio) : ENXIO;
}

/* Taken and dropped: what a process writes is not the host's to know, and adds nothing to it. */
static int randWrite(dev_t, struct uio *uio, int)
{
    char sink[64];
    int err = 0;

    while (uio->uio_resid > 0 && !err) {
        int n = uio->uio_resid > (int) sizeof(sink) ? (int) sizeof(sink) : uio->uio_resid;

        err = uiomove(sink, n, uio);
    }
    bzero(sink, sizeof(sink));
    return err;
}

static struct cdevsw gRandSW = {
    (open_close_fcn_t *) randOpenClose, (open_close_fcn_t *) randOpenClose,
    (read_write_fcn_t *) randRead,      (read_write_fcn_t *) randWrite,
    eno_ioctl, eno_stop, eno_reset, 0,
    (select_fcn_t *) seltrue, eno_mmap, eno_strat, eno_getc, eno_putc, 0
};

bool io_virtio_VirtioRand::devPublish()
{
    if (gRand)
        return false;       /* a second device: the first one serves */
    fMajor = cdevsw_add(-1, &gRandSW);
    if (fMajor < 0) {
        IOLog("VirtioRand: no character device major left\n");
        return false;
    }
    gRand = this;
    fNodeRandom  = devfs_make_node(makedev(fMajor, 0), DEVFS_CHAR, UID_ROOT, GID_WHEEL, 0666, "random");
    fNodeURandom = devfs_make_node(makedev(fMajor, 1), DEVFS_CHAR, UID_ROOT, GID_WHEEL, 0666, "urandom");
    IOLog("VirtioRand: /dev/random and /dev/urandom (major %d), served from the device\n", fMajor);
    return true;
}

void io_virtio_VirtioRand::devUnpublish()
{
    if (gRand != this)
        return;
    if (fNodeRandom)
        devfs_remove(fNodeRandom);
    if (fNodeURandom)
        devfs_remove(fNodeURandom);
    fNodeRandom = fNodeURandom = 0;
    cdevsw_remove(fMajor, &gRandSW);
    fMajor = -1;
    gRand = 0;
}
#else
void io_virtio_VirtioRand::feed(UInt32 len)
{
    struct iovec iov;
    struct uio uio;
    int err;

    iov.iov_base = (caddr_t) fBuf;
    iov.iov_len = len;
    bzero(&uio, sizeof(uio));
    uio.uio_iov = &iov;
    uio.uio_iovcnt = 1;
    uio.uio_offset = 0;
    uio.uio_resid = len;
    uio.uio_segflg = UIO_SYSSPACE;
    uio.uio_rw = UIO_WRITE;
    uio.uio_procp = 0;
    err = random_write(0, &uio, 0);
    bzero(fBuf, kBufferSize);
    if (err) {
        if (!fFeedFailed)
            IOLog("VirtioRand: the kernel's generator refused the bytes (error %d)\n", err);
        fFeedFailed = true;
        return;
    }
    fFeedFailed = false;
    fBytes += len;
}
#endif

void io_virtio_VirtioRand::publishCounts()
{
    OSNumber *n;

    if ((n = OSNumber::withNumber(fBytes, 32))) {
        setProperty("BytesFed", n);
        n->release();
    }
    if ((n = OSNumber::withNumber(fRequests, 32))) {
        setProperty("Requests", n);
        n->release();
    }
}

void io_virtio_VirtioRand::tick()
{
    UInt32 delay = kPollMS, len;

    if (fStopping)
        return;
    if (!fPending) {
        request();
    } else if (fLastUsed != *fUsedIdx) {
        barrier();
        len = fUsedRing[fLastUsed % fNum].len;
        fLastUsed++;
        fPending = false;
        if (len > kBufferSize)
            len = kBufferSize;
        if (len)
            feed(len);
        fRequests++;
        publishCounts();
        if (fPrimed < kPrimeBytes) {
            fPrimed += len ? len : kBufferSize;     /* an empty answer still counts, or this would never end */
            delay = kPrimeMS;
            if (fPrimed >= kPrimeBytes)
                IOLog("VirtioRand: primed the generator with %d bytes from the device\n", (int) fBytes);
        } else
            delay = fIntervalMS;
#ifdef VIRTIORAND_DEVICE
        if (fPoolLen < kPoolSize)
            delay = kPrimeMS;   /* the pool refills as fast as the device gives */
#endif
    } else if (++fWaitTicks == kStuckTicks)
        IOLog("VirtioRand: the device has not answered for %d ms\n", kStuckTicks * kPollMS);
    fTimer->setTimeoutMS(delay);
}

void io_virtio_VirtioRand::tickAction(OSObject *owner, IOTimerEventSource *)
{
    ((io_virtio_VirtioRand *) owner)->tick();
}

/* ------------------------------------------------------------------------------------------ */
/* the service                                                                                 */
/* ------------------------------------------------------------------------------------------ */

bool io_virtio_VirtioRand::start(IOService *provider)
{
    OSNumber *interval;

    if (!super::start(provider))
        return false;

    fPCI = OSDynamicCast(IOPCIDevice, provider);
    if (!fPCI)
        return false;
    fPCI->retain();
    if (!fPCI->open(this))
        goto fail;

#ifdef VIRTIORAND_DEVICE
    fMajor = -1;
    fPoolLock = IOSimpleLockAlloc();
    if (!fPoolLock)
        goto fail;
#endif
    fIntervalMS = kIntervalDefaultMS;
    interval = OSDynamicCast(OSNumber, getProperty("IntervalMS"));
    if (interval && interval->unsigned32BitValue() >= kIntervalMinMS)
        fIntervalMS = interval->unsigned32BitValue();

    fWorkLoop = IOWorkLoop::workLoop();
    if (!fWorkLoop)
        goto fail;
    fTimer = IOTimerEventSource::timerEventSource(this, tickAction);
    if (!fTimer || fWorkLoop->addEventSource(fTimer) != kIOReturnSuccess)
        goto fail;

    if (!deviceStart())
        goto fail;

    IOLog("VirtioRand: I/O at 0x%x%s, request queue %d entries, %d bytes at a time every %d ms\n",
          fIOBase, fIO ? "" : " (through the bridge)", fNum, kBufferSize, (int) fIntervalMS);
    publishCounts();
#ifdef VIRTIORAND_DEVICE
    devPublish();
#endif
    registerService();
    fTimer->setTimeoutMS(kPrimeMS);
    return true;

fail:
    IOLog("VirtioRand: could not start\n");
    stop(provider);
    return false;
}

void io_virtio_VirtioRand::stop(IOService *provider)
{
    fStopping = true;
#ifdef VIRTIORAND_DEVICE
    devUnpublish();
#endif
    if (fTimer)
        fTimer->cancelTimeout();
    if (fPCI && (fIO || fIOBase))
        deviceReset();
    if (fPCI && fPCI->isOpen(this))
        fPCI->close(this);
    super::stop(provider);
}

void io_virtio_VirtioRand::free()
{
    if (fWorkLoop && fTimer)
        fWorkLoop->removeEventSource(fTimer);
    if (fTimer)
        fTimer->release();
    if (fWorkLoop)
        fWorkLoop->release();
    if (fRing)
        IOFreeContiguous(fRing, fRingSize);
    if (fBuf)
        IOFreeContiguous(fBuf, kPage);
#ifdef VIRTIORAND_DEVICE
    if (fPoolLock)
        IOSimpleLockFree(fPoolLock);
#endif
    if (fIO)
        fIO->release();
    if (fPCI)
        fPCI->release();
    super::free();
}
