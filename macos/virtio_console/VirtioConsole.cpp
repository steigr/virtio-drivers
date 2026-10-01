/*
 * VirtioConsole: see VirtioConsole.h for what this is and why it is written the way it is.
 *
 * The port's gated entry points -- watchStateGated above all, and the acquire/release and
 * sleep-until-ready loops -- follow Apple16X50UARTSync.cpp from Apple's Apple16X50Serial
 * (apple-oss-distributions/Apple16X50Serial, tag Apple16X50Serial-10.5), which Apple published
 * under its sample-code licence; the virtio side is written from the virtio 0.9.5 (legacy PCI)
 * specification.
 */

#include "VirtioConsole.h"

#include <IOKit/IOLib.h>
#include <IOKit/serial/IOSerialKeys.h>

#define super IOSerialDriverSync
OSDefineMetaClassAndStructors(io_virtio_VirtioConsole, IOSerialDriverSync)

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
    kDescNext          = 1,
    kDescWrite         = 2,
    kAvailNoInterrupt  = 1
};

enum {
    kPage              = 4096,
    kRxBuffers         = 16,        /* posted to the device at once */
    kRxBufferSize      = 1024,
    kTxBufferSize      = 4096,
    kPortQueueSize     = 16384,     /* each direction of the port */
    kPollMS            = 10
};

/* PCI configuration space, spelled out because 10.0's headers name only some of it. */
enum {
    kConfigCommand     = 0x04,
    kConfigBAR0        = 0x10,
    kCommandIO         = 0x0001,
    kCommandBusMaster  = 0x0004
};

/* The state bits this driver owns, as against those the port's client sets. */
#define kQueueStateMask (PD_S_RXQ_MASK | PD_S_TXQ_MASK | PD_S_TX_BUSY)
#define kLineAlwaysUp   (PD_RS232_S_CAR | PD_RS232_S_CTS | PD_RS232_S_DSR)

static inline UInt32 roundPage(UInt32 n) { return (n + kPage - 1) & ~(UInt32)(kPage - 1); }

/* Orders memory the device reads against the register write that tells it to. PowerPC's sync;
 * GCC 2.95 and 3 both take this spelling. */
static inline void barrier() { __asm__ __volatile__ ("sync" : : : "memory"); }

/* ------------------------------------------------------------------------------------------ */
/* the device                                                                                  */
/* ------------------------------------------------------------------------------------------ */

static UInt16 rd16(IOPCIDevice *pci, IOMemoryMap *map, UInt16 base, UInt16 off) { return pci->ioRead16(base + off, map); }
static UInt32 rd32(IOPCIDevice *pci, IOMemoryMap *map, UInt16 base, UInt16 off) { return pci->ioRead32(base + off, map); }
static void   wr8 (IOPCIDevice *pci, IOMemoryMap *map, UInt16 base, UInt16 off, UInt8  v) { pci->ioWrite8 (base + off, v, map); }
static void   wr16(IOPCIDevice *pci, IOMemoryMap *map, UInt16 base, UInt16 off, UInt16 v) { pci->ioWrite16(base + off, v, map); }
static void   wr32(IOPCIDevice *pci, IOMemoryMap *map, UInt16 base, UInt16 off, UInt32 v) { pci->ioWrite32(base + off, v, map); }

/* With the BAR mapped, register offsets are relative to it; without, they are absolute I/O
 * ports, the BAR's own address plus the register. */
static UInt16 gIOBase;
#define RD16(o)    rd16(fPCI, fIO, fIO ? 0 : gIOBase, (o))
#define RD32(o)    rd32(fPCI, fIO, fIO ? 0 : gIOBase, (o))
#define WR8(o, v)  wr8 (fPCI, fIO, fIO ? 0 : gIOBase, (o), (v))
#define WR16(o, v) wr16(fPCI, fIO, fIO ? 0 : gIOBase, (o), (v))
#define WR32(o, v) wr32(fPCI, fIO, fIO ? 0 : gIOBase, (o), (v))

void io_virtio_VirtioConsole::deviceReset()
{
    WR8(kVirtioStatus, 0);
    fDeviceUp = false;
}

