/*
 * VirtioNet: see VirtioNet.h for what this is and why it is written the way it is.
 *
 * The IONetworkController side -- its own work loop, an IOGatedOutputQueue, the interface attached
 * in start and the device brought up in enable -- follows Apple's Intel82557 driver of xnu-123
 * (iokit/Drivers/network/drvIntel82557), the 10.0 generation's PCI Ethernet driver; the virtio side
 * is written from the virtio 0.9.5 (legacy PCI) specification and follows VirtioConsole.cpp.
 */

#include "VirtioNet.h"

#include <IOKit/IOLib.h>
#include <libkern/OSAtomic.h>

#ifndef VIRTIONET_KPI
extern "C" {
#include <sys/param.h>
#include <sys/mbuf.h>
}
#endif

#define super IOEthernetController
OSDefineMetaClassAndStructors(io_virtio_VirtioNet, IOEthernetController)

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

/* The network device's configuration: the MAC address, then a 16-bit status. */
enum {
    kConfigMAC           = kVirtioConfig + 0,
    kConfigStatus        = kVirtioConfig + 6
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
    kFeatureMAC        = 0x00000020,    /* VIRTIO_NET_F_MAC, bit 5 */
    kFeatureStatus     = 0x00010000     /* VIRTIO_NET_F_STATUS, bit 16 */
};

enum {
    kNetLinkUp         = 0x0001         /* VIRTIO_NET_S_LINK_UP */
};

enum {
    kDescNext          = 1,
    kDescWrite         = 2,
    kAvailNoInterrupt  = 1,
    kUsedNoNotify      = 1
};

enum {
    kPage              = 4096,
    kSlotSize          = 2048,      /* two slots to a page */
    kHeaderLen         = 10,        /* struct virtio_net_hdr, without MRG_RXBUF's num_buffers */
    kFrameOffset       = 16,        /* the frame's place in its slot */
    kFrameMax          = 1518,      /* 1514 and an 802.1Q tag; no GSO is negotiated */
    kFrameMin          = 14,        /* an Ethernet header */
    kRxSlotsMax        = 128,
    kTxSlotsMax        = 64,
    kOutputQueueSize   = 256,       /* packets IONetworkingFamily holds while the ring is full */
    kPollMS            = 10,        /* the timer's period when polled */
    kWatchdogMS        = 1000,      /* and when interrupts carry the work */
    kLinkPollTicks     = 100,       /* polled, the link status is read every second */
    kQuietTicksMax     = 2          /* watchdog ticks with unannounced work before polling */
};

/* What the interface is told it runs at. The device has no speed; this is what 10.0's and 10.1's
 * Network preferences and ifconfig can name. */
#define kLinkSpeed  ((UInt64) 1000 * 1000 * 1000)

/* PCI configuration space, spelled out because 10.0's headers name only some of it. */
enum {
    kConfigCommand     = 0x04,
    kConfigBAR0        = 0x10,
    kCommandIO         = 0x0001,
    kCommandBusMaster  = 0x0004
};

static inline UInt32 roundPage(UInt32 n) { return (n + kPage - 1) & ~(UInt32)(kPage - 1); }

/* Orders memory the device reads against the register write that tells it to, and the used index
 * against the used element read after it. PowerPC's sync, which GCC 2.95 and 3 both take in this
 * spelling; on Intel mfence, which every Intel Mac's processor has. */
#if defined(__ppc__)
static inline void barrier() { __asm__ __volatile__ ("sync" : : : "memory"); }
#elif defined(__i386__) || defined(__x86_64__)
static inline void barrier() { __asm__ __volatile__ ("mfence" : : : "memory"); }
#else
#error "VirtioNet knows PowerPC and Intel"
#endif

/* A packet's data, length and successor, and a received one's frame placed in it: by the fields
 * before 10.4, through the mbuf KPI after. */
