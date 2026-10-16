/*
 * VirtioBlock: see VirtioBlock.h for what this is and why it is written the way it is.
 *
 * The IOBlockStorageDevice side follows Apple's ATA and SCSI block-storage nubs of the 10.0
 * generation (IOATABlockStorageDevice, IOSCSIBlockCommandsDevice) and IOBlockStorageDriver's own
 * header, which says what it calls and when; the virtio side is written from the virtio 0.9.5
 * (legacy PCI) specification and follows VirtioConsole.cpp and VirtioNet.cpp.
 */

#include "VirtioBlock.h"

#include <IOKit/IOLib.h>
#include <libkern/OSAtomic.h>
#ifdef __LP64__
#include <IOKit/IOKitKeys.h>       /* kIOMaximumByteCount*Key */
#endif

#define super IOBlockStorageDevice
OSDefineMetaClassAndStructors(io_virtio_VirtioBlock, IOBlockStorageDevice)

/* Legacy virtio PCI: the common header in the I/O BAR, then -- with MSI-X off, as it is here --
 * the device's own configuration at 20. IOPCIDevice's ioRead/ioWrite present PCI's little-endian
 * registers in the host's order. */
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

/* The block device's configuration starts with the capacity, in 512-byte sectors. */
enum {
    kConfigCapacity      = kVirtioConfig + 0     /* 64, in the guest's byte order */
};

enum {
    kStatusAcknowledge = 0x01,
    kStatusDriver      = 0x02,
    kStatusDriverOK    = 0x04,
    kStatusFailed      = 0x80
};

enum {
    kISRQueue          = 0x01,
    kISRConfig         = 0x02
};

enum {
    kFeatureReadOnly   = 0x00000020,    /* VIRTIO_BLK_F_RO, bit 5 */
    kFeatureFlush      = 0x00000200     /* VIRTIO_BLK_F_FLUSH, bit 9 */
};

enum {
    kBlkIn             = 0,             /* VIRTIO_BLK_T_IN: read */
    kBlkOut            = 1,             /* VIRTIO_BLK_T_OUT: write */
    kBlkFlush          = 4              /* VIRTIO_BLK_T_FLUSH */
};

enum {
    kBlkStatusOK       = 0,             /* VIRTIO_BLK_S_OK */
    kBlkStatusIOError  = 1,
    kBlkStatusUnsupp   = 2,
    kBlkStatusPending  = 0xFF           /* ours: the device has not written it yet */
};

enum {
    kDescNext          = 1,
    kDescWrite         = 2,
    kAvailNoInterrupt  = 1,
    kUsedNoNotify      = 1
};

enum {
    kPage              = 4096,
    kSectorSize        = 512,
    kSegsMax           = 32,            /* data descriptors in a chain */
    kDescsPerSlot      = kSegsMax + 2,  /* and the header and the status */
    kMaxTransfer       = (kSegsMax - 2) * kPage,   /* 30 pages: at most 31 segments, whatever the start */
    kSlotsMax          = 16,
    kPollMS            = 2,             /* the timer's period when polled, while requests are out */
    kIdleAckMS         = 100,           /* and while none are, to acknowledge a stray interrupt */
    kStrayLogMax       = 5,             /* stray interrupts logged, then counted only */
    kWatchdogMS        = 50,            /* and when interrupts carry the work */
    kQuietTicksMax     = 2,             /* watchdog ticks with unannounced work before a recovery */
    kRecoveriesMax     = 5,             /* recoveries in a row with no interrupt between, before polling */
    kRecoveryLogMax    = 5,             /* recoveries logged, then counted only */
    kStallMS           = 5000           /* requests out this long with no completion are logged */
};

/* PCI configuration space, spelled out because 10.0's headers name only some of it. */
enum {
    kConfigCommand     = 0x04,
    kConfigBAR0        = 0x10,
    kCommandIO         = 0x0001,
    kCommandBusMaster  = 0x0004
};

static inline UInt32 roundPage(UInt32 n) { return (n + kPage - 1) & ~(UInt32)(kPage - 1); }

/* IOStorage::complete takes the completion by value in the 32-bit kernels and by pointer in the
 * 64-bit ones. */
static inline void storageComplete(IOStorageCompletion &c, IOReturn status, UInt64 bytes)
{
#ifdef __LP64__
    IOStorage::complete(&c, status, bytes);
#else
    IOStorage::complete(c, status, bytes);
#endif
}

/* Orders memory the device reads against the register write that tells it to, and the used index
 * against the used element read after it. PowerPC's sync, which GCC 2.95 and 3 both take in this
 * spelling; x86's mfence (every Intel Mac's CPU has SSE2). */
#ifdef __ppc__
static inline void barrier() { __asm__ __volatile__ ("sync" : : : "memory"); }
#else
static inline void barrier() { __asm__ __volatile__ ("mfence" : : : "memory"); }
#endif

/* The physical address and length of the contiguous piece of a buffer at an offset. */
static inline VirtioBlockPhys physicalSegment(IOMemoryDescriptor *md, IOByteCount offset, IOByteCount *len)
{
#if defined(__LP64__)
    return md->getPhysicalSegment(offset, len, kIOMemoryMapperNone);
#elif defined(__i386__)
    return md->getPhysicalSegment64(offset, len);
#else
    return md->getPhysicalSegment(offset, len);
#endif
}