bool io_virtio_VirtioConsole::queueSetup(Virtq *q, UInt16 index, UInt32 bufSize, UInt16 bufCount, bool deviceWrites)
{
    UInt32 availOff, usedOff, i;

    WR16(kVirtioQueueSel, index);
    q->index = index;
    q->num = RD16(kVirtioQueueNum);
    if (q->num == 0) {
        IOLog("VirtioConsole: the device has no queue %d\n", index);
        return false;
    }
    if (RD32(kVirtioQueuePFN) != 0) {
        IOLog("VirtioConsole: queue %d is already in use\n", index);
        return false;
    }
    if (bufCount > q->num)
        bufCount = q->num;

    availOff = 16 * q->num;
    usedOff = roundPage(availOff + 6 + 2 * q->num);
    q->memSize = usedOff + roundPage(6 + 8 * q->num);
    q->mem = (UInt8 *) IOMallocContiguous(q->memSize, kPage, &q->memPhys);
    if (!q->mem)
        return false;
    bzero(q->mem, q->memSize);

    q->desc       = (volatile VirtqDesc *) q->mem;
    q->availFlags = (volatile UInt16 *) (q->mem + availOff);
    q->availIdx   = q->availFlags + 1;
    q->availRing  = q->availFlags + 2;
    q->usedFlags  = (volatile UInt16 *) (q->mem + usedOff);
    q->usedIdx    = q->usedFlags + 1;
    q->usedRing   = (volatile VirtqUsedElem *) (q->mem + usedOff + 4);
    q->lastUsed   = 0;
    q->nextAvail  = 0;
    *q->availFlags = kAvailNoInterrupt;   /* polled: the device need not interrupt */

    q->bufSize  = bufSize;
    q->bufCount = bufCount;
    q->bufsSize = roundPage(bufSize * bufCount);
    q->bufs = (UInt8 *) IOMallocContiguous(q->bufsSize, kPage, &q->bufsPhys);
    if (!q->bufs)
        return false;
    bzero(q->bufs, q->bufsSize);
    for (i = 0; i < bufCount; i++) {
        q->desc[i].addrHigh = 0;
        q->desc[i].addrLow  = q->bufsPhys + i * bufSize;
        q->desc[i].len      = bufSize;
        q->desc[i].flags    = deviceWrites ? kDescWrite : 0;
        q->desc[i].next     = 0;
    }
    barrier();
    WR32(kVirtioQueuePFN, q->memPhys / kPage);
    return true;
}

void io_virtio_VirtioConsole::queueFree(Virtq *q)
{
    if (q->mem)
        IOFreeContiguous(q->mem, q->memSize);
    if (q->bufs)
        IOFreeContiguous(q->bufs, q->bufsSize);
    q->mem = 0;
    q->bufs = 0;
}

/* Hands descriptor id to the device and tells it so. */
void io_virtio_VirtioConsole::queuePublish(Virtq *q, UInt16 id)
{
    q->availRing[q->nextAvail % q->num] = id;
    barrier();
    q->nextAvail++;
    *q->availIdx = q->nextAvail;
    barrier();
    WR16(kVirtioQueueNotify, q->index);
}

bool io_virtio_VirtioConsole::deviceStart()
{
    UInt16 cmd, i;

    /* The I/O BAR, mapped; failing that, its port number, for IOPCIDevice's own I/O mapping. */
    fIO = fPCI->mapDeviceMemoryWithRegister(kConfigBAR0);
    gIOBase = (UInt16) (fPCI->configRead32(kConfigBAR0) & ~(UInt32) 3);
    cmd = fPCI->configRead16(kConfigCommand);
    fPCI->configWrite16(kConfigCommand, cmd | kCommandIO | kCommandBusMaster);
    IOLog("VirtioConsole: I/O at 0x%x%s\n", gIOBase, fIO ? ", mapped" : ", through the bridge");

    deviceReset();
    WR8(kVirtioStatus, kStatusAcknowledge);
    WR8(kVirtioStatus, kStatusAcknowledge | kStatusDriver);
    (void) RD32(kVirtioHostFeatures);
    WR32(kVirtioGuestFeatures, 0);   /* no MULTIPORT: port 0 alone, on queues 0 and 1 */

    if (!queueSetup(&fRx, 0, kRxBufferSize, kRxBuffers, true) || !queueSetup(&fTx, 1, kTxBufferSize, 1, false)) {
        WR8(kVirtioStatus, kStatusFailed);
        return false;
    }
    WR8(kVirtioStatus, kStatusAcknowledge | kStatusDriver | kStatusDriverOK);

    for (i = 0; i < fRx.bufCount; i++)
        queuePublish(&fRx, i);
    fTxBusy = false;
    fDeviceUp = true;
    IOLog("VirtioConsole: receive queue %d entries, transmit queue %d\n", fRx.num, fTx.num);
    return true;
}