#ifdef VIRTIONET_KPI
static inline UInt8          *pktData(VirtioNetPacket m) { return (UInt8 *) mbuf_data(m); }
static inline SInt32          pktLen(VirtioNetPacket m)  { return (SInt32) mbuf_len(m); }
static inline VirtioNetPacket pktNext(VirtioNetPacket m) { return mbuf_next(m); }
static inline void pktPlace(VirtioNetPacket m, UInt32 skip, UInt32 len)
{
    mbuf_setdata(m, (UInt8 *) mbuf_data(m) + skip, len);
    mbuf_pkthdr_setlen(m, len);
}
#else
static inline UInt8          *pktData(VirtioNetPacket m) { return mtod(m, UInt8 *); }
static inline SInt32          pktLen(VirtioNetPacket m)  { return m->m_len; }
static inline VirtioNetPacket pktNext(VirtioNetPacket m) { return m->m_next; }
static inline void pktPlace(VirtioNetPacket m, UInt32 skip, UInt32 len)
{
    m->m_data += skip;
    m->m_len = len;
    m->m_pkthdr.len = len;
}
#endif

/* With the BAR mapped, register offsets are relative to it; without, they are absolute I/O ports,
 * the BAR's own address plus the register. */
UInt8  io_virtio_VirtioNet::rd8(UInt16 off)            { return fPCI->ioRead8((fIO ? 0 : fIOBase) + off, fIO); }
UInt16 io_virtio_VirtioNet::rd16(UInt16 off)           { return fPCI->ioRead16((fIO ? 0 : fIOBase) + off, fIO); }
UInt32 io_virtio_VirtioNet::rd32(UInt16 off)           { return fPCI->ioRead32((fIO ? 0 : fIOBase) + off, fIO); }
void   io_virtio_VirtioNet::wr8(UInt16 off, UInt8 v)   { fPCI->ioWrite8((fIO ? 0 : fIOBase) + off, v, fIO); }
void   io_virtio_VirtioNet::wr16(UInt16 off, UInt16 v) { fPCI->ioWrite16((fIO ? 0 : fIOBase) + off, v, fIO); }
void   io_virtio_VirtioNet::wr32(UInt16 off, UInt32 v) { fPCI->ioWrite32((fIO ? 0 : fIOBase) + off, v, fIO); }

/* ------------------------------------------------------------------------------------------ */
/* the device                                                                                  */
/* ------------------------------------------------------------------------------------------ */

void io_virtio_VirtioNet::deviceReset()
{
    wr8(kVirtioStatus, 0);
}

/* Sizes a queue by what the device reports and allocates its rings and slots; the rings are given
 * to the device by queueStart, each time the interface comes up. */
bool io_virtio_VirtioNet::queueAlloc(VirtioNetQueue *q, UInt16 index, UInt16 maxSlots)
{
    UInt32 availOff, usedOff, i;

    wr16(kVirtioQueueSel, index);
    q->index = index;
    q->num = rd16(kVirtioQueueNum);
    if (q->num < 2) {
        IOLog("VirtioNet: queue %d has %d entries\n", index, q->num);
        return false;
    }
    q->slots = q->num / 2;
    if (q->slots > maxSlots)
        q->slots = maxSlots;

    availOff = 16 * q->num;
    usedOff = roundPage(availOff + 6 + 2 * q->num);
    q->memSize = usedOff + roundPage(6 + 8 * q->num);
    q->mem = (UInt8 *) IOMallocContiguous(q->memSize, kPage, &q->memPhys);
    if (!q->mem)
        return false;
    q->desc       = (volatile VirtioNetDesc *) q->mem;
    q->availFlags = (volatile UInt16 *) (q->mem + availOff);
    q->availIdx   = q->availFlags + 1;
    q->availRing  = q->availFlags + 2;
    q->usedFlags  = (volatile UInt16 *) (q->mem + usedOff);
    q->usedIdx    = q->usedFlags + 1;
    q->usedRing   = (volatile VirtioNetUsedElem *) (q->mem + usedOff + 4);

    /* A page at a time: many small physically contiguous pieces are to be had long after one
     * large one is not. */
    q->pageCount = (q->slots + 1) / 2;
    q->pages = (UInt8 **) IOMalloc(q->pageCount * sizeof(UInt8 *));
    q->pagesPhys = (IOPhysicalAddress *) IOMalloc(q->pageCount * sizeof(IOPhysicalAddress));
    if (!q->pages || !q->pagesPhys)
        return false;
    bzero(q->pages, q->pageCount * sizeof(UInt8 *));
    for (i = 0; i < q->pageCount; i++) {
        q->pages[i] = (UInt8 *) IOMallocContiguous(kPage, kPage, &q->pagesPhys[i]);
        if (!q->pages[i])
            return false;
    }
    return true;
}