/* IOMallocContiguous wants an IOPhysicalAddress, which is 32 bits on i386 and PowerPC. */
static void *mallocContiguous(vm_size_t size, VirtioBlockPhys *phys)
{
    IOPhysicalAddress p = 0;
    void *mem = IOMallocContiguous(size, kPage, &p);

    *phys = p;
    return mem;
}

/* With the BAR mapped, register offsets are relative to it; without, they are absolute I/O ports,
 * the BAR's own address plus the register. */
UInt8  io_virtio_VirtioBlock::rd8(UInt16 off)            { return fPCI->ioRead8((fIO ? 0 : fIOBase) + off, fIO); }
UInt16 io_virtio_VirtioBlock::rd16(UInt16 off)           { return fPCI->ioRead16((fIO ? 0 : fIOBase) + off, fIO); }
UInt32 io_virtio_VirtioBlock::rd32(UInt16 off)           { return fPCI->ioRead32((fIO ? 0 : fIOBase) + off, fIO); }
void   io_virtio_VirtioBlock::wr8(UInt16 off, UInt8 v)   { fPCI->ioWrite8((fIO ? 0 : fIOBase) + off, v, fIO); }
void   io_virtio_VirtioBlock::wr16(UInt16 off, UInt16 v) { fPCI->ioWrite16((fIO ? 0 : fIOBase) + off, v, fIO); }
void   io_virtio_VirtioBlock::wr32(UInt16 off, UInt32 v) { fPCI->ioWrite32((fIO ? 0 : fIOBase) + off, v, fIO); }

/* ------------------------------------------------------------------------------------------ */
/* the device                                                                                  */
/* ------------------------------------------------------------------------------------------ */

void io_virtio_VirtioBlock::deviceReset()
{
    wr8(kVirtioStatus, 0);
}

/* Sizes the queue by what the device reports and allocates its rings and the slots' header page. */
bool io_virtio_VirtioBlock::queueAlloc()
{
    UInt32 availOff, usedOff;

    wr16(kVirtioQueueSel, 0);
    fNum = rd16(kVirtioQueueNum);
    fSlots = fNum / kDescsPerSlot;
    if (fSlots < 1) {
        IOLog("VirtioBlock: the queue has %d entries, too few for one request\n", fNum);
        return false;
    }
    if (fSlots > kSlotsMax)
        fSlots = kSlotsMax;

    availOff = 16 * fNum;
    usedOff = roundPage(availOff + 6 + 2 * fNum);
    fMemSize = usedOff + roundPage(6 + 8 * fNum);
    fMem = (UInt8 *) mallocContiguous(fMemSize, &fMemPhys);
    if (!fMem)
        return false;
    fDesc       = (volatile VirtioBlockDesc *) fMem;
    fAvailFlags = (volatile UInt16 *) (fMem + availOff);
    fAvailIdx   = fAvailFlags + 1;
    fAvailRing  = fAvailFlags + 2;
    fUsedFlags  = (volatile UInt16 *) (fMem + usedOff);
    fUsedIdx    = fUsedFlags + 1;
    fUsedRing   = (volatile VirtioBlockUsedElem *) (fMem + usedOff + 4);

    fMeta = (VirtioBlockMeta *) mallocContiguous(kPage, &fMetaPhys);
    fInFlight = (VirtioBlockRequest **) IOMalloc(fSlots * sizeof(VirtioBlockRequest *));
    fFree = (UInt16 *) IOMalloc(fSlots * sizeof(UInt16));
    if (!fMeta || !fInFlight || !fFree)
        return false;
    bzero(fInFlight, fSlots * sizeof(VirtioBlockRequest *));
#ifdef VIRTIOBLOCK_DMACOMMAND
    fDMA = (IODMACommand **) IOMalloc(fSlots * sizeof(IODMACommand *));
    if (!fDMA)
        return false;
    bzero(fDMA, fSlots * sizeof(IODMACommand *));
    for (UInt16 i = 0; i < fSlots; i++) {
        fDMA[i] = IODMACommand::withSpecification(kIODMACommandOutputHost64, 64, 0,
                                                  IODMACommand::kMapped, kMaxTransfer, 1);
        if (!fDMA[i])
            return false;
    }
#endif
    return true;
}

void io_virtio_VirtioBlock::queueFree()
{
#ifdef VIRTIOBLOCK_DMACOMMAND
    if (fDMA) {
        for (UInt16 i = 0; i < fSlots; i++)
            if (fDMA[i])
                fDMA[i]->release();
        IOFree(fDMA, fSlots * sizeof(IODMACommand *));
        fDMA = 0;
    }
#endif
    if (fInFlight)
        IOFree(fInFlight, fSlots * sizeof(VirtioBlockRequest *));
    if (fFree)
        IOFree(fFree, fSlots * sizeof(UInt16));
    if (fMeta)
        IOFreeContiguous(fMeta, kPage);
    if (fMem)
        IOFreeContiguous(fMem, fMemSize);
    fInFlight = 0;
    fFree = 0;
    fMeta = 0;
    fMem = 0;
}

/* Once, in start: maps the device, negotiates, reads the capacity and gives the device its queue.
 * The device is live when this returns true. */