/* Moves what the device has received into the port's receive queue, and gives each buffer back.
 * A buffer that does not fit yet is left where it is for the next poll. */
bool io_virtio_VirtioConsole::pollRx()
{
    bool moved = false;

    while (fRx.lastUsed != *fRx.usedIdx) {
        volatile VirtqUsedElem *e;
        UInt32 id, len, room, i;
        UInt8 *src;

        barrier();
        e = &fRx.usedRing[fRx.lastUsed % fRx.num];
        id = e->id;
        len = e->len;
        if (id >= fRx.bufCount)
            break;
        if (len > fRx.bufSize)
            len = fRx.bufSize;
        room = fRxQ.size - fRxQ.count;
        if (len > room)
            break;
        src = fRx.bufs + id * fRx.bufSize;
        for (i = 0; i < len; i++)
            fRxQ.buf[(fRxQ.head + fRxQ.count + i) % fRxQ.size] = src[i];
        fRxQ.count += len;
        fRx.lastUsed++;
        queuePublish(&fRx, (UInt16) id);
        moved = true;
    }
    return moved;
}

/* Takes back the transmit buffer once the device is done with it, and sends the next one. */
bool io_virtio_VirtioConsole::pollTx()
{
    bool moved = false;
    UInt32 n, i;

    if (fTxBusy) {
        if (fTx.lastUsed == *fTx.usedIdx)
            return false;
        barrier();
        fTx.lastUsed = *fTx.usedIdx;
        fTxBusy = false;
        moved = true;
    }
    if (fTxQ.count == 0 || !(fState & PD_S_ACTIVE))
        return moved;

    n = fTxQ.count < fTx.bufSize ? fTxQ.count : fTx.bufSize;
    for (i = 0; i < n; i++)
        fTx.bufs[i] = fTxQ.buf[(fTxQ.head + i) % fTxQ.size];
    fTxQ.head = (fTxQ.head + n) % fTxQ.size;
    fTxQ.count -= n;
    fTx.desc[0].len = n;
    fTxBusy = true;
    queuePublish(&fTx, 0);
    return true;
}

void io_virtio_VirtioConsole::poll()
{
    bool moved;

    if (!fDeviceUp || fStopping)
        return;
    moved = pollRx();
    if (pollTx())
        moved = true;
    if (moved)
        setStateGated(queueState(), kQueueStateMask);
    if (fState & PD_S_ACTIVE)
        fTimer->setTimeoutMS(kPollMS);
}

void io_virtio_VirtioConsole::pollAction(OSObject *owner, IOTimerEventSource *)
{
    ((io_virtio_VirtioConsole *) owner)->poll();
}

/* ------------------------------------------------------------------------------------------ */
/* the service                                                                                 */
/* ------------------------------------------------------------------------------------------ */

static bool byteQueueInit(ByteQueue *q, UInt32 size)
{
    q->buf = (UInt8 *) IOMalloc(size);
    if (!q->buf)
        return false;
    q->size = size;
    q->head = 0;
    q->count = 0;
    q->lowWater = size / 4;
    q->highWater = (size * 3) / 4;
    return true;
}

static void byteQueueFree(ByteQueue *q)
{
    if (q->buf)
        IOFree(q->buf, q->size);
    q->buf = 0;
}

bool io_virtio_VirtioConsole::start(IOService *provider)
{
    if (!super::start(provider))
        return false;

    fPCI = OSDynamicCast(IOPCIDevice, provider);
    if (!fPCI)
        return false;
    fPCI->retain();
    if (!fPCI->open(this))
        goto fail;

    fDataRate = 9600 << 1;
    fDataSize = 8 << 1;
    fParity = PD_RS232_PARITY_NONE;
    fStopBits = 1 << 1;
    fXOn = 0x11;
    fXOff = 0x13;

    if (!byteQueueInit(&fRxQ, kPortQueueSize) || !byteQueueInit(&fTxQ, kPortQueueSize))
        goto fail;

    fWorkLoop = IOWorkLoop::workLoop();
    if (!fWorkLoop)
        goto fail;
    fGate = IOCommandGate::commandGate(this);
    if (!fGate || fWorkLoop->addEventSource(fGate) != kIOReturnSuccess)
        goto fail;
    fTimer = IOTimerEventSource::timerEventSource(this, pollAction);
    if (!fTimer || fWorkLoop->addEventSource(fTimer) != kIOReturnSuccess)
        goto fail;

    if (!deviceStart())
        goto fail;

    /* The port, as IOSerialFamily's BSD client finds it: /dev/cu.virtio-0 and /dev/tty.virtio-0. */
    fNub = new IORS232SerialStreamSync;
    if (!fNub || !fNub->init(0, 0) || !fNub->attach(this))
        goto fail;
    fNub->setProperty(kIOTTYBaseNameKey, "virtio-");
    fNub->setProperty(kIOTTYSuffixKey, "0");
    fNub->registerService();
    return true;

fail:
    IOLog("VirtioConsole: could not start\n");
    stop(provider);
    return false;
}