void io_virtio_VirtioNet::queueFree(VirtioNetQueue *q)
{
    UInt32 i;

    if (q->pages) {
        for (i = 0; i < q->pageCount; i++)
            if (q->pages[i])
                IOFreeContiguous(q->pages[i], kPage);
        IOFree(q->pages, q->pageCount * sizeof(UInt8 *));
    }
    if (q->pagesPhys)
        IOFree(q->pagesPhys, q->pageCount * sizeof(IOPhysicalAddress));
    if (q->mem)
        IOFreeContiguous(q->mem, q->memSize);
    q->pages = 0;
    q->pagesPhys = 0;
    q->mem = 0;
}

UInt8 *io_virtio_VirtioNet::slotAddress(VirtioNetQueue *q, UInt16 slot)
{
    return q->pages[slot / 2] + (slot % 2) * kSlotSize;
}

/* Clears the rings and slots, lays out every slot's chain -- descriptor 2n for its header, 2n+1
 * for its frame -- and gives the rings to the device. */
void io_virtio_VirtioNet::queueStart(VirtioNetQueue *q, bool deviceWrites)
{
    UInt32 i;
    UInt16 flags = deviceWrites ? kDescWrite : 0;

    bzero(q->mem, q->memSize);
    for (i = 0; i < q->pageCount; i++)
        bzero(q->pages[i], kPage);
    for (i = 0; i < q->slots; i++) {
        IOPhysicalAddress phys = q->pagesPhys[i / 2] + (i % 2) * kSlotSize;
        q->desc[2 * i].addr         = phys;
        q->desc[2 * i].len          = kHeaderLen;
        q->desc[2 * i].flags        = flags | kDescNext;
        q->desc[2 * i].next         = 2 * i + 1;
        q->desc[2 * i + 1].addr     = phys + kFrameOffset;
        q->desc[2 * i + 1].len      = kFrameMax;
        q->desc[2 * i + 1].flags    = flags;
        q->desc[2 * i + 1].next     = 0;
    }
    q->lastUsed = 0;
    q->nextAvail = 0;
    *q->availFlags = (fPolled || !deviceWrites) ? kAvailNoInterrupt : 0;
    barrier();
    wr16(kVirtioQueueSel, q->index);
    wr32(kVirtioQueuePFN, (UInt32) (q->memPhys / kPage));
}

/* Makes a slot's chain available; the device is told by queueNotify. */
void io_virtio_VirtioNet::queuePublish(VirtioNetQueue *q, UInt16 slot)
{
    q->availRing[q->nextAvail % q->num] = 2 * slot;
    barrier();
    q->nextAvail++;
    *q->availIdx = q->nextAvail;
}

/* Tells the device unless it said it is looking anyway. */
void io_virtio_VirtioNet::queueNotify(VirtioNetQueue *q)
{
    barrier();
    if (!(*q->usedFlags & kUsedNoNotify))
        wr16(kVirtioQueueNotify, q->index);
}

/* The status word, in the guest's byte order as a legacy device keeps its configuration, read a
 * byte at a time. A link bit that shows up in the high byte alone is taken as the other order. */
UInt16 io_virtio_VirtioNet::readStatus()
{
    UInt16 s = (UInt16) ((rd8(kConfigStatus) << 8) | rd8(kConfigStatus + 1));

    if ((s & 0xFF00) && !(s & 0x00FF))
        s = (UInt16) (s >> 8);
    return s;
}