bool io_virtio_VirtioBlock::deviceStart()
{
    UInt16 cmd, i;
    UInt32 features;
    UInt8 capacity[8];

    /* The I/O BAR, mapped; failing that, its port number, for IOPCIDevice's own I/O mapping. */
    fIO = fPCI->mapDeviceMemoryWithRegister(kConfigBAR0);
    fIOBase = (UInt16) (fPCI->configRead32(kConfigBAR0) & ~(UInt32) 3);
    cmd = fPCI->configRead16(kConfigCommand);
    fPCI->configWrite16(kConfigCommand, cmd | kCommandIO | kCommandBusMaster);

    deviceReset();
    wr8(kVirtioStatus, kStatusAcknowledge);
    wr8(kVirtioStatus, kStatusAcknowledge | kStatusDriver);
    features = rd32(kVirtioHostFeatures);
    fFeatures = features & (kFeatureReadOnly | kFeatureFlush);
    wr32(kVirtioGuestFeatures, fFeatures);

    /* The capacity is in the guest's byte order, so its bytes, read one at a time as VirtioNet reads
     * its MAC address, are a UInt64 in memory as they are: most significant first on PowerPC, least
     * on Intel. */
    for (i = 0; i < 8; i++)
        capacity[i] = rd8(kConfigCapacity + i);
    bcopy(capacity, &fCapacity, sizeof fCapacity);
    if (fCapacity == 0) {
        IOLog("VirtioBlock: the device reports no capacity\n");
        wr8(kVirtioStatus, kStatusFailed);
        return false;
    }

    if (!queueAlloc()) {
        wr8(kVirtioStatus, kStatusFailed);
        return false;
    }
    bzero(fMem, fMemSize);
    bzero(fMeta, kPage);
    fLastUsed = 0;
    fNextAvail = 0;
    *fAvailFlags = fPolled ? kAvailNoInterrupt : 0;
    for (i = 0; i < fSlots; i++)
        fFree[i] = fSlots - 1 - i;
    fFreeCount = fSlots;
    barrier();
    wr16(kVirtioQueueSel, 0);
    wr32(kVirtioQueuePFN, fMemPhys / kPage);
    wr8(kVirtioStatus, kStatusAcknowledge | kStatusDriver | kStatusDriverOK);
    return true;
}

/* ------------------------------------------------------------------------------------------ */
/* the work                                                                                    */
/* ------------------------------------------------------------------------------------------ */

/* Builds the slot's chain for the request's next chunk -- at most kMaxTransfer from where the device
 * left off -- and gives it to the device. A request larger than that goes out in chunks, one after
 * the other on its slot (service): 10.0's IOBlockStorageDriver does not split requests at
 * reportMaxReadTransfer, and refusing them failed every read of a large file, which then read as
 * zeros (live, 10.0.4 booted from virtio-blk, 2026-10-03: "executeRequest: request failed to start!",
 * apps shown as folders). False if the chunk takes more segments than a chain has, which its size
 * prevents; nothing is changed then. */
bool io_virtio_VirtioBlock::issue(VirtioBlockRequest *req, UInt16 slot)
{
    volatile VirtioBlockDesc *d = fDesc + slot * kDescsPerSlot;
    VirtioBlockMeta *meta = fMeta + slot;
    VirtioBlockPhys metaPhys = fMetaPhys + slot * sizeof(VirtioBlockMeta);
    UInt16 flags = (req->type == kBlkIn) ? kDescWrite : 0;
    UInt32 chunk = req->blocks - req->done;
    UInt32 offset, length, n = 0;
    UInt16 base = slot * kDescsPerSlot;

    if (chunk > kMaxTransfer / kSectorSize)
        chunk = kMaxTransfer / kSectorSize;
    offset = req->done * kSectorSize;
    length = offset + chunk * kSectorSize;

#ifdef VIRTIOBLOCK_DMACOMMAND
    /* The chunk's segments from the slot's IODMACommand, prepared for the chunk alone and completed
     * when the device has finished it (chunkDone). Up to 0.2.6 they came from the buffer's own
     * getPhysicalSegment64, and with two CPUs Tiger's memory was corrupted by this driver's I/O:
     * 64 MiB written to a virtio-blk disk read back wrong at every block size -- whole pages that
     * held another process's exec arguments ("PATH=...", "_=/usr/bin/head") where the disk's data
     * belonged, the disk itself holding the right data -- a page of a file on the AHCI system disk
     * overwritten the same way, and pmap_remove panics ("mapping not in pv_list", "null pv_list")
     * within minutes, polled or interrupt-driven. The same test was clean with the disk on AHCI and
     * with one CPU (tiger-guest, 10.4.11, 2026-10-16); Apple's drivers on these kernels take their
     * buffers through an IODMACommand. Leopard's installed systems had the same signature: a kext
     * holding strings from the account's new home directory, an HFS catalog node holding a Mach-O
     * header. */
    if (req->buffer) {
        IODMACommand *dma = fDMA[slot];
        IODMACommand::Segment64 segs[kSegsMax];
        UInt32 num = kSegsMax, i;
        UInt64 off = offset, total = 0;

        if (req->dma != dma) {
            if (dma->setMemoryDescriptor(req->buffer, false) != kIOReturnSuccess)
                return false;
            req->dma = dma;
        }
        if (dma->prepare(offset, length - offset, false, true) != kIOReturnSuccess)
            return false;
        if (dma->gen64IOVMSegments(&off, segs, &num) != kIOReturnSuccess) {
            dma->complete(false, false);
            return false;
        }
        for (i = 0; i < num; i++) {
            d[1 + n].addr     = segs[i].fIOVMAddr;
            d[1 + n].len      = (UInt32) segs[i].fLength;
            d[1 + n].flags    = flags | kDescNext;
            d[1 + n].next     = base + 2 + n;
            total += segs[i].fLength;
            n++;
        }
        if (!n || total != length - offset) {
            dma->complete(false, false);
            return false;
        }
    }
#else
    if (req->buffer) {
        while (offset < length) {
            IOByteCount segLen;
            VirtioBlockPhys phys = physicalSegment(req->buffer, offset, &segLen);

            if (!phys || !segLen || n == kSegsMax)
                return false;
            if (segLen > length - offset)
                segLen = length - offset;
            d[1 + n].addr     = phys;
            d[1 + n].len      = segLen;
            d[1 + n].flags    = flags | kDescNext;
            d[1 + n].next     = base + 2 + n;
            offset += segLen;
            n++;
        }
    }
#endif

    meta->type       = req->type;
    meta->ioprio     = 0;
    meta->sector     = req->block + req->done;
    req->chunk       = chunk;
    meta->status     = kBlkStatusPending;

    d[0].addr     = metaPhys;
    d[0].len      = 16;
    d[0].flags    = kDescNext;
    d[0].next     = base + 1;

    d[1 + n].addr     = metaPhys + 16;
    d[1 + n].len      = 1;
    d[1 + n].flags    = kDescWrite;
    d[1 + n].next     = 0;

    fInFlight[slot] = req;
    fAvailRing[fNextAvail % fNum] = base;
    barrier();
    fNextAvail++;
    *fAvailIdx = fNextAvail;
    barrier();
    if (!(*fUsedFlags & kUsedNoNotify))
        wr16(kVirtioQueueNotify, 0);
    return true;
}