void io_virtio_VirtioConsole::stop(IOService *provider)
{
    fStopping = true;
    if (fGate) {
        fGate->commandWakeup((void *) &fAcquired, false);
        fGate->commandWakeup((void *) &fState, false);
    }
    if (fTimer)
        fTimer->cancelTimeout();
    if (fNub) {
        fNub->detach(this);
        fNub->release();
        fNub = 0;
    }
    if (fPCI && (fIO || gIOBase))
        deviceReset();
    if (fPCI && fPCI->isOpen(this))
        fPCI->close(this);
    super::stop(provider);
}

void io_virtio_VirtioConsole::free()
{
    if (fWorkLoop) {
        if (fTimer)
            fWorkLoop->removeEventSource(fTimer);
        if (fGate)
            fWorkLoop->removeEventSource(fGate);
    }
    if (fTimer)
        fTimer->release();
    if (fGate)
        fGate->release();
    if (fWorkLoop)
        fWorkLoop->release();
    queueFree(&fRx);
    queueFree(&fTx);
    byteQueueFree(&fRxQ);
    byteQueueFree(&fTxQ);
    if (fIO)
        fIO->release();
    if (fPCI)
        fPCI->release();
    super::free();
}

/* ------------------------------------------------------------------------------------------ */
/* the port                                                                                    */
/* ------------------------------------------------------------------------------------------ */

UInt32 io_virtio_VirtioConsole::queueState()
{
    UInt32 s = 0;

    if (fRxQ.count == 0)                s |= PD_S_RXQ_EMPTY;
    if (fRxQ.count <= fRxQ.lowWater)    s |= PD_S_RXQ_LOW_WATER;
    if (fRxQ.count >= fRxQ.highWater)   s |= PD_S_RXQ_HIGH_WATER;
    if (fRxQ.count == fRxQ.size)        s |= PD_S_RXQ_FULL;
    if (fTxQ.count == 0 && !fTxBusy)    s |= PD_S_TXQ_EMPTY;
    if (fTxQ.count <= fTxQ.lowWater)    s |= PD_S_TXQ_LOW_WATER;
    if (fTxQ.count >= fTxQ.highWater)   s |= PD_S_TXQ_HIGH_WATER;
    if (fTxQ.count == fTxQ.size)        s |= PD_S_TXQ_FULL;
    if (fTxBusy)                        s |= PD_S_TX_BUSY;
    return s;
}

void io_virtio_VirtioConsole::setStateGated(UInt32 state, UInt32 mask)
{
    UInt32 delta;

    state = (fState & ~mask) | (state & mask);
    delta = state ^ fState;
    fState = state;
    if (delta & fWatchMask)
        fGate->commandWakeup((void *) &fState, false);
}

void io_virtio_VirtioConsole::activate(bool on)
{
    if (on) {
        if (fState & PD_S_ACTIVE)
            return;
        setStateGated(PD_S_ACTIVE, PD_S_ACTIVE);
        fTimer->setTimeoutMS(kPollMS);
    } else {
        if (!(fState & PD_S_ACTIVE))
            return;
        fTimer->cancelTimeout();
        fRxQ.head = fRxQ.count = 0;
        fTxQ.head = fTxQ.count = 0;
        setStateGated(queueState(), PD_S_ACTIVE | kQueueStateMask);
    }
}

IOReturn io_virtio_VirtioConsole::acquirePort(bool sleep, void *)
{
    if (fStopping)
        return kIOReturnOffline;
    return fGate->runAction(acquirePortAction, (void *) sleep);
}

IOReturn io_virtio_VirtioConsole::acquirePortAction(OSObject *owner, void *arg0, void *, void *, void *)
{
    return ((io_virtio_VirtioConsole *) owner)->acquirePortGated(arg0 != 0);
}