/* Once, in start: maps the device, reads its address and sizes its queues, and leaves it reset. */
bool io_virtio_VirtioNet::deviceProbe()
{
    UInt16 cmd, i;
    UInt32 features;

    /* The I/O BAR, mapped; failing that, its port number, for IOPCIDevice's own I/O mapping. */
    fIO = fPCI->mapDeviceMemoryWithRegister(kConfigBAR0);
    fIOBase = (UInt16) (fPCI->configRead32(kConfigBAR0) & ~(UInt32) 3);
    cmd = fPCI->configRead16(kConfigCommand);
    fPCI->configWrite16(kConfigCommand, cmd | kCommandIO | kCommandBusMaster);

    deviceReset();
    wr8(kVirtioStatus, kStatusAcknowledge);
    wr8(kVirtioStatus, kStatusAcknowledge | kStatusDriver);
    features = rd32(kVirtioHostFeatures);
    if (features & kFeatureMAC) {
        for (i = 0; i < kIOEthernetAddressSize; i++)
            fMAC.bytes[i] = rd8(kConfigMAC + i);
    } else {
        /* QEMU always offers one; a locally administered address otherwise. */
        static const UInt8 fallback[kIOEthernetAddressSize] = { 0x02, 0x56, 0x4e, 0x00, 0x00, 0x01 };
        bcopy(fallback, fMAC.bytes, kIOEthernetAddressSize);
        IOLog("VirtioNet: the device has no MAC address; using a made-up one\n");
    }

    if (!queueAlloc(&fRx, 0, kRxSlotsMax) || !queueAlloc(&fTx, 1, kTxSlotsMax)) {
        wr8(kVirtioStatus, kStatusFailed);
        return false;
    }
    fTxFree = (UInt16 *) IOMalloc(fTx.slots * sizeof(UInt16));
    if (!fTxFree)
        return false;
    deviceReset();
    return true;
}

/* Each time the interface comes up: negotiates, gives the device the rings and every receive
 * slot. */
bool io_virtio_VirtioNet::deviceStart()
{
    UInt16 i;

    deviceReset();
    wr8(kVirtioStatus, kStatusAcknowledge);
    wr8(kVirtioStatus, kStatusAcknowledge | kStatusDriver);
    fFeatures = rd32(kVirtioHostFeatures) & (kFeatureMAC | kFeatureStatus);
    wr32(kVirtioGuestFeatures, fFeatures);

    queueStart(&fRx, true);
    queueStart(&fTx, false);
    wr8(kVirtioStatus, kStatusAcknowledge | kStatusDriver | kStatusDriverOK);

    for (i = 0; i < fRx.slots; i++)
        queuePublish(&fRx, i);
    queueNotify(&fRx);

    for (i = 0; i < fTx.slots; i++)
        fTxFree[i] = fTx.slots - 1 - i;
    fTxFreeCount = fTx.slots;
    fTxStalled = false;
    return true;
}

/* ------------------------------------------------------------------------------------------ */
/* the work                                                                                    */
/* ------------------------------------------------------------------------------------------ */

/* Hands what the device has received to the interface, and each slot back to the device. */
bool io_virtio_VirtioNet::serviceRx()
{
    bool received = false, returned = false;

    while (fRx.lastUsed != *fRx.usedIdx) {
        volatile VirtioNetUsedElem *e;
        UInt32 id, len;
        UInt16 slot;

        barrier();
        e = &fRx.usedRing[fRx.lastUsed % fRx.num];
        id = e->id;
        len = e->len;
        fRx.lastUsed++;
        slot = (UInt16) (id / 2);
        if ((id & 1) || slot >= fRx.slots) {
            IOLog("VirtioNet: the device returned descriptor %d, which is no receive chain\n", (int) id);
            fNetStats->inputErrors++;
            continue;
        }
        if (len < kHeaderLen + kFrameMin || len > kHeaderLen + kFrameMax) {
            fNetStats->inputErrors++;
        } else {
            UInt32 frame = len - kHeaderLen;
            /* Two bytes more and the data moved up by them, so that the IP header behind the
             * 14-byte Ethernet header lands on a word. */
            VirtioNetPacket m = allocatePacket(frame + 2);
            if (!m) {
                fNetStats->inputErrors++;
            } else {
                pktPlace(m, 2, frame);
                bcopy(slotAddress(&fRx, slot) + kFrameOffset, pktData(m), frame);
                fNetif->inputPacket(m, frame, IONetworkInterface::kInputOptionQueuePacket);
                fNetStats->inputPackets++;
                received = true;
            }
        }
        queuePublish(&fRx, slot);
        returned = true;
    }
    if (returned)
        queueNotify(&fRx);
    if (received)
        fNetif->flushInputQueue();
    return received;
}