/* The device has finished the request's chunk: its slot's DMA command lets go of it, synchronized. */
void io_virtio_VirtioBlock::chunkDone(VirtioBlockRequest *req)
{
#ifdef VIRTIOBLOCK_DMACOMMAND
    if (req->dma)
        req->dma->complete(false, true);
#endif
}

/* Calls the request's completion and lets go of its buffer -- the slot's DMA command first, so
 * that a request the completion sends at once can have the slot. */
void io_virtio_VirtioBlock::finish(VirtioBlockRequest *req, IOReturn status)
{
    UInt64 bytes = (status == kIOReturnSuccess) ? (UInt64) req->blocks * kSectorSize : 0;

#ifdef VIRTIOBLOCK_DMACOMMAND
    if (req->dma) {
        req->dma->clearMemoryDescriptor(false);
        req->dma = 0;
    }
#endif

    if (req->buffer) {
        req->buffer->complete();
        req->buffer->release();
    }
    storageComplete(req->completion, status, bytes);
    IOFree(req, sizeof(VirtioBlockRequest));
}

/* On the work loop: a request goes out at once if a slot is free, else it waits. */
void io_virtio_VirtioBlock::submit(VirtioBlockRequest *req)
{
    UInt16 slot;

    if (!fRunning) {
        finish(req, kIOReturnNotAttached);
        return;
    }
    if (fWaitHead || !fFreeCount) {
        req->next = 0;
        if (fWaitTail)
            fWaitTail->next = req;
        else
            fWaitHead = req;
        fWaitTail = req;
        armTimer();
        return;
    }
    slot = fFree[--fFreeCount];
    if (!issue(req, slot)) {
        fFree[fFreeCount++] = slot;
        finish(req, kIOReturnNoResources);
        return;
    }
    armTimer();
}

IOReturn io_virtio_VirtioBlock::armAction(OSObject *owner, void *, void *, void *, void *)
{
    ((io_virtio_VirtioBlock *) owner)->armTimer();
    return kIOReturnSuccess;
}

IOReturn io_virtio_VirtioBlock::submitAction(OSObject *owner, void *req, void *, void *, void *)
{
    ((io_virtio_VirtioBlock *) owner)->submit((VirtioBlockRequest *) req);
    return kIOReturnSuccess;
}

/* Takes back what the device has finished, then sends what waited for a slot. */
void io_virtio_VirtioBlock::service()
{
    while (fLastUsed != *fUsedIdx) {
        VirtioBlockRequest *req;
        UInt32 id;
        UInt16 slot;
        UInt8 status;

        barrier();
        id = fUsedRing[fLastUsed % fNum].id;
        fLastUsed++;
        slot = (UInt16) (id / kDescsPerSlot);
        if (id % kDescsPerSlot || slot >= fSlots || !fInFlight[slot]) {
            IOLog("VirtioBlock: the device returned descriptor %d, which heads no request in flight\n", (int) id);
            continue;
        }
        req = fInFlight[slot];
        fInFlight[slot] = 0;
        status = fMeta[slot].status;
        chunkDone(req);
        if (status == kBlkStatusOK && req->done + req->chunk < req->blocks) {
            req->done += req->chunk;
            if (issue(req, slot))
                continue;           /* the next chunk, on the same slot */
            fFree[fFreeCount++] = slot;
            finish(req, kIOReturnNoResources);
            continue;
        }
        fFree[fFreeCount++] = slot;
        finish(req, status == kBlkStatusOK ? kIOReturnSuccess :
                    status == kBlkStatusUnsupp ? kIOReturnUnsupported : kIOReturnIOError);
    }

    while (fWaitHead && fFreeCount) {
        VirtioBlockRequest *req = fWaitHead;
        UInt16 slot = fFree[fFreeCount - 1];

        fWaitHead = req->next;
        if (!fWaitHead)
            fWaitTail = 0;
        if (issue(req, slot))
            fFreeCount--;
        else
            finish(req, kIOReturnNoResources);
    }
}