IOReturn io_virtio_VirtioConsole::acquirePortGated(bool sleep)
{
    while (fAcquired) {
        IOReturn rtn;
        if (!sleep)
            return kIOReturnExclusiveAccess;
        rtn = fGate->commandSleep((void *) &fAcquired);
        if (fStopping)
            return kIOReturnOffline;
        if (rtn != THREAD_AWAKENED)
            return kIOReturnAborted;
    }
    fAcquired = true;
    fRxQ.head = fRxQ.count = 0;
    fTxQ.head = fTxQ.count = 0;
    setStateGated(PD_S_ACQUIRED | PD_S_TX_ENABLE | PD_S_RX_ENABLE | kLineAlwaysUp | queueState(),
                  PD_RS232_S_MASK | PD_S_MASK);
    return kIOReturnSuccess;
}

IOReturn io_virtio_VirtioConsole::releasePort(void *)
{
    return fGate->runAction(releasePortAction);
}

IOReturn io_virtio_VirtioConsole::releasePortAction(OSObject *owner, void *, void *, void *, void *)
{
    return ((io_virtio_VirtioConsole *) owner)->releasePortGated();
}

IOReturn io_virtio_VirtioConsole::releasePortGated()
{
    if (!fAcquired)
        return kIOReturnNotOpen;
    activate(false);
    setStateGated(0, PD_RS232_S_MASK | PD_S_MASK);
    fAcquired = false;
    fGate->commandWakeup((void *) &fAcquired, false);
    return kIOReturnSuccess;
}

IOReturn io_virtio_VirtioConsole::setState(UInt32 state, UInt32 mask, void *)
{
    if (!fAcquired)
        return kIOReturnOffline;
    if (mask & (PD_S_ACQUIRED | PD_S_ACTIVE))
        return kIOReturnBadArgument;
    mask &= ~(kQueueStateMask | kLineAlwaysUp);   /* the driver's, and the line's inputs */
    if (mask)
        fGate->runAction(setStateAction, (void *) state, (void *) mask);
    return kIOReturnSuccess;
}

IOReturn io_virtio_VirtioConsole::setStateAction(OSObject *owner, void *arg0, void *arg1, void *, void *)
{
    ((io_virtio_VirtioConsole *) owner)->setStateGated((UInt32) arg0, (UInt32) arg1);
    return kIOReturnSuccess;
}

UInt32 io_virtio_VirtioConsole::getState(void *)
{
    return fState;
}

IOReturn io_virtio_VirtioConsole::watchState(UInt32 *state, UInt32 mask, void *)
{
    if (!state)
        return kIOReturnBadArgument;
    if (!mask)
        return kIOReturnSuccess;
    if (fStopping)
        return kIOReturnOffline;
    return fGate->runAction(watchStateAction, (void *) state, (void *) mask);
}

IOReturn io_virtio_VirtioConsole::watchStateAction(OSObject *owner, void *arg0, void *arg1, void *, void *)
{
    return ((io_virtio_VirtioConsole *) owner)->watchStateGated((UInt32 *) arg0, (UInt32) arg1);
}

/* Waits until at least one of the bits in mask equals its value in *state. Unless the caller
 * watches ACQUIRED or ACTIVE itself, a port going inactive ends the wait too, as an I/O error.
 *
 * An I/O error and not kIOReturnOffline, which is what Apple16X50Serial returns: 10.0's and 10.1's
 * IOSerialBSDClient (IOSerialFamily-6 and -7) end their tty threads only on kIOReturnIOError while
 * it is killing them, and loop on anything else. Closing the port deactivates it first, so its
 * transmit thread got offline back at once, called again, forever, and the close that waited for
 * it never returned: 10.0 hung on every reboot and shutdown once its agent's tty was open, a CPU
 * spinning in the gate's lock (live, 2026-09-30). IOSerialFamily-11 on also ends them on an I/O
 * error while killing them. */