/* Takes back the transmit slots the device is done with. */
void io_virtio_VirtioNet::serviceTx()
{
    while (fTx.lastUsed != *fTx.usedIdx) {
        UInt32 id;

        barrier();
        id = fTx.usedRing[fTx.lastUsed % fTx.num].id;
        fTx.lastUsed++;
        if ((id & 1) || id / 2 >= fTx.slots || fTxFreeCount >= fTx.slots) {
            IOLog("VirtioNet: the device returned descriptor %d, which is no transmit chain in use\n", (int) id);
            continue;
        }
        fTxFree[fTxFreeCount++] = (UInt16) (id / 2);
    }
}

void io_virtio_VirtioNet::service()
{
    if (!fEnabled)
        return;
    serviceTx();
    serviceRx();
    if (fTxStalled && fTxFreeCount) {
        fTxStalled = false;
        *fTx.availFlags = kAvailNoInterrupt;
        fOutputQueue->service(IOBasicOutputQueue::kServiceAsync);
    }
}

void io_virtio_VirtioNet::updateLink()
{
    UInt16 status = (fFeatures & kFeatureStatus) ? readStatus() : kNetLinkUp;

    if (fLinkReported && (status & kNetLinkUp) == (fLinkStatus & kNetLinkUp))
        return;
    if (fLinkReported)
        IOLog("VirtioNet: link %s\n", (status & kNetLinkUp) ? "up" : "down");
    fLinkStatus = status;
    fLinkReported = true;
    if (status & kNetLinkUp)
        setLinkStatus(kIONetworkLinkValid | kIONetworkLinkActive, fMedium, kLinkSpeed);
    else
        setLinkStatus(kIONetworkLinkValid, 0);
}

/* At primary interrupt time: reading the ISR lowers the device's interrupt line. The bits are kept
 * for the action, which the work loop runs. */
bool io_virtio_VirtioNet::interruptFilter(OSObject *owner, IOFilterInterruptEventSource *)
{
    io_virtio_VirtioNet *me = (io_virtio_VirtioNet *) owner;
    UInt8 isr = me->rd8(kVirtioISR);

    if (!isr)
        return false;   /* someone else's, on a shared line */
    OSBitOrAtomic(isr, (UInt32 *) &me->fISR);
    OSIncrementAtomic((SInt32 *) &me->fInterrupts);
    return true;
}

void io_virtio_VirtioNet::interruptAction(OSObject *owner, IOInterruptEventSource *, int count)
{
    ((io_virtio_VirtioNet *) owner)->interruptOccurred(count);
}

void io_virtio_VirtioNet::interruptOccurred(int)
{
    UInt32 isr;

    do {
        isr = fISR;
    } while (!OSCompareAndSwap(isr, 0, (UInt32 *) &fISR));
    if (!fEnabled)
        return;
    if (isr & kISRConfig)
        updateLink();
    service();
}

void io_virtio_VirtioNet::timeoutAction(OSObject *owner, IOTimerEventSource *)
{
    ((io_virtio_VirtioNet *) owner)->timeoutOccurred();
}

/* Polled, the timer does all the work. Otherwise it is a watchdog: it does what an interrupt may
 * have missed, and if work keeps waiting that no interrupt announced -- the platform did not route
 * the line -- the driver polls from then on. */