/* Whether anything is out with the device or waiting to be. */
#define BUSY()  (fFreeCount < fSlots || fWaitHead)

/* While there is work, the timer polls (when no interrupt announces completions) or checks (when
 * one does) that the interrupt has not been missed. */
void io_virtio_VirtioBlock::armTimer()
{
    UInt32 ms = !fPolled ? kWatchdogMS : (BUSY() ? kPollMS : kIdleAckMS);

    if (fTimerArmed) {
        if (ms >= fTimerMS)
            return;             /* due sooner already */
        fTimer->cancelTimeout();
    }
    fTimerArmed = true;
    fTimerMS = ms;
    fTimer->setTimeoutMS(ms);
}

/* Polled, the ISR is read on every tick all the same, idle or busy. A legacy virtio device lowers its
 * INTx only when the ISR is read, and the avail ring's NO_INTERRUPT flag is a hint: whatever set the
 * ISR would leave a level-triggered line asserted that no handler of this driver's clears, and on a
 * line someone else has unmasked that is a storm. Read here, the line drops within one tick; the
 * first few are logged. (Written when a polled VirtioBlock was suspected of a storm on 10.6.8 that
 * turned out to be VirtioNet's -- 0.2.1 logged no interrupt in that run -- and kept: the read is
 * cheap and the reasoning holds.) */
void io_virtio_VirtioBlock::ackInterrupt()
{
    UInt8 isr = rd8(kVirtioISR);

    if (isr && fStrays++ < kStrayLogMax)
        IOLog("VirtioBlock: polled, and the device raised its interrupt (ISR 0x%x)\n", isr);
}

/* At primary interrupt time: reading the ISR lowers the device's interrupt line. The bits are kept
 * for the action, which the work loop runs. */
bool io_virtio_VirtioBlock::interruptFilter(OSObject *owner, IOFilterInterruptEventSource *)
{
    io_virtio_VirtioBlock *me = (io_virtio_VirtioBlock *) owner;
    UInt8 isr = me->rd8(kVirtioISR);

    if (!isr)
        return false;   /* someone else's, on a shared line */
    OSBitOrAtomic(isr, (UInt32 *) &me->fISR);
    OSIncrementAtomic((SInt32 *) &me->fInterrupts);
    return true;
}

void io_virtio_VirtioBlock::interruptAction(OSObject *owner, IOInterruptEventSource *, int count)
{
    ((io_virtio_VirtioBlock *) owner)->interruptOccurred(count);
}

void io_virtio_VirtioBlock::interruptOccurred(int)
{
    UInt32 isr;

    do {
        isr = fISR;
    } while (!OSCompareAndSwap(isr, 0, (UInt32 *) &fISR));
    if (fRunning)
        service();
}

void io_virtio_VirtioBlock::timeoutAction(OSObject *owner, IOTimerEventSource *)
{
    ((io_virtio_VirtioBlock *) owner)->timeoutOccurred();
}

/* Registers the device's interrupt and enables it at once: a filter source registered and left
 * disabled is what a shared INTx cannot afford (see interruptDetach). */
bool io_virtio_VirtioBlock::interruptAttach()
{
    if (fInterrupt)
        return true;
    fInterrupt = IOFilterInterruptEventSource::filterInterruptEventSource(this, interruptAction,
                                                                          interruptFilter, fPCI, 0);
    if (!fInterrupt)
        return false;
    if (fWorkLoop->addEventSource(fInterrupt) != kIOReturnSuccess) {
        fInterrupt->release();
        fInterrupt = 0;
        return false;
    }
    fInterrupt->enable();
    return true;
}

/* Gives the interrupt back. Disabled and left registered, as 0.2.3 left it on falling back to
 * polling, the source would get the vector masked by the next interrupt on it -- and with it every
 * other device on a shared line: on q35 the system disk shares its line with the 9p share and the
 * gauntlet's second disk. */
void io_virtio_VirtioBlock::interruptDetach()
{
    if (!fInterrupt)
        return;
    fInterrupt->disable();
    if (fWorkLoop)
        fWorkLoop->removeEventSource(fInterrupt);
    fInterrupt->release();
    fInterrupt = 0;
}

/* Polled, the timer does all the work. Otherwise it is a watchdog: completions that no interrupt
 * announced are a lost interrupt, and are recovered from -- the ISR read, which lowers the device's
 * line so that its next completion raises it afresh, and the completions served. Only when
 * recoveries keep coming with no interrupt between them -- the platform does not route the line
 * at all -- does the driver poll from then on.
 *
 * Up to 0.2.4 the first lost interrupt turned polling on for good. On the whole gauntlet of
 * 2026-10-14 and again with 0.2.4 the next day, Leopard's and Mountain Lion's to High Sierra's
 * system disks took thousands of interrupts (4126, 8660, 11543) and then one did not come, the
 * device's ISR still 0x1: a lost one, on a line q35 shares among the system disk, Virtio9P and the
 * gauntlet's second disk, not a line that was never routed. */