IOReturn io_virtio_VirtioConsole::watchStateGated(UInt32 *state, UInt32 mask)
{
    bool watchForInactive = false;
    IOReturn rtn;

    if (!(mask & (PD_S_ACQUIRED | PD_S_ACTIVE))) {
        *state &= ~PD_S_ACTIVE;
        mask |= PD_S_ACTIVE;
        watchForInactive = true;
    }
    while (true) {
        if (mask & ~(*state ^ fState)) {
            *state = fState;
            rtn = (watchForInactive && !(fState & PD_S_ACTIVE)) ? kIOReturnIOError : kIOReturnSuccess;
            break;
        }
        fWatchMask |= mask;
        rtn = fGate->commandSleep((void *) &fState);
        if (fStopping) {
            rtn = kIOReturnOffline;
            break;
        }
        if (rtn == THREAD_TIMED_OUT) {
            rtn = kIOReturnTimeout;
            break;
        }
        if (rtn == THREAD_INTERRUPTED) {
            rtn = kIOReturnAborted;
            break;
        }
    }
    /* The mask cannot be narrowed back to the other sleepers' interests, so it is cleared and
     * they are all woken to register them again. */
    fWatchMask = 0;
    fGate->commandWakeup((void *) &fState, false);
    return rtn;
}

UInt32 io_virtio_VirtioConsole::nextEvent(void *)
{
    if (!fAcquired)
        return PD_E_EOQ;
    return (UInt32) fGate->runAction(nextEventAction);
}

IOReturn io_virtio_VirtioConsole::nextEventAction(OSObject *owner, void *, void *, void *, void *)
{
    io_virtio_VirtioConsole *me = (io_virtio_VirtioConsole *) owner;
    return (IOReturn) (me->fRxQ.count ? PD_E_VALID_DATA_BYTE : PD_E_EOQ);
}

IOReturn io_virtio_VirtioConsole::executeEvent(UInt32 event, UInt32 data, void *)
{
    if (!fAcquired)
        return kIOReturnOffline;
    return fGate->runAction(executeEventAction, (void *) event, (void *) data);
}

IOReturn io_virtio_VirtioConsole::executeEventAction(OSObject *owner, void *arg0, void *arg1, void *, void *)
{
    return ((io_virtio_VirtioConsole *) owner)->executeEventGated((UInt32) arg0, (UInt32) arg1);
}

/* A virtual line: everything a UART would be told is accepted and remembered, and changes nothing
 * about how bytes travel. */
IOReturn io_virtio_VirtioConsole::executeEventGated(UInt32 event, UInt32 data)
{
    switch (event) {
    case PD_E_ACTIVE:
        activate(data != 0);
        break;
    case PD_E_TXQ_FLUSH:
        fTxQ.head = fTxQ.count = 0;
        setStateGated(queueState(), kQueueStateMask);
        break;
    case PD_E_RXQ_FLUSH:
        fRxQ.head = fRxQ.count = 0;
        setStateGated(queueState(), kQueueStateMask);
        break;
    case PD_E_TXQ_SIZE:
    case PD_E_RXQ_SIZE:
        break;   /* fixed */
    case PD_E_TXQ_LOW_WATER:  fTxQ.lowWater = data;  setStateGated(queueState(), kQueueStateMask); break;
    case PD_E_TXQ_HIGH_WATER: fTxQ.highWater = data; setStateGated(queueState(), kQueueStateMask); break;
    case PD_E_RXQ_LOW_WATER:  fRxQ.lowWater = data;  setStateGated(queueState(), kQueueStateMask); break;
    case PD_E_RXQ_HIGH_WATER: fRxQ.highWater = data; setStateGated(queueState(), kQueueStateMask); break;
    case PD_E_DATA_RATE:        fDataRate = data;    break;
    case PD_E_DATA_SIZE:        fDataSize = data;    break;
    case PD_E_DATA_INTEGRITY:   fParity = data;      break;
    case PD_RS232_E_STOP_BITS:  fStopBits = data;    break;
    case PD_E_RX_DATA_RATE:
    case PD_E_RX_DATA_SIZE:
    case PD_E_RX_DATA_INTEGRITY:
    case PD_RS232_E_RX_STOP_BITS:
        break;   /* the receive side follows the transmit side */
    case PD_E_FLOW_CONTROL:     fFlowControl = data; break;
    case PD_RS232_E_XON_BYTE:   fXOn = data;         break;
    case PD_RS232_E_XOFF_BYTE:  fXOff = data;        break;
    case PD_E_DELAY:            fDelay = data;       break;
    case PD_E_DATA_LATENCY:     fDataLatency = data; break;
    case PD_RS232_E_MIN_LATENCY: fMinLatency = data; break;
    case PD_E_SPECIAL_BYTE:
    case PD_E_VALID_DATA_BYTE:
        break;   /* no byte is special on this line */
    case PD_RS232_E_LINE_BREAK:
        setStateGated(data ? PD_RS232_S_BRK : 0, PD_RS232_S_BRK);
        break;
    default:
        return kIOReturnBadArgument;
    }
    return kIOReturnSuccess;
}