void io_virtio_VirtioNet::timeoutOccurred()
{
    if (!fEnabled)
        return;
    if (fPolled) {
        service();
        if (++fLinkTicks >= kLinkPollTicks) {
            fLinkTicks = 0;
            updateLink();
        }
        fTimer->setTimeoutMS(kPollMS);
        return;
    }

    if (fRx.lastUsed != *fRx.usedIdx && fInterrupts == fInterruptsSeen) {
        if (++fQuietTicks >= kQuietTicksMax) {
            IOLog("VirtioNet: received packets raise no interrupt; polling every %d ms instead\n", kPollMS);
            fInterrupt->disable();
            fPolled = true;
            *fRx.availFlags = kAvailNoInterrupt;
            *fTx.availFlags = kAvailNoInterrupt;
        }
    } else
        fQuietTicks = 0;
    fInterruptsSeen = fInterrupts;
    service();
    updateLink();
    fTimer->setTimeoutMS(fPolled ? kPollMS : kWatchdogMS);
}

/* ------------------------------------------------------------------------------------------ */
/* the controller                                                                              */
/* ------------------------------------------------------------------------------------------ */

bool io_virtio_VirtioNet::start(IOService *provider)
{
    OSBoolean *polled;
    IONetworkMedium *medium;

    if (!super::start(provider))
        return false;

    fPCI = OSDynamicCast(IOPCIDevice, provider);
    if (!fPCI)
        return false;
    fPCI->retain();
    if (!fPCI->open(this))
        goto fail;

    fOutputQueue = getOutputQueue();
    if (!fOutputQueue)
        goto fail;

    polled = OSDynamicCast(OSBoolean, getProperty("Polled"));
    fPolled = polled && polled->isTrue();

    if (!deviceProbe())
        goto fail;

    fTimer = IOTimerEventSource::timerEventSource(this, timeoutAction);
    if (!fTimer || fWorkLoop->addEventSource(fTimer) != kIOReturnSuccess)
        goto fail;
    if (!fPolled) {
        fInterrupt = IOFilterInterruptEventSource::filterInterruptEventSource(this, interruptAction,
                                                                              interruptFilter, provider, 0);
        if (!fInterrupt || fWorkLoop->addEventSource(fInterrupt) != kIOReturnSuccess) {
            IOLog("VirtioNet: the device's interrupt cannot be had; polling every %d ms instead\n", kPollMS);
            if (fInterrupt)
                fInterrupt->release();
            fInterrupt = 0;
            fPolled = true;
        }
    }

    /* One medium, always selected: the link is whatever QEMU's backend is. */
    fMediumDict = OSDictionary::withCapacity(1);
    if (!fMediumDict)
        goto fail;
    medium = IONetworkMedium::medium(kIOMediumEthernetAuto, kLinkSpeed);
    if (!medium)
        goto fail;
    IONetworkMedium::addMedium(fMediumDict, medium);
    medium->release();
    if (!publishMediumDictionary(fMediumDict))
        goto fail;
    fMedium = IONetworkMedium::getMediumWithType(fMediumDict, kIOMediumEthernetAuto);
    setSelectedMedium(fMedium);

    IOLog("VirtioNet: %02x:%02x:%02x:%02x:%02x:%02x, I/O at 0x%x%s, %d receive and %d transmit slots, %s\n",
          fMAC.bytes[0], fMAC.bytes[1], fMAC.bytes[2], fMAC.bytes[3], fMAC.bytes[4], fMAC.bytes[5],
          fIOBase, fIO ? "" : " (through the bridge)", fRx.slots, fTx.slots,
          fPolled ? "polled" : "interrupt-driven");

    /*
     * Ask to be en0. On PowerPC, 10.2 and 10.3's InterfaceNamer keeps unit 0 for the interface the
     * kernel calls primary (10.3.3 on: built-in), and IONetworkInterface decides that, when the
     * interface is made, from a "built-in" property on the controller's provider -- which Apple's
     * Open Firmware puts on the onboard Ethernet and OpenBIOS puts on nothing. Without it the
     * interface came up as en1, and 10.3's login window refused every console login: its MCX check
     * reads the machine's address from en0 ("canMCXUserLogin: Can't get machine address 5"), live
     * on panther-guest on 2026-10-06. A disk that already named the interface keeps the name it has in
     * /Library/Preferences/SystemConfiguration/NetworkInterfaces.plist.
     */
    if (!provider->getProperty("built-in")) {
        OSData *builtin = OSData::withCapacity(0);
        if (builtin) {
            provider->setProperty("built-in", builtin);
            builtin->release();
        }
    }
    if (!attachInterface((IONetworkInterface **) &fNetif, false))
        goto fail;
    fNetif->registerService();
    IOLog("VirtioNet: interface attached\n");
    return true;

fail:
    IOLog("VirtioNet: could not start\n");
    stop(provider);
    return false;
}