void io_virtio_VirtioBlock::timeoutOccurred()
{
    fTimerArmed = false;
    if (!fRunning)
        return;
    if (fPolled)
        ackInterrupt();
    if (!fPolled) {
        if (fInterrupts != fInterruptsSeen)
            fRecoveries = 0;    /* interrupts came since the last recovery */
        if (fLastUsed != *fUsedIdx && fInterrupts == fInterruptsSeen) {
            if (++fQuietTicks >= kQuietTicksMax) {
                UInt8 isr = rd8(kVirtioISR);    /* lowers the line */

                fQuietTicks = 0;
                fRecovered++;
                if (++fRecoveries >= kRecoveriesMax) {
                    IOLog("VirtioBlock: completions raise no interrupt (%u taken, %u recovered from); polling every %d ms instead\n",
                          (unsigned) fInterrupts, (unsigned) fRecovered, kPollMS);
                    interruptDetach();
                    fPolled = true;
                    *fAvailFlags = kAvailNoInterrupt;
                } else if (fRecovered <= kRecoveryLogMax)
                    IOLog("VirtioBlock: an interrupt was lost (ISR 0x%x, %u taken); recovered, still interrupt-driven\n",
                          isr, (unsigned) fInterrupts);
            }
        } else
            fQuietTicks = 0;
        fInterruptsSeen = fInterrupts;
    }
    service();
    noteStall();
    if (BUSY() || fPolled)
        armTimer();
}

/* Requests out with no completion for five seconds are logged once, with what the rings and the
 * ISR say: lion-guest on 0976e8e froze with every disk request stalled and nothing in its log to say
 * where (2026-10-14). */
void io_virtio_VirtioBlock::noteStall()
{
    if (!BUSY() || fLastUsed != fStallUsed) {
        fStallUsed = fLastUsed;
        fStallMS = 0;
        fStallSaid = false;
        return;
    }
    fStallMS += fTimerMS;
    if (fStallMS >= kStallMS && !fStallSaid) {
        fStallSaid = true;
        IOLog("VirtioBlock: %d requests out, %d waiting, no completion for %d ms (avail %d, used %d, device used %d, ISR 0x%x, %s)\n",
              fSlots - fFreeCount, fWaitHead ? 1 : 0, (int) fStallMS, fNextAvail, fLastUsed, *fUsedIdx,
              rd8(kVirtioISR), fPolled ? "polled" : "interrupt-driven");
    }
}

/* ------------------------------------------------------------------------------------------ */
/* the device nub                                                                              */
/* ------------------------------------------------------------------------------------------ */

bool io_virtio_VirtioBlock::start(IOService *provider)
{
    OSBoolean *polled;

    if (!super::start(provider))
        return false;

    fPCI = OSDynamicCast(IOPCIDevice, provider);
    if (!fPCI)
        return false;
    fPCI->retain();
    if (!fPCI->open(this))
        goto fail;

    polled = OSDynamicCast(OSBoolean, getProperty("Polled"));
    fPolled = polled && polled->isTrue();

    fWorkLoop = IOWorkLoop::workLoop();
    if (!fWorkLoop)
        goto fail;
    fGate = IOCommandGate::commandGate(this);
    if (!fGate || fWorkLoop->addEventSource(fGate) != kIOReturnSuccess)
        goto fail;
    fTimer = IOTimerEventSource::timerEventSource(this, timeoutAction);
    if (!fTimer || fWorkLoop->addEventSource(fTimer) != kIOReturnSuccess)
        goto fail;
    if (!deviceStart())
        goto fail;
    /* The interrupt only now: the device is reset and its ISR clear, so the line is low. Up to
     * 0.2.3 it was registered before the reset and enabled after it, and the source sat registered
     * and disabled while whatever the firmware left -- OVMF and OpenCore read the disk through the
     * device too -- could hold the line up; an interrupt on a vector whose source is disabled gets
     * the vector masked. On Leopard and Mountain Lion to High Sierra every VirtioBlock fell back to
     * polling (the whole gauntlet of 2026-10-14, the system disk alone included, on a line shared
     * with Virtio9P), which is what a masked vector looks like; VirtioNet 0.2.2 learned the same of
     * a shared line. Suspected, not shown: the fallback's line now says what was seen. */
    if (!fPolled && !interruptAttach()) {
        IOLog("VirtioBlock: the device's interrupt cannot be had; polling every %d ms instead\n", kPollMS);
        fPolled = true;
        *fAvailFlags = kAvailNoInterrupt;
    }
    fRunning = true;
    if (fPolled) {
        ackInterrupt();         /* whatever the firmware or a reset left */
        fGate->runAction(armAction);
    }

    IOLog("VirtioBlock: %lu sectors (%lu MB)%s, I/O at 0x%x%s, %d request slots, %s%s\n",
          (unsigned long) fCapacity, (unsigned long) (fCapacity / 2048),
          (fFeatures & kFeatureReadOnly) ? ", read-only" : "",
          fIOBase, fIO ? "" : " (through the bridge)", fSlots,
          fPolled ? "polled" : "interrupt-driven",
          (fFeatures & kFeatureFlush) ? ", flush" : "");

    /* What IOBlockStorageDriver::probe compares the provider's against. */
    setProperty(kIOBlockStorageDeviceTypeKey, kIOBlockStorageDeviceTypeGeneric);
#ifdef __LP64__
    /* The 64-bit kernels read the largest transfer from properties, not reportMaxRead/WriteTransfer.
     * Larger requests are still taken (issue sends them in chunks). */
    setProperty(kIOMaximumByteCountReadKey, kMaxTransfer, 64);
    setProperty(kIOMaximumByteCountWriteKey, kMaxTransfer, 64);
#endif
    registerService();
    return true;

fail:
    IOLog("VirtioBlock: could not start\n");
    stop(provider);
    return false;
}