IOReturn io_virtio_VirtioConsole::requestEvent(UInt32 event, UInt32 *data, void *)
{
    if (!fAcquired)
        return kIOReturnOffline;
    if (!data)
        return kIOReturnBadArgument;
    return fGate->runAction(requestEventAction, (void *) event, (void *) data);
}

IOReturn io_virtio_VirtioConsole::requestEventAction(OSObject *owner, void *arg0, void *arg1, void *, void *)
{
    return ((io_virtio_VirtioConsole *) owner)->requestEventGated((UInt32) arg0, (UInt32 *) arg1);
}

IOReturn io_virtio_VirtioConsole::requestEventGated(UInt32 event, UInt32 *data)
{
    switch (event) {
    case PD_E_ACTIVE:             *data = (fState & PD_S_ACTIVE) != 0;      break;
    case PD_E_FLOW_CONTROL:       *data = fFlowControl;                     break;
    case PD_E_DELAY:              *data = fDelay;                           break;
    case PD_E_DATA_LATENCY:       *data = fDataLatency;                     break;
    case PD_E_TXQ_SIZE:           *data = fTxQ.size;                        break;
    case PD_E_RXQ_SIZE:           *data = fRxQ.size;                        break;
    case PD_E_TXQ_LOW_WATER:      *data = fTxQ.lowWater;                    break;
    case PD_E_RXQ_LOW_WATER:      *data = fRxQ.lowWater;                    break;
    case PD_E_TXQ_HIGH_WATER:     *data = fTxQ.highWater;                   break;
    case PD_E_RXQ_HIGH_WATER:     *data = fRxQ.highWater;                   break;
    case PD_E_TXQ_AVAILABLE:      *data = fTxQ.size - fTxQ.count;           break;
    case PD_E_RXQ_AVAILABLE:      *data = fRxQ.count;                       break;
    case PD_E_DATA_RATE:          *data = fDataRate;                        break;
    case PD_E_RX_DATA_RATE:       *data = 0;                                break;
    case PD_E_DATA_SIZE:          *data = fDataSize;                        break;
    case PD_E_RX_DATA_SIZE:       *data = 0;                                break;
    case PD_E_DATA_INTEGRITY:     *data = fParity;                          break;
    case PD_E_RX_DATA_INTEGRITY:  *data = 0;                                break;
    case PD_RS232_E_STOP_BITS:    *data = fStopBits;                        break;
    case PD_RS232_E_RX_STOP_BITS: *data = 0;                                break;
    case PD_RS232_E_XON_BYTE:     *data = fXOn;                             break;
    case PD_RS232_E_XOFF_BYTE:    *data = fXOff;                            break;
    case PD_RS232_E_LINE_BREAK:   *data = (fState & PD_RS232_S_BRK) != 0;   break;
    case PD_RS232_E_MIN_LATENCY:  *data = fMinLatency;                      break;
    default:
        return kIOReturnBadArgument;
    }
    return kIOReturnSuccess;
}

/* One byte at a time is data; any other event is carried out at once, since nothing on this line
 * has to wait for the bytes before it. */
IOReturn io_virtio_VirtioConsole::enqueueEvent(UInt32 event, UInt32 data, bool sleep, void *refCon)
{
    if (event == PD_E_VALID_DATA_BYTE) {
        UInt8 byte = (UInt8) data;
        UInt32 count;
        return enqueueData(&byte, 1, &count, sleep, refCon);
    }
    return executeEvent(event, data, refCon);
}

IOReturn io_virtio_VirtioConsole::dequeueEvent(UInt32 *event, UInt32 *data, bool sleep, void *)
{
    if (!event || !data)
        return kIOReturnBadArgument;
    if (!fAcquired)
        return kIOReturnOffline;
    return fGate->runAction(dequeueEventAction, (void *) event, (void *) data, (void *) sleep);
}

IOReturn io_virtio_VirtioConsole::dequeueEventAction(OSObject *owner, void *arg0, void *arg1, void *arg2, void *)
{
    return ((io_virtio_VirtioConsole *) owner)->dequeueEventGated((UInt32 *) arg0, (UInt32 *) arg1, arg2 != 0);
}