void io_virtio_VirtioNet::stop(IOService *provider)
{
    fEnabled = false;
    if (fTimer)
        fTimer->cancelTimeout();
    if (fInterrupt)
        fInterrupt->disable();
    if (fPCI && (fIO || fIOBase))
        deviceReset();
    if (fPCI && fPCI->isOpen(this))
        fPCI->close(this);
    super::stop(provider);
}

/* Refused once the interface is attached, which is from start on. These releases cannot take an
 * interface out of the BSD stack again: kextunload terminated the driver, unloaded the module and
 * said it succeeded, and en2 stayed listed by ifconfig with its controller freed and its code gone,
 * for the next ifconfig to call into (live, 10.2.8, 2026-10-01). kextunload terminates a module's
 * drivers before it unloads it and gives up at the first that refuses (../ppc/virtio9p). */
bool io_virtio_VirtioNet::terminate(IOOptionBits options)
{
    if (fNetif) {
        IOLog("VirtioNet: %s%d cannot be detached from the network stack; not unloading\n",
              fNetif->getNamePrefix(), (int) fNetif->getUnitNumber());
        return false;
    }
    return super::terminate(options);
}

void io_virtio_VirtioNet::free()
{
    if (fWorkLoop) {
        if (fInterrupt)
            fWorkLoop->removeEventSource(fInterrupt);
        if (fTimer)
            fWorkLoop->removeEventSource(fTimer);
    }
    if (fInterrupt)
        fInterrupt->release();
    if (fTimer)
        fTimer->release();
    if (fNetif)
        fNetif->release();
    if (fMediumDict)
        fMediumDict->release();
    queueFree(&fRx);
    queueFree(&fTx);
    if (fTxFree)
        IOFree(fTxFree, fTx.slots * sizeof(UInt16));
    if (fIO)
        fIO->release();
    if (fPCI)
        fPCI->release();
    if (fWorkLoop)
        fWorkLoop->release();
    super::free();
}

/* IONetworkController::start asks for the work loop before it makes the output queue on it. */
bool io_virtio_VirtioNet::createWorkLoop()
{
    fWorkLoop = IOWorkLoop::workLoop();
    return fWorkLoop != 0;
}

IOWorkLoop *io_virtio_VirtioNet::getWorkLoop() const
{
    return fWorkLoop;
}

IOOutputQueue *io_virtio_VirtioNet::createOutputQueue()
{
    return IOGatedOutputQueue::withTarget(this, getWorkLoop());
}

bool io_virtio_VirtioNet::configureInterface(IONetworkInterface *netif)
{
    IONetworkData *data;

    if (!super::configureInterface(netif))
        return false;
    data = netif->getParameter(kIONetworkStatsKey);
    if (!data || !(fNetStats = (IONetworkStats *) data->getBuffer()))
        return false;
    return true;
}

/* ifconfig up, through IONetworkController's command gate and so on the work loop. */
IOReturn io_virtio_VirtioNet::enable(IONetworkInterface *)
{
    if (fEnabled)
        return kIOReturnSuccess;
    if (!deviceStart())
        return kIOReturnIOError;
    fEnabled = true;
    fQuietTicks = 0;
    fInterruptsSeen = fInterrupts;
    if (fInterrupt && !fPolled)
        fInterrupt->enable();
    fTimer->setTimeoutMS(fPolled ? kPollMS : kWatchdogMS);
    fLinkReported = false;
    updateLink();
    fOutputQueue->setCapacity(kOutputQueueSize);
    fOutputQueue->start();
    IOLog("VirtioNet: %s%d up\n", fNetif->getNamePrefix(), (int) fNetif->getUnitNumber());
    return kIOReturnSuccess;
}