/* Anything still with the device or waiting is failed: its client has gone, or is going. */
void io_virtio_VirtioBlock::stop(IOService *provider)
{
    VirtioBlockRequest *req;
    UInt16 slot;

    fRunning = false;
    if (fTimer)
        fTimer->cancelTimeout();
    interruptDetach();
    if (fPCI && (fIO || fIOBase))
        deviceReset();
    for (slot = 0; fInFlight && slot < fSlots; slot++) {
        req = fInFlight[slot];
        fInFlight[slot] = 0;
        if (req)
            finish(req, kIOReturnNotAttached);
    }
    while ((req = fWaitHead) != 0) {
        fWaitHead = req->next;
        finish(req, kIOReturnNotAttached);
    }
    fWaitTail = 0;
    if (fPCI && fPCI->isOpen(this))
        fPCI->close(this);
    super::stop(provider);
}

void io_virtio_VirtioBlock::free()
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
    queueFree();
    if (fIO)
        fIO->release();
    if (fPCI)
        fPCI->release();
    if (fWorkLoop)
        fWorkLoop->release();
    super::free();
}

/* ------------------------------------------------------------------------------------------ */
/* IOBlockStorageDevice                                                                        */
/* ------------------------------------------------------------------------------------------ */

/* On the caller's thread: checks the request, wires the buffer, and enters the work loop with it.
 * The request has the buffer retained and prepared, and is freed by finish. */
IOReturn io_virtio_VirtioBlock::queueRequest(IOMemoryDescriptor *buffer, UInt32 type, UInt64 block,
                                      UInt64 nblks, IOStorageCompletion completion)
{
    VirtioBlockRequest *req;
    IOReturn r;

    if (!fRunning)
        return kIOReturnNotAttached;
    if (type != kBlkFlush) {
        if (!buffer || block + nblks > fCapacity || nblks > 0xFFFFFFFFULL ||
            (UInt64) nblks * kSectorSize > buffer->getLength())
            return kIOReturnBadArgument;
        if (type == kBlkOut && (fFeatures & kFeatureReadOnly))
            return kIOReturnNotWritable;
        if (nblks == 0) {
            storageComplete(completion, kIOReturnSuccess, 0);
            return kIOReturnSuccess;
        }
    }
    req = (VirtioBlockRequest *) IOMalloc(sizeof(VirtioBlockRequest));
    if (!req)
        return kIOReturnNoMemory;
    req->next = 0;
    req->buffer = (type == kBlkFlush) ? 0 : buffer;
    req->completion = completion;
    req->type = type;
    req->block = block;
    req->blocks = (type == kBlkFlush) ? 0 : (UInt32) nblks;
    req->done = 0;
    req->chunk = 0;
#ifdef VIRTIOBLOCK_DMACOMMAND
    req->dma = 0;
#endif
    if (req->buffer) {
        r = req->buffer->prepare();
        if (r != kIOReturnSuccess) {
            IOFree(req, sizeof(VirtioBlockRequest));
            return r;
        }
        req->buffer->retain();
    }
    r = fGate->runAction(submitAction, req);
    if (r != kIOReturnSuccess)
        finish(req, r);     /* the completion is called with the failure, the caller sees success */
    return kIOReturnSuccess;
}

/* The direction is the buffer's: kIODirectionIn is data into memory, a read. */
static inline UInt32 direction(IOMemoryDescriptor *buffer)
{
    return (buffer && buffer->getDirection() == kIODirectionIn) ? kBlkIn : kBlkOut;
}

struct VirtioBlockSyncWait {
    volatile bool     done;
    volatile IOReturn status;
};

static void syncDone(void *, void *parameter, IOReturn status, UInt64)
{
    VirtioBlockSyncWait *w = (VirtioBlockSyncWait *) parameter;

    w->status = status;
    barrier();
    w->done = true;
}

/* Through the asynchronous path, waiting a millisecond at a time for its completion -- which only
 * the work loop delivers, from its interrupt action or its timer. A caller that is the work loop
 * itself (a completion this driver called that asked, in turn, for a flush or a synchronous read)
 * would wait for ever for itself, so there the device is driven from here. The suspected shape of
 * lion-guest's freeze on 0976e8e (2026-10-14): iMac19,1's ACPI could not enable the PCI links
 * ("LINK: [GSIB] no interrupt / enable failed"), VirtioBlock polled, and after Security Update
 * 2014-004 every disk request stalled with no panic. Not shown: there is no log of where it
 * stopped. */
static IOReturn syncWait(io_virtio_VirtioBlock *me, VirtioBlockSyncWait *w, IOReturn r)
{
    if (r != kIOReturnSuccess)
        return r;
    while (!w->done)
        if (!me->driveOnWorkLoop())
            IOSleep(1);
    return w->status;
}

bool io_virtio_VirtioBlock::driveOnWorkLoop()
{
    if (!fWorkLoop || !fWorkLoop->onThread())
        return false;
    if (fPolled)
        ackInterrupt();
    service();
    IODelay(50);
    return true;
}

#ifdef __LP64__

/* The attributes (FUA, priority) are not acted on: without the FLUSH feature there is no cache, and
 * with it doSynchronizeCache flushes it. */