IOReturn io_virtio_VirtioConsole::dequeueEventGated(UInt32 *event, UInt32 *data, bool sleep)
{
    if (!(fState & PD_S_ACTIVE))
        return kIOReturnOffline;
    while (fRxQ.count == 0) {
        UInt32 state = 0;
        IOReturn rtn;
        if (!sleep) {
            *event = PD_E_EOQ;
            return kIOReturnSuccess;
        }
        rtn = watchStateGated(&state, PD_S_RXQ_EMPTY);
        if (rtn != kIOReturnSuccess)
            return rtn;
    }
    *event = PD_E_VALID_DATA_BYTE;
    *data = fRxQ.buf[fRxQ.head];
    fRxQ.head = (fRxQ.head + 1) % fRxQ.size;
    fRxQ.count--;
    setStateGated(queueState(), kQueueStateMask);
    return kIOReturnSuccess;
}

IOReturn io_virtio_VirtioConsole::enqueueData(UInt8 *buffer, UInt32 size, UInt32 *count, bool sleep, void *)
{
    if (!buffer || !count)
        return kIOReturnBadArgument;
    *count = 0;
    if (!fAcquired)
        return kIOReturnOffline;
    return fGate->runAction(enqueueDataAction, (void *) buffer, (void *) size, (void *) count, (void *) sleep);
}

IOReturn io_virtio_VirtioConsole::enqueueDataAction(OSObject *owner, void *arg0, void *arg1, void *arg2, void *arg3)
{
    return ((io_virtio_VirtioConsole *) owner)->enqueueDataGated((UInt8 *) arg0, (UInt32) arg1, (UInt32 *) arg2, arg3 != 0);
}

IOReturn io_virtio_VirtioConsole::enqueueDataGated(UInt8 *buffer, UInt32 size, UInt32 *count, bool sleep)
{
    if (!(fState & PD_S_ACTIVE))
        return kIOReturnOffline;
    while (*count < size) {
        UInt32 room = fTxQ.size - fTxQ.count, n, i;
        if (room == 0) {
            UInt32 state = 0;
            IOReturn rtn;
            if (!sleep)
                break;
            rtn = watchStateGated(&state, PD_S_TXQ_FULL);
            if (rtn != kIOReturnSuccess)
                return rtn;
            continue;
        }
        n = size - *count;
        if (n > room)
            n = room;
        for (i = 0; i < n; i++)
            fTxQ.buf[(fTxQ.head + fTxQ.count + i) % fTxQ.size] = buffer[*count + i];
        fTxQ.count += n;
        *count += n;
        pollTx();   /* start sending now rather than at the next tick */
        setStateGated(queueState(), kQueueStateMask);
    }
    return kIOReturnSuccess;
}

IOReturn io_virtio_VirtioConsole::dequeueData(UInt8 *buffer, UInt32 size, UInt32 *count, UInt32 min, void *)
{
    if (!buffer || !count || size < min)
        return kIOReturnBadArgument;
    *count = 0;
    if (!fAcquired)
        return kIOReturnOffline;
    return fGate->runAction(dequeueDataAction, (void *) buffer, (void *) size, (void *) count, (void *) min);
}

IOReturn io_virtio_VirtioConsole::dequeueDataAction(OSObject *owner, void *arg0, void *arg1, void *arg2, void *arg3)
{
    return ((io_virtio_VirtioConsole *) owner)->dequeueDataGated((UInt8 *) arg0, (UInt32) arg1, (UInt32 *) arg2, (UInt32) arg3);
}

/* Copies what there is, up to size, and sleeps only while fewer than min bytes have come. */
IOReturn io_virtio_VirtioConsole::dequeueDataGated(UInt8 *buffer, UInt32 size, UInt32 *count, UInt32 min)
{
    if (!(fState & PD_S_ACTIVE))
        return kIOReturnOffline;
    while (true) {
        UInt32 n = fRxQ.count, i;
        if (n > size - *count)
            n = size - *count;
        for (i = 0; i < n; i++)
            buffer[*count + i] = fRxQ.buf[(fRxQ.head + i) % fRxQ.size];
        fRxQ.head = (fRxQ.head + n) % fRxQ.size;
        fRxQ.count -= n;
        *count += n;
        if (n)
            setStateGated(queueState(), kQueueStateMask);
        if (*count >= min || *count == size)
            break;
        {
            UInt32 state = 0;
            IOReturn rtn = watchStateGated(&state, PD_S_RXQ_EMPTY);
            if (rtn != kIOReturnSuccess)
                return rtn;
        }
    }
    return kIOReturnSuccess;
}