IOReturn io_virtio_VirtioNet::disable(IONetworkInterface *)
{
    if (!fEnabled)
        return kIOReturnSuccess;
    fEnabled = false;
    fOutputQueue->stop();
    fOutputQueue->setCapacity(0);
    fOutputQueue->flush();
    fTimer->cancelTimeout();
    if (fInterrupt)
        fInterrupt->disable();
    deviceReset();
    setLinkStatus(0, 0);
    return kIOReturnSuccess;
}

/* Copies the frame into a free slot and hands it to the device. With every slot in flight the
 * queue stalls, an interrupt is asked for, and service() restarts it when a slot comes back. */
UInt32 io_virtio_VirtioNet::outputPacket(VirtioNetPacket m, void *)
{
    VirtioNetPacket n;
    UInt8 *dst;
    UInt32 total = 0;
    UInt16 slot;

    if (!fEnabled) {
        freePacket(m);
        return kIOReturnOutputDropped;
    }
    serviceTx();
    if (fTxFreeCount == 0) {
        if (!fPolled)
            *fTx.availFlags = 0;
        barrier();
        serviceTx();   /* a slot may have come back before the device saw the flag */
        if (fTxFreeCount == 0) {
            fTxStalled = true;
            return kIOReturnOutputStall;
        }
        *fTx.availFlags = kAvailNoInterrupt;
    }

    slot = fTxFree[fTxFreeCount - 1];
    dst = slotAddress(&fTx, slot) + kFrameOffset;
    for (n = m; n; n = pktNext(n)) {
        SInt32 len = pktLen(n);

        if (len <= 0)
            continue;
        if (total + len > kFrameMax) {
            fNetStats->outputErrors++;
            freePacket(m);
            return kIOReturnOutputDropped;
        }
        bcopy(pktData(n), dst + total, len);
        total += len;
    }
    if (total < kFrameMin) {
        fNetStats->outputErrors++;
        freePacket(m);
        return kIOReturnOutputDropped;
    }
    fTxFreeCount--;
    fTx.desc[2 * slot + 1].len = total;
    queuePublish(&fTx, slot);
    queueNotify(&fTx);
    fNetStats->outputPackets++;
    freePacket(m);
    return kIOReturnOutputSuccess;
}

void io_virtio_VirtioNet::getPacketBufferConstraints(IOPacketBufferConstraints *constraints) const
{
    constraints->alignStart  = kIOPacketBufferAlign4;
    constraints->alignLength = kIOPacketBufferAlign1;
}

const OSString *io_virtio_VirtioNet::newVendorString() const
{
    return OSString::withCString("Virtio");
}

const OSString *io_virtio_VirtioNet::newModelString() const
{
    return OSString::withCString("virtio-net (legacy PCI)");
}

IOReturn io_virtio_VirtioNet::selectMedium(const IONetworkMedium *medium)
{
    if (medium && medium != fMedium)
        return kIOReturnUnsupported;
    setSelectedMedium(fMedium);
    return kIOReturnSuccess;
}

IOReturn io_virtio_VirtioNet::getHardwareAddress(IOEthernetAddress *addr)
{
    bcopy(&fMAC, addr, sizeof(*addr));
    return kIOReturnSuccess;
}

/* Without the control queue there is nothing to tell the device: it delivers every frame on the
 * link whatever is asked, and the stack above discards what is not for it. */
IOReturn io_virtio_VirtioNet::setPromiscuousMode(bool)
{
    return kIOReturnSuccess;
}

IOReturn io_virtio_VirtioNet::setMulticastMode(bool)
{
    return kIOReturnSuccess;
}

IOReturn io_virtio_VirtioNet::setMulticastList(IOEthernetAddress *, UInt32)
{
    return kIOReturnSuccess;
}