IOReturn io_virtio_VirtioBlock::doAsyncReadWrite(IOMemoryDescriptor *buffer, UInt64 block, UInt64 nblks,
                                               IOStorageAttributes *, IOStorageCompletion *completion)
{
    return queueRequest(buffer, direction(buffer), block, nblks, *completion);
}

IOReturn io_virtio_VirtioBlock::getWriteCacheState(bool *enabled)
{
    *enabled = (fFeatures & kFeatureFlush) != 0;
    return kIOReturnSuccess;
}

IOReturn io_virtio_VirtioBlock::setWriteCacheState(bool)
{
    return kIOReturnUnsupported;
}

#else /* !__LP64__ */

IOReturn io_virtio_VirtioBlock::doAsyncReadWrite(IOMemoryDescriptor *buffer, UInt32 block, UInt32 nblks,
                                               IOStorageCompletion completion)
{
    return queueRequest(buffer, direction(buffer), block, nblks, completion);
}

IOReturn io_virtio_VirtioBlock::doSyncReadWrite(IOMemoryDescriptor *buffer, UInt32 block, UInt32 nblks)
{
    VirtioBlockSyncWait w;
    IOStorageCompletion c;

    w.done = false;
    w.status = kIOReturnSuccess;
    c.target = 0;
    c.action = syncDone;
    c.parameter = &w;
    return syncWait(this, &w, doAsyncReadWrite(buffer, block, nblks, c));
}

IOReturn io_virtio_VirtioBlock::reportMaxReadTransfer(UInt64, UInt64 *max)
{
    *max = kMaxTransfer;
    return kIOReturnSuccess;
}

IOReturn io_virtio_VirtioBlock::reportMaxWriteTransfer(UInt64, UInt64 *max)
{
    *max = kMaxTransfer;
    return kIOReturnSuccess;
}

#endif /* !__LP64__ */

/* Without the FLUSH feature the device has no write cache to flush. */
IOReturn io_virtio_VirtioBlock::doSynchronizeCache(void)
{
    VirtioBlockSyncWait w;
    IOStorageCompletion c;

    if (!(fFeatures & kFeatureFlush))
        return kIOReturnSuccess;
    w.done = false;
    w.status = kIOReturnSuccess;
    c.target = 0;
    c.action = syncDone;
    c.parameter = &w;
    return syncWait(this, &w, queueRequest(0, kBlkFlush, 0, 0, c));
}

IOReturn io_virtio_VirtioBlock::doEjectMedia(void)
{
    return kIOReturnUnsupported;
}

IOReturn io_virtio_VirtioBlock::doFormatMedia(UInt64)
{
    return kIOReturnUnsupported;
}

UInt32 io_virtio_VirtioBlock::doGetFormatCapacities(UInt64 *, UInt32) const
{
    return 0;
}

IOReturn io_virtio_VirtioBlock::doLockUnlockMedia(bool)
{
    return kIOReturnUnsupported;
}

char *io_virtio_VirtioBlock::getVendorString(void)
{
    static char s[] = "Virtio";
    return s;
}

char *io_virtio_VirtioBlock::getProductString(void)
{
    static char s[] = "virtio-blk disk";
    return s;
}

char *io_virtio_VirtioBlock::getRevisionString(void)
{
    static char s[] = VIRTIOBLOCK_VERSION;
    return s;
}

char *io_virtio_VirtioBlock::getAdditionalDeviceInfoString(void)
{
    static char s[] = "legacy PCI";
    return s;
}

IOReturn io_virtio_VirtioBlock::reportBlockSize(UInt64 *blockSize)
{
    *blockSize = kSectorSize;
    return kIOReturnSuccess;
}

IOReturn io_virtio_VirtioBlock::reportEjectability(bool *isEjectable)
{
    *isEjectable = false;
    return kIOReturnSuccess;
}

IOReturn io_virtio_VirtioBlock::reportLockability(bool *isLockable)
{
    *isLockable = false;
    return kIOReturnSuccess;
}

/* Block numbers are 32 bits in the 32-bit kernels' doAsyncReadWrite (2 TB); 64 in the 64-bit ones'. */
IOReturn io_virtio_VirtioBlock::reportMaxValidBlock(UInt64 *maxBlock)
{
    UInt64 last = fCapacity - 1;

#ifndef __LP64__
    if (last > 0xFFFFFFFFULL)
        last = 0xFFFFFFFFULL;
#endif
    *maxBlock = last;
    return kIOReturnSuccess;
}

/* IOBlockStorageDriver starts with no media and acts only on a reported change: the first report
 * is the disk arriving (live, 10.2.8, 2026-10-02: reporting no change, it attached and made no IOMedia). */
IOReturn io_virtio_VirtioBlock::reportMediaState(bool *mediaPresent, bool *changedState)
{
    *mediaPresent = true;
    *changedState = !fMediaReported;
    fMediaReported = true;
    return kIOReturnSuccess;
}

IOReturn io_virtio_VirtioBlock::reportPollRequirements(bool *pollRequired, bool *pollIsExpensive)
{
    *pollRequired = false;
    *pollIsExpensive = false;
    return kIOReturnSuccess;
}

IOReturn io_virtio_VirtioBlock::reportRemovability(bool *isRemovable)
{
    *isRemovable = false;
    return kIOReturnSuccess;
}

IOReturn io_virtio_VirtioBlock::reportWriteProtection(bool *isWriteProtected)
{
    *isWriteProtected = (fFeatures & kFeatureReadOnly) != 0;
    return kIOReturnSuccess;
}
