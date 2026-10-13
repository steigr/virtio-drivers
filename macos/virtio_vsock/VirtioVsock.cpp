/*
 * VirtioVsock: see VirtioVsock.h for what this is and why it is written the way it is.
 *
 * The transport is written from the virtio 1.x specification ("Virtio Over PCI Bus", "Socket
 * Device") and the modern-device handling of elliotnunn/classicvirtio's transport-ndrv.c; the IOKit
 * side (the work loop, the filter interrupt, the page-sized contiguous slots) follows VirtioNet.cpp.
 */

#include "VirtioVsock.h"

#include <IOKit/IOLib.h>
#include <libkern/OSAtomic.h>

#ifndef THREAD_ABORTSAFE
#define THREAD_ABORTSAFE 2
#endif

#define super IOService
OSDefineMetaClassAndStructors(io_virtio_VirtioVsock, IOService)

/* Modern virtio PCI: the structures named by vendor capabilities (id 9) in configuration space. */
enum {
    kCapCommon   = 1,
    kCapNotify   = 2,
    kCapISR      = 3,
    kCapDevice   = 4,
    kCapPCICfg   = 5
};

/* struct virtio_pci_common_cfg */
enum {
    kComDeviceFeatureSel = 0x00,    /* 32 */
    kComDeviceFeature    = 0x04,    /* 32 */
    kComDriverFeatureSel = 0x08,    /* 32 */
    kComDriverFeature    = 0x0C,    /* 32 */
    kComNumQueues        = 0x12,    /* 16 */
    kComStatus           = 0x14,    /*  8 */
    kComQueueSelect      = 0x16,    /* 16 */
    kComQueueSize        = 0x18,    /* 16 */
    kComQueueMSIX        = 0x1A,    /* 16 */
    kComQueueEnable      = 0x1C,    /* 16 */
    kComQueueNotifyOff   = 0x1E,    /* 16 */
    kComQueueDesc        = 0x20,    /* 64 */
    kComQueueDriver      = 0x28,    /* 64 */
    kComQueueDevice      = 0x30     /* 64 */
};

enum {
    kStatusAcknowledge = 0x01,
    kStatusDriver      = 0x02,
    kStatusDriverOK    = 0x04,
    kStatusFeaturesOK  = 0x08,
    kStatusFailed      = 0x80
};

enum {
    kISRQueue          = 0x01,
    kISRConfig         = 0x02
};

enum {
    kFeatureVersion1   = 1,         /* bit 32, as bit 0 of the second word */
    kFeatureStream     = 0x00000001 /* VIRTIO_VSOCK_F_STREAM, bit 0, offered by newer devices */
};

enum {
    kDescNext          = 1,
    kDescWrite         = 2,
    kAvailNoInterrupt  = 1
};

/* The socket device: configuration, header, operations. */
enum {
    kHdrSrcCid   = 0,       /* 64 */
    kHdrDstCid   = 8,       /* 64 */
    kHdrSrcPort  = 16,      /* 32 */
    kHdrDstPort  = 20,      /* 32 */
    kHdrLen      = 24,      /* 32 */
    kHdrType     = 28,      /* 16 */
    kHdrOp       = 30,      /* 16 */
    kHdrFlags    = 32,      /* 32 */
    kHdrBufAlloc = 36,      /* 32 */
    kHdrFwdCnt   = 40,      /* 32 */
    kHdrSize     = 44
};

enum {
    kTypeStream  = 1
};

enum {
    kOpInvalid       = 0,
    kOpRequest       = 1,
    kOpResponse      = 2,
    kOpRst           = 3,
    kOpShutdown      = 4,
    kOpRW            = 5,
    kOpCreditUpdate  = 6,
    kOpCreditRequest = 7
};

enum {
    kShutdownRcv     = 1,
    kShutdownSend    = 2
};

enum {
    kEventTransportReset = 0
};

enum {
    kCommandMemory     = 0x0002,
    kCommandBusMaster  = 0x0004,
    kCommandIntxOff    = 0x0400,        /* PCI 2.3's Interrupt Disable */
    kConfigCommand     = 0x04,
    kConfigStatus      = 0x04,      /* the status is its upper half */
    kConfigCapPtr      = 0x34,
    kStatusCapList     = 0x10
};

enum {
    kPage              = 4096,
    kSlotSize          = 2048,      /* two slots to a page */
    kPayloadMax        = 2000,      /* what one packet carries: a slot less the header, kept to words */
    kRxSlotsMax        = 64,
    kTxSlotsMax        = 32,
    kEvSlots           = 4,
    kTxReserve         = 6,         /* transmit slots data may not take: for control packets */
    kConnBuf           = 32768,     /* what each connection can buffer, as it tells the peer */
    kPollMS            = 10,
    kWatchdogMS        = 100,
    kQuietTicksMax     = 20,
    kConnectDefaultTicks = 100,     /* in 100 ms: ten seconds */
    kClosingTicks      = 80,
    kPortFirst         = 1024
};

/* A command to the work loop. */
struct VsockCmd {
    int             op;
    void           *client;
    UInt32          a[4];
    UInt8          *data;           /* read: where the bytes go; write: the bytes */
    UInt32          dataLen;        /* read: room for them; write: how many */
    UInt32          out[4];
    IOReturn        result;
};

static inline UInt32 roundPage(UInt32 n) { return (n + kPage - 1) & ~(UInt32)(kPage - 1); }

/* PowerPC's sync, which GCC 2.95 and 3 both take in this spelling; on Intel mfence, as VirtioNet. */
#if defined(__ppc__)
static inline void barrier() { __asm__ __volatile__ ("sync" : : : "memory"); }
#elif defined(__i386__) || defined(__x86_64__)
static inline void barrier() { __asm__ __volatile__ ("mfence" : : : "memory"); }
#else
#error "VirtioVsock knows PowerPC and Intel"
#endif

/* Little-endian, a byte at a time: for the rings and slots, which the device reads as memory. */
static inline UInt16 get16(const volatile UInt8 *p) { return (UInt16) (p[0] | (p[1] << 8)); }
static inline UInt32 get32(const volatile UInt8 *p)
{
    return (UInt32) p[0] | ((UInt32) p[1] << 8) | ((UInt32) p[2] << 16) | ((UInt32) p[3] << 24);
}
static inline void put16(volatile UInt8 *p, UInt16 v) { p[0] = (UInt8) v; p[1] = (UInt8) (v >> 8); }
static inline void put32(volatile UInt8 *p, UInt32 v)
{
    p[0] = (UInt8) v; p[1] = (UInt8) (v >> 8); p[2] = (UInt8) (v >> 16); p[3] = (UInt8) (v >> 24);
}

/* The BAR's registers are little-endian. PowerPC reverses them on the way, with an eieio after a
 * store so that stores reach the device in order; on Intel they are the processor's own order, and
 * uncached device memory keeps stores in order by itself, so only the compiler is held back. */
#if defined(__ppc__)
static inline UInt32 ld32le(volatile UInt8 *p)
{
    UInt32 v;
    __asm__ __volatile__ ("lwbrx %0,0,%1" : "=r" (v) : "r" (p) : "memory");
    return v;
}
static inline UInt16 ld16le(volatile UInt8 *p)
{
    UInt32 v;
    __asm__ __volatile__ ("lhbrx %0,0,%1" : "=r" (v) : "r" (p) : "memory");
    return (UInt16) v;
}
static inline void st32le(volatile UInt8 *p, UInt32 v)
{
    __asm__ __volatile__ ("stwbrx %0,0,%1; eieio" : : "r" (v), "r" (p) : "memory");
}
static inline void st16le(volatile UInt8 *p, UInt16 v)
{
    __asm__ __volatile__ ("sthbrx %0,0,%1; eieio" : : "r" ((UInt32) v), "r" (p) : "memory");
}
static inline void st8(volatile UInt8 *p, UInt8 v)
{
    *p = v;
    __asm__ __volatile__ ("eieio" : : : "memory");
}
#else
static inline UInt32 ld32le(volatile UInt8 *p) { return *(volatile UInt32 *) p; }
static inline UInt16 ld16le(volatile UInt8 *p) { return *(volatile UInt16 *) p; }
static inline void st32le(volatile UInt8 *p, UInt32 v)
{
    *(volatile UInt32 *) p = v;
    __asm__ __volatile__ ("" : : : "memory");
}
static inline void st16le(volatile UInt8 *p, UInt16 v)
{
    *(volatile UInt16 *) p = v;
    __asm__ __volatile__ ("" : : : "memory");
}
static inline void st8(volatile UInt8 *p, UInt8 v)
{
    *p = v;
    __asm__ __volatile__ ("" : : : "memory");
}
#endif

/* A physical address's high half: zero on a 32-bit kernel, whose IOPhysicalAddress is 32 bits;
 * on the 64-bit kernels of 10.6 on it is 64, and memory above 4 GB is the device's to reach. */
static inline UInt32 physHi(IOPhysicalAddress a)
{
#if defined(__LP64__)
    return (UInt32) ((UInt64) a >> 32);
#else
    (void) a;
    return 0;
#endif
}

/* ------------------------------------------------------------------------------------------ */
/* the transport                                                                               */
/* ------------------------------------------------------------------------------------------ */

/* Interrupts off on this CPU and back as they were, around a configuration-window access that the
 * interrupt filter must not cut into. Spelled out because ml_set_interrupts_enabled, which
 * IOSimpleLockLockDisableInterrupt is inline over, is not in the KPI 10.4 links against
 * (xnu-792.25.20's config/*.exports). Intel only: PowerPC's BARs are mapped, never windowed. */
#if defined(__i386__) || defined(__x86_64__)
static inline unsigned long interruptsOff()
{
    unsigned long flags;

    __asm__ __volatile__ ("pushf; pop %0; cli" : "=r" (flags) : : "memory");
    return flags;
}

static inline void interruptsRestore(unsigned long flags)
{
    __asm__ __volatile__ ("push %0; popf" : : "r" (flags) : "memory", "cc");
}
#endif

/* A register of one of the structures: through its mapping, or, where that could not be had,
 * through the PCI_CFG capability's window into the BAR. Values are the register's, in the host's
 * order. */
UInt32 io_virtio_VirtioVsock::regRead(VsockRegion *r, UInt32 off, int size)
{
    if (r->virt) {
        volatile UInt8 *p = r->virt + off;
        UInt32 v;
        if (size == 4)
            v = ld32le(p);
        else if (size == 2)
            v = ld16le(p);
        else
            v = *p;
        return v;
    }
    if (!fCfgWindowOff)
        return 0;
    {
        UInt32 v;
#if defined(__i386__) || defined(__x86_64__)
        unsigned long flags = interruptsOff();

        IOSimpleLockLock(fWindowLock);
#endif
        fPCI->configWrite32(fCfgWindowOff + 4, r->bar);
        fPCI->configWrite32(fCfgWindowOff + 8, r->offset + off);
        fPCI->configWrite32(fCfgWindowOff + 12, size);
        v = fPCI->configRead32(fCfgWindowOff + 16);
#if defined(__i386__) || defined(__x86_64__)
        IOSimpleLockUnlock(fWindowLock);
        interruptsRestore(flags);
#endif
        return size == 4 ? v : (v & ((1UL << (8 * size)) - 1));
    }
}

void io_virtio_VirtioVsock::regWrite(VsockRegion *r, UInt32 off, int size, UInt32 value)
{
    if (r->virt) {
        volatile UInt8 *p = r->virt + off;
        if (size == 4)
            st32le(p, value);
        else if (size == 2)
            st16le(p, (UInt16) value);
        else
            st8(p, (UInt8) value);
        return;
    }
    if (!fCfgWindowOff)
        return;
    {
#if defined(__i386__) || defined(__x86_64__)
        unsigned long flags = interruptsOff();

        IOSimpleLockLock(fWindowLock);
#endif
        fPCI->configWrite32(fCfgWindowOff + 4, r->bar);
        fPCI->configWrite32(fCfgWindowOff + 8, r->offset + off);
        fPCI->configWrite32(fCfgWindowOff + 12, size);
        fPCI->configWrite32(fCfgWindowOff + 16, value);
#if defined(__i386__) || defined(__x86_64__)
        IOSimpleLockUnlock(fWindowLock);
        interruptsRestore(flags);
#endif
    }
}

UInt8  io_virtio_VirtioVsock::cfgRead8(UInt32 off)              { return (UInt8) regRead(&fCommon, off, 1); }
UInt16 io_virtio_VirtioVsock::cfgRead16(UInt32 off)             { return (UInt16) regRead(&fCommon, off, 2); }
UInt32 io_virtio_VirtioVsock::cfgRead32(UInt32 off)             { return regRead(&fCommon, off, 4); }
void   io_virtio_VirtioVsock::cfgWrite8(UInt32 off, UInt8 v)    { regWrite(&fCommon, off, 1, v); }
void   io_virtio_VirtioVsock::cfgWrite16(UInt32 off, UInt16 v)  { regWrite(&fCommon, off, 2, v); }
void   io_virtio_VirtioVsock::cfgWrite32(UInt32 off, UInt32 v)  { regWrite(&fCommon, off, 4, v); }

bool io_virtio_VirtioVsock::mapBar(UInt8 bar)
{
    if (bar > 5)
        return false;
    /* The address IOPCIFamily recorded for the BAR is mapped, and it must be the one the device
     * decodes: where they differ, the mapping reads and writes somewhere else. Live on 2026-10-12,
     * twice: 10.4.11's 32-bit kernel recorded 0x4000 for a BAR 4 the firmware had put at
     * 0x800004000 -- RAM, which 0.2.0 wrote the device status into -- and 10.8.5's configurator
     * recorded 0x81004000 for the same BAR, which the device did not move to; both read zeros where
     * VIRTIO_F_VERSION_1 should have been. Such a BAR is left unmapped and its registers are reached
     * through the PCI_CFG capability's window instead. Compared in 32-bit halves, for GCC 2.95. */
    {
        UInt32 lo = fPCI->configRead32(0x10 + 4 * bar), hi = 0;
        IODeviceMemory *mem = fPCI->getDeviceMemoryWithRegister(0x10 + 4 * bar);

        if ((lo & 0x7) == 0x4 && bar < 5)
            hi = fPCI->configRead32(0x10 + 4 * bar + 4);
        if (mem) {
            IOPhysicalAddress rec = mem->getPhysicalAddress();

            if ((UInt32) rec != (lo & ~0xFU) || physHi(rec) != hi) {
                IOLog("VirtioVsock: BAR %d decodes at %08x%08x and was recorded at %08x%08x; not mapping it\n",
                      bar, (unsigned int) hi, (unsigned int) (lo & ~0xFU),
                      (unsigned int) physHi(rec), (unsigned int) rec);
                return false;
            }
        }
    }
    if (!fMaps[bar])
        fMaps[bar] = fPCI->mapDeviceMemoryWithRegister(0x10 + 4 * bar);
    return fMaps[bar] != 0;
}

/* Walks the capability list for the structures and maps their BARs. Capabilities that name a BAR
 * that could not be mapped are reached through the PCI_CFG window, if the device has one. */
bool io_virtio_VirtioVsock::findCaps()
{
    UInt32 cap, guard = 0;
    bool haveCommon = false, haveNotify = false, haveISR = false, haveDevice = false;

    if (!((fPCI->configRead32(kConfigStatus) >> 16) & kStatusCapList))
        return false;
    cap = fPCI->configRead32(kConfigCapPtr) & 0xFC;
    while (cap && guard++ < 48) {
        UInt32 w0 = fPCI->configRead32(cap);
        UInt8 vndr = (UInt8) w0, next = (UInt8) (w0 >> 8), type = (UInt8) (w0 >> 24);

        if (vndr == 9) {
            VsockRegion r;
            r.bar = (UInt8) fPCI->configRead32(cap + 4);
            r.offset = fPCI->configRead32(cap + 8);
            r.length = fPCI->configRead32(cap + 12);
            r.virt = 0;
            r.capOff = (UInt16) cap;
            if (type == kCapPCICfg) {
                fCfgWindowOff = (UInt16) cap;
            } else if (type >= kCapCommon && type <= kCapDevice) {
                if (type == kCapCommon && !haveCommon)       { fCommon = r; haveCommon = true; }
                else if (type == kCapNotify && !haveNotify) {
                    fNotify = r; haveNotify = true;
                    fNotifyMult = fPCI->configRead32(cap + 16);
                }
                else if (type == kCapISR && !haveISR)        { fISRReg = r; haveISR = true; }
                else if (type == kCapDevice && !haveDevice)  { fDevice = r; haveDevice = true; }
            }
        }
        cap = next & 0xFC;
    }
    if (!haveCommon || !haveNotify || !haveISR || !haveDevice)
        return false;

    /* Only reads of the window need the memory space off; mapping is all the rest. */
    {
        VsockRegion *all[4];
        int i;
        all[0] = &fCommon; all[1] = &fNotify; all[2] = &fISRReg; all[3] = &fDevice;
        for (i = 0; i < 4; i++) {
            if (mapBar(all[i]->bar) && fMaps[all[i]->bar]->getLength() >= all[i]->offset + all[i]->length) {
                all[i]->virt = (UInt8 *) fMaps[all[i]->bar]->getVirtualAddress() + all[i]->offset;
                /* regRead and regWrite are relative to the structure */
            } else {
                IOLog("VirtioVsock: BAR %d is not mapped%s\n", all[i]->bar,
                      fCfgWindowOff ? "; using the PCI configuration window" : "");
                if (!fCfgWindowOff)
                    return false;
            }
        }
    }
    return true;
}

void io_virtio_VirtioVsock::deviceReset()
{
    int i;

    cfgWrite8(kComStatus, 0);
    for (i = 0; i < 1000 && cfgRead8(kComStatus) != 0; i++)
        IODelay(100);
}

bool io_virtio_VirtioVsock::queueAlloc(VsockQueue *q, UInt16 index, UInt16 maxSlots)
{
    UInt32 i;

    cfgWrite16(kComQueueSelect, index);
    q->index = index;
    q->num = cfgRead16(kComQueueSize);
    if (q->num < 2) {
        IOLog("VirtioVsock: queue %d has %d entries\n", index, q->num);
        return false;
    }
    q->slots = q->num < maxSlots ? q->num : maxSlots;

    q->availOff = 16 * q->num;
    q->usedOff = roundPage(q->availOff + 6 + 2 * q->num);
    q->memSize = q->usedOff + roundPage(6 + 8 * q->num);
    q->mem = (UInt8 *) IOMallocContiguous(q->memSize, kPage, &q->memPhys);
    if (!q->mem)
        return false;

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

void io_virtio_VirtioVsock::queueFree(VsockQueue *q)
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

UInt8 *io_virtio_VirtioVsock::slotAddress(VsockQueue *q, UInt16 slot)
{
    return q->pages[slot / 2] + (slot % 2) * kSlotSize;
}

/* Lays out the descriptors and hands the three parts of the ring to the device. A device-writable
 * queue's slots are all given to it by the caller. */
bool io_virtio_VirtioVsock::queueSetup(VsockQueue *q)
{
    UInt32 i;
    bool deviceWrites = q->index != 1;
    UInt32 slotLen = q->index == 2 ? 16 : kSlotSize;

    bzero(q->mem, q->memSize);
    for (i = 0; i < q->pageCount; i++)
        bzero(q->pages[i], kPage);
    for (i = 0; i < q->slots; i++) {
        volatile UInt8 *d = q->mem + 16 * i;
        IOPhysicalAddress phys = q->pagesPhys[i / 2] + (i % 2) * kSlotSize;
        put32(d, (UInt32) phys);
        put32(d + 4, physHi(phys));
        put32(d + 8, slotLen);
        put16(d + 12, deviceWrites ? kDescWrite : 0);
    }
    q->lastUsed = 0;
    q->nextAvail = 0;
    put16(q->mem + q->availOff, (fPolled || q->index == 1) ? kAvailNoInterrupt : 0);
    barrier();

    cfgWrite16(kComQueueSelect, q->index);
    cfgWrite16(kComQueueSize, q->num);
    cfgWrite16(kComQueueMSIX, 0xFFFF);
    cfgWrite32(kComQueueDesc,       (UInt32) q->memPhys);
    cfgWrite32(kComQueueDesc + 4,   physHi(q->memPhys));
    cfgWrite32(kComQueueDriver,     (UInt32) (q->memPhys + q->availOff));
    cfgWrite32(kComQueueDriver + 4, physHi(q->memPhys + q->availOff));
    cfgWrite32(kComQueueDevice,     (UInt32) (q->memPhys + q->usedOff));
    cfgWrite32(kComQueueDevice + 4, physHi(q->memPhys + q->usedOff));
    q->notifyOff = cfgRead16(kComQueueNotifyOff);
    cfgWrite16(kComQueueEnable, 1);
    return true;
}

void io_virtio_VirtioVsock::queuePublish(VsockQueue *q, UInt16 slot)
{
    volatile UInt8 *avail = q->mem + q->availOff;

    put16(avail + 4 + 2 * (q->nextAvail % q->num), slot);
    barrier();
    q->nextAvail++;
    put16(avail + 2, q->nextAvail);
}

void io_virtio_VirtioVsock::queueNotify(VsockQueue *q)
{
    barrier();
    regWrite(&fNotify, (UInt32) q->notifyOff * fNotifyMult, 2, q->index);
}

bool io_virtio_VirtioVsock::deviceProbe()
{
    UInt32 cmd, f0, f1;

    cmd = fPCI->configRead16(kConfigCommand);
    /* INTx on: the interrupt is the device's only one, MSI-X being left off. */
    fPCI->configWrite16(kConfigCommand, (cmd | kCommandMemory | kCommandBusMaster) & ~kCommandIntxOff);

    if (!findCaps()) {
        IOLog("VirtioVsock: the device has no usable modern virtio PCI capabilities\n");
        return false;
    }

    deviceReset();
    cfgWrite8(kComStatus, kStatusAcknowledge);
    cfgWrite8(kComStatus, kStatusAcknowledge | kStatusDriver);

    cfgWrite32(kComDeviceFeatureSel, 0);
    f0 = cfgRead32(kComDeviceFeature);
    cfgWrite32(kComDeviceFeatureSel, 1);
    f1 = cfgRead32(kComDeviceFeature);
    if (!(f1 & kFeatureVersion1)) {
        IOLog("VirtioVsock: the device does not offer VIRTIO_F_VERSION_1\n");
        cfgWrite8(kComStatus, kStatusFailed);
        return false;
    }
    fFeatures = f0 & kFeatureStream;
    cfgWrite32(kComDriverFeatureSel, 0);
    cfgWrite32(kComDriverFeature, fFeatures);
    cfgWrite32(kComDriverFeatureSel, 1);
    cfgWrite32(kComDriverFeature, kFeatureVersion1);
    cfgWrite8(kComStatus, kStatusAcknowledge | kStatusDriver | kStatusFeaturesOK);
    if (!(cfgRead8(kComStatus) & kStatusFeaturesOK)) {
        IOLog("VirtioVsock: the device did not accept the features\n");
        cfgWrite8(kComStatus, kStatusFailed);
        return false;
    }

    fCidLo = regRead(&fDevice, 0, 4);
    fCidHi = regRead(&fDevice, 4, 4);

    if (!queueAlloc(&fRx, 0, kRxSlotsMax) || !queueAlloc(&fTx, 1, kTxSlotsMax) ||
        !queueAlloc(&fEv, 2, kEvSlots)) {
        cfgWrite8(kComStatus, kStatusFailed);
        return false;
    }
    fTxFree = (UInt16 *) IOMalloc(fTx.slots * sizeof(UInt16));
    if (!fTxFree)
        return false;
    return true;
}

/* Gives the device its queues and every receive and event slot, and lets it run. */
bool io_virtio_VirtioVsock::deviceStart()
{
    UInt16 i;

    queueSetup(&fRx);
    queueSetup(&fTx);
    queueSetup(&fEv);
    cfgWrite8(kComStatus, kStatusAcknowledge | kStatusDriver | kStatusFeaturesOK | kStatusDriverOK);

    for (i = 0; i < fRx.slots; i++)
        queuePublish(&fRx, i);
    queueNotify(&fRx);
    for (i = 0; i < fEv.slots; i++)
        queuePublish(&fEv, i);
    queueNotify(&fEv);

    for (i = 0; i < fTx.slots; i++)
        fTxFree[i] = fTx.slots - 1 - i;
    fTxFreeCount = fTx.slots;
    return true;
}

/* ------------------------------------------------------------------------------------------ */
/* connections                                                                                 */
/* ------------------------------------------------------------------------------------------ */

VsockConn *io_virtio_VirtioVsock::allocConn()
{
    int i;

    for (i = 0; i < kMaxConns; i++) {
        VsockConn *c = &fConns[i];
        if (c->state == kConnFree) {
            bzero(c, sizeof(*c));
            return c;
        }
    }
    return 0;
}

void io_virtio_VirtioVsock::freeConn(VsockConn *c)
{
    if (c->rxBuf)
        IOFree(c->rxBuf, kConnBuf);
    bzero(c, sizeof(*c));
}

VsockConn *io_virtio_VirtioVsock::findConn(UInt32 hi, UInt32 lo, UInt32 peerPort, UInt32 localPort)
{
    int i;

    for (i = 0; i < kMaxConns; i++) {
        VsockConn *c = &fConns[i];
        if (c->state != kConnFree && c->state != kConnListen &&
            c->localPort == localPort && c->peerPort == peerPort &&
            c->peerCidHi == hi && c->peerCidLo == lo)
            return c;
    }
    return 0;
}

VsockConn *io_virtio_VirtioVsock::findListener(UInt32 port)
{
    int i;

    for (i = 0; i < kMaxConns; i++)
        if (fConns[i].state == kConnListen && fConns[i].localPort == port)
            return &fConns[i];
    return 0;
}

VsockConn *io_virtio_VirtioVsock::handleToConn(void *client, UInt32 handle)
{
    VsockConn *c;

    if (handle >= kMaxConns)
        return 0;
    c = &fConns[handle];
    if (c->state == kConnFree || c->owner != client)
        return 0;
    return c;
}

UInt32 io_virtio_VirtioVsock::writeCredit(VsockConn *c)
{
    UInt32 inflight = c->txCnt - c->peerFwdCnt;

    return inflight >= c->peerBufAlloc ? 0 : c->peerBufAlloc - inflight;
}

/* The device was reset under us: whatever was connected is not. */
void io_virtio_VirtioVsock::resetAll()
{
    int i;

    for (i = 0; i < kMaxConns; i++) {
        VsockConn *c = &fConns[i];
        if (c->state == kConnConnecting || c->state == kConnConnected) {
            c->state = kConnReset;
            c->pending = 0;
            c->shutdownPeer = 0;    /* an error, not an end of stream */
        } else if (c->state == kConnClosing) {
            freeConn(c);
        }
    }
}

/* ------------------------------------------------------------------------------------------ */
/* packets                                                                                     */
/* ------------------------------------------------------------------------------------------ */

/* Puts one packet on the transmit queue. Data may not take the slots kept back for control
 * packets. False when there is no slot. */
bool io_virtio_VirtioVsock::sendPacket(UInt32 dstHi, UInt32 dstLo, UInt32 srcPort, UInt32 dstPort,
                                     UInt16 op, UInt32 flags, UInt32 fwdCnt, const UInt8 *data,
                                     UInt32 len, bool control)
{
    UInt16 slot;
    UInt8 *p;

    serviceTx();
    if (fTxFreeCount == 0 || (!control && fTxFreeCount <= kTxReserve))
        return false;
    slot = fTxFree[--fTxFreeCount];
    p = slotAddress(&fTx, slot);
    put32(p + kHdrSrcCid, fCidLo);      put32(p + kHdrSrcCid + 4, fCidHi);
    put32(p + kHdrDstCid, dstLo);       put32(p + kHdrDstCid + 4, dstHi);
    put32(p + kHdrSrcPort, srcPort);
    put32(p + kHdrDstPort, dstPort);
    put32(p + kHdrLen, len);
    put16(p + kHdrType, kTypeStream);
    put16(p + kHdrOp, op);
    put32(p + kHdrFlags, flags);
    put32(p + kHdrBufAlloc, kConnBuf);
    put32(p + kHdrFwdCnt, fwdCnt);
    if (len)
        bcopy(data, p + kHdrSize, len);
    put32(fTx.mem + 16 * slot + 8, kHdrSize + len);
    queuePublish(&fTx, slot);
    queueNotify(&fTx);
    return true;
}

bool io_virtio_VirtioVsock::sendConn(VsockConn *c, UInt16 op, UInt32 flags, const UInt8 *data, UInt32 len,
                                   bool control)
{
    if (!sendPacket(c->peerCidHi, c->peerCidLo, c->localPort, c->peerPort, op, flags, c->fwdCnt,
                    data, len, control))
        return false;
    c->fwdSent = c->fwdCnt;
    return true;
}

/* Sends what connections owe the peer. A packet that finds no slot is tried again when one comes
 * back (serviceTx's caller runs this). */
void io_virtio_VirtioVsock::flushPending()
{
    int i;

    for (i = 0; i < kMaxConns; i++) {
        VsockConn *c = &fConns[i];

        if (!c->pending || c->state == kConnFree)
            continue;
        if ((c->pending & kPendRequest) && sendConn(c, kOpRequest, 0, 0, 0, true))
            c->pending &= ~kPendRequest;
        if ((c->pending & kPendResponse) && sendConn(c, kOpResponse, 0, 0, 0, true))
            c->pending &= ~kPendResponse;
        if ((c->pending & kPendCredit) && sendConn(c, kOpCreditUpdate, 0, 0, 0, true))
            c->pending &= ~kPendCredit;
        if ((c->pending & kPendCreditReq) && sendConn(c, kOpCreditRequest, 0, 0, 0, true))
            c->pending &= ~kPendCreditReq;
        if ((c->pending & kPendShutdown) && sendConn(c, kOpShutdown, c->shutdownMine, 0, 0, true))
            c->pending &= ~kPendShutdown;
        if ((c->pending & kPendRst) && !(c->pending & ~kPendRst) && sendConn(c, kOpRst, 0, 0, 0, true)) {
            c->pending &= ~kPendRst;
            if (c->state == kConnClosing)
                freeConn(c);
        }
    }
}

/* One received packet, header and payload in a slot. */
void io_virtio_VirtioVsock::receive(UInt8 *p, UInt32 total)
{
    UInt32 srcHi, srcLo, dstHi, dstLo, srcPort, dstPort, len, flags, bufAlloc, fwd;
    UInt16 type, op;
    VsockConn *c;

    if (total < kHdrSize)
        return;
    srcLo = get32(p + kHdrSrcCid);   srcHi = get32(p + kHdrSrcCid + 4);
    dstLo = get32(p + kHdrDstCid);   dstHi = get32(p + kHdrDstCid + 4);
    srcPort = get32(p + kHdrSrcPort);
    dstPort = get32(p + kHdrDstPort);
    len = get32(p + kHdrLen);
    type = get16(p + kHdrType);
    op = get16(p + kHdrOp);
    flags = get32(p + kHdrFlags);
    bufAlloc = get32(p + kHdrBufAlloc);
    fwd = get32(p + kHdrFwdCnt);

    if (len > total - kHdrSize || dstHi != fCidHi || dstLo != fCidLo)
        return;
    if (type != kTypeStream) {
        if (op != kOpRst)
            sendPacket(srcHi, srcLo, dstPort, srcPort, kOpRst, 0, 0, 0, 0, true);
        return;
    }

    c = findConn(srcHi, srcLo, srcPort, dstPort);
    if (!c) {
        VsockConn *l;

        if (op == kOpRequest && (l = findListener(dstPort)) && l->acceptCount < kAcceptMax) {
            VsockConn *n = allocConn();

            if (n) {
                n->rxBuf = (UInt8 *) IOMalloc(kConnBuf);
                if (n->rxBuf) {
                    n->state = kConnConnected;
                    n->pending = kPendResponse;
                    n->localPort = dstPort;
                    n->peerPort = srcPort;
                    n->peerCidHi = srcHi;
                    n->peerCidLo = srcLo;
                    n->peerBufAlloc = bufAlloc;
                    n->peerFwdCnt = fwd;
                    n->owner = l->owner;
                    n->listener = (l - fConns) + 1;
                    l->acceptq[l->acceptCount++] = (UInt16) (n - fConns);
                    return;
                }
                bzero(n, sizeof(*n));
            }
        }
        if (op != kOpRst)
            sendPacket(srcHi, srcLo, dstPort, srcPort, kOpRst, 0, 0, 0, 0, true);
        return;
    }

    c->peerBufAlloc = bufAlloc;
    c->peerFwdCnt = fwd;
    switch (op) {
    case kOpRequest:
        c->pending |= kPendRst;
        break;
    case kOpResponse:
        if (c->state == kConnConnecting) {
            c->state = kConnConnected;
        } else
            c->pending |= kPendRst;
        break;
    case kOpRst:
        if (c->state == kConnConnecting)
            c->refused = 1;
        if (c->state == kConnClosing) {
            freeConn(c);
        } else {
            c->state = kConnReset;
            c->pending = 0;
            if (!c->shutdownPeer)
                c->shutdownPeer = 0;
        }
        break;
    case kOpShutdown:
        c->shutdownPeer |= (UInt8) flags;
        if (c->shutdownPeer == (kShutdownRcv | kShutdownSend)) {
            /* The peer is done with it entirely and expects the reset that completes the close.
             * What it sent before stays readable. */
            if (c->state == kConnClosing) {
                c->pending |= kPendRst;
            } else if (c->state == kConnConnected) {
                sendConn(c, kOpRst, 0, 0, 0, true);
                c->state = kConnReset;
                c->pending = 0;
            }
        }
        break;
    case kOpRW:
        if (c->state == kConnConnected || c->state == kConnReset) {
            UInt32 room = kConnBuf - c->rxLen, n = len < room ? len : room, at, first;

            if (n) {
                at = (c->rxHead + c->rxLen) % kConnBuf;
                first = kConnBuf - at;
                if (first > n)
                    first = n;
                bcopy(p + kHdrSize, c->rxBuf + at, first);
                if (n > first)
                    bcopy(p + kHdrSize + first, c->rxBuf, n - first);
                c->rxLen += n;
                c->recvCnt += n;
            }
        }
        break;
    case kOpCreditUpdate:
        break;
    case kOpCreditRequest:
        c->pending |= kPendCredit;
        break;
    default:
        break;
    }
}

/* ------------------------------------------------------------------------------------------ */
/* the work                                                                                    */
/* ------------------------------------------------------------------------------------------ */

void io_virtio_VirtioVsock::serviceTx()
{
    volatile UInt8 *used = fTx.mem + fTx.usedOff;
    while (fTx.lastUsed != get16(used + 2)) {
        UInt32 id;

        barrier();
        id = get32(used + 4 + 8 * (fTx.lastUsed % fTx.num));
        fTx.lastUsed++;
        if (id >= fTx.slots || fTxFreeCount >= fTx.slots) {
            IOLog("VirtioVsock: the device returned descriptor %d, which is no transmit slot in use\n", (int) id);
            continue;
        }
        fTxFree[fTxFreeCount++] = (UInt16) id;
    }
}

void io_virtio_VirtioVsock::serviceRx()
{
    volatile UInt8 *used = fRx.mem + fRx.usedOff;
    bool returned = false;

    while (fRx.lastUsed != get16(used + 2)) {
        UInt32 id, len;

        barrier();
        id = get32(used + 4 + 8 * (fRx.lastUsed % fRx.num));
        len = get32(used + 8 + 8 * (fRx.lastUsed % fRx.num));
        fRx.lastUsed++;
        if (id >= fRx.slots) {
            IOLog("VirtioVsock: the device returned descriptor %d, which is no receive slot\n", (int) id);
            continue;
        }
        receive(slotAddress(&fRx, (UInt16) id), len > kSlotSize ? kSlotSize : len);
        queuePublish(&fRx, (UInt16) id);
        returned = true;
    }
    if (returned)
        queueNotify(&fRx);
}

void io_virtio_VirtioVsock::serviceEvents()
{
    volatile UInt8 *used = fEv.mem + fEv.usedOff;
    bool returned = false;

    while (fEv.lastUsed != get16(used + 2)) {
        UInt32 id;

        barrier();
        id = get32(used + 4 + 8 * (fEv.lastUsed % fEv.num));
        fEv.lastUsed++;
        if (id >= fEv.slots)
            continue;
        if (get32(slotAddress(&fEv, (UInt16) id)) == kEventTransportReset) {
            IOLog("VirtioVsock: transport reset\n");
            fCidLo = regRead(&fDevice, 0, 4);
            fCidHi = regRead(&fDevice, 4, 4);
            resetAll();
        }
        bzero(slotAddress(&fEv, (UInt16) id), 16);
        queuePublish(&fEv, (UInt16) id);
        returned = true;
    }
    if (returned)
        queueNotify(&fEv);
}

void io_virtio_VirtioVsock::service()
{
    if (!fRunning)
        return;
    serviceTx();
    serviceRx();
    serviceEvents();
    flushPending();
}

bool io_virtio_VirtioVsock::interruptFilter(OSObject *owner, IOFilterInterruptEventSource *)
{
    io_virtio_VirtioVsock *me = (io_virtio_VirtioVsock *) owner;
    UInt8 isr = (UInt8) me->regRead(&me->fISRReg, 0, 1);

    if (!isr)
        return false;
    OSBitOrAtomic(isr, (UInt32 *) &me->fISR);
    OSIncrementAtomic((SInt32 *) &me->fInterrupts);
    return true;
}

void io_virtio_VirtioVsock::interruptAction(OSObject *owner, IOInterruptEventSource *, int)
{
    ((io_virtio_VirtioVsock *) owner)->interruptOccurred();
}

void io_virtio_VirtioVsock::interruptOccurred()
{
    UInt32 isr = 0;

    do {
        isr = fISR;
    } while (!OSCompareAndSwap(isr, 0, (UInt32 *) &fISR));
    if (!fRunning)
        return;
    if (isr & kISRConfig) {
        fCidLo = regRead(&fDevice, 0, 4);
        fCidHi = regRead(&fDevice, 4, 4);
    }
    service();
}

void io_virtio_VirtioVsock::timeoutAction(OSObject *owner, IOTimerEventSource *)
{
    ((io_virtio_VirtioVsock *) owner)->timeoutOccurred();
}

/* Every 100 ms: the connection deadlines, and what an interrupt may have missed. Polled, every
 * 10 ms does the work. If work keeps waiting that no interrupt announced -- the platform did not
 * route the line -- the driver polls from then on. */
void io_virtio_VirtioVsock::timeoutOccurred()
{
    static UInt32 acc = 0;
    int i;

    if (!fRunning)
        return;
    if (!fPolled && fRx.lastUsed != get16(fRx.mem + fRx.usedOff + 2) && fInterrupts == fInterruptsSeen) {
        if (++fQuietTicks >= kQuietTicksMax) {
            IOLog("VirtioVsock: packets raise no interrupt; polling every %d ms instead\n", kPollMS);
            fInterrupt->disable();
            fPolled = true;
            put16(fRx.mem + fRx.availOff, kAvailNoInterrupt);
            put16(fEv.mem + fEv.availOff, kAvailNoInterrupt);
        }
    } else
        fQuietTicks = 0;
    fInterruptsSeen = fInterrupts;
    service();

    acc += fPolled ? kPollMS : kWatchdogMS;
    if (acc >= kWatchdogMS) {
        acc = 0;
        for (i = 0; i < kMaxConns; i++) {
            VsockConn *c = &fConns[i];

            if (!c->ticks || c->state == kConnFree)
                continue;
            if (--c->ticks)
                continue;
            if (c->state == kConnClosing) {
                /* the peer never answered the close: reset it ourselves */
                sendConn(c, kOpRst, 0, 0, 0, true);
                freeConn(c);
            }
        }
    }
    fTimer->setTimeoutMS(fPolled ? kPollMS : kWatchdogMS);
}

/* ------------------------------------------------------------------------------------------ */
/* the commands, on the work loop                                                              */
/* ------------------------------------------------------------------------------------------ */

enum {
    kCmdListen, kCmdAccept, kCmdConnect, kCmdRead, kCmdWrite, kCmdShutdown, kCmdClose, kCmdInfo, kCmdPoll,
    kCmdCloseAll
};

IOReturn io_virtio_VirtioVsock::commandAction(OSObject *owner, void *cmd, void *, void *, void *)
{
    return ((io_virtio_VirtioVsock *) owner)->runCommand(cmd);
}

IOReturn io_virtio_VirtioVsock::command(int op, void *client, UInt32 a0, UInt32 a1, UInt32 a2, UInt32 a3,
                                      UInt8 *data, UInt32 dataLen, UInt32 *out)
{
    VsockCmd cmd;
    UInt32 i;

    cmd.op = op;
    cmd.client = client;
    cmd.a[0] = a0; cmd.a[1] = a1; cmd.a[2] = a2; cmd.a[3] = a3;
    cmd.data = data;
    cmd.dataLen = dataLen;
    for (i = 0; i < 4; i++)
        cmd.out[i] = 0;
    cmd.result = kIOReturnOffline;
    if (fDead || !fGate)
        return kIOReturnOffline;
    fGate->runAction(commandAction, &cmd);
    if (out)
        for (i = 0; i < 4; i++)
            out[i] = cmd.out[i];
    return cmd.result;
}

/* Releases our direction of a connection on the user's request. */
void io_virtio_VirtioVsock::shutdownRelease(VsockConn *c, UInt8 how)
{
    c->shutdownMine |= how;
    c->pending |= kPendShutdown;
}

IOReturn io_virtio_VirtioVsock::runCommand(void *arg)
{
    VsockCmd *cmd = (VsockCmd *) arg;
    VsockConn *c;
    IOReturn r = kIOReturnSuccess;
    UInt32 i;

    if (!fRunning) {
        cmd->result = kIOReturnOffline;
        return cmd->result;
    }

    switch (cmd->op) {

    case kCmdListen: {
        UInt32 port = cmd->a[0];

        if (!port) { r = kIOReturnBadArgument; break; }
        if (findListener(port)) { r = kIOReturnExclusiveAccess; break; }
        c = allocConn();
        if (!c) { r = kIOReturnNoResources; break; }
        c->state = kConnListen;
        c->localPort = port;
        c->owner = cmd->client;
        cmd->out[0] = c - fConns;
        break;
    }

    case kCmdAccept: {
        c = handleToConn(cmd->client, cmd->a[0]);
        if (!c || c->state != kConnListen) { r = kIOReturnNotOpen; break; }
        if (!c->acceptCount) { r = kIOReturnNotReady; break; }
        {
            VsockConn *n = &fConns[c->acceptq[0]];

            for (i = 1; i < c->acceptCount; i++)
                c->acceptq[i - 1] = c->acceptq[i];
            c->acceptCount--;
            n->listener = 0;
            cmd->out[0] = n - fConns;
            cmd->out[1] = n->peerCidHi;
            cmd->out[2] = n->peerCidLo;
            cmd->out[3] = n->peerPort;
        }
        break;
    }

    /* Starts a connection: the REQUEST goes out as the queue allows, and the caller polls for
     * VSOCK_P_CONNECTED, or VSOCK_P_RESET for a refusal. */
    case kCmdConnect: {
        UInt32 port = cmd->a[2];

        if (!port) { r = kIOReturnBadArgument; break; }
        c = allocConn();
        if (!c) { r = kIOReturnNoResources; break; }
        c->rxBuf = (UInt8 *) IOMalloc(kConnBuf);
        if (!c->rxBuf) { bzero(c, sizeof(*c)); r = kIOReturnNoMemory; break; }
        for (;;) {
            bool taken = false;

            if (++fNextPort < kPortFirst)
                fNextPort = kPortFirst;
            for (i = 0; i < kMaxConns; i++)
                if (&fConns[i] != c && fConns[i].state != kConnFree && fConns[i].localPort == fNextPort)
                    taken = true;
            if (!taken)
                break;
        }
        c->localPort = fNextPort;
        c->peerCidHi = cmd->a[0];
        c->peerCidLo = cmd->a[1];
        c->peerPort = port;
        c->owner = cmd->client;
        c->state = kConnConnecting;
        c->pending = kPendRequest;
        cmd->out[0] = c - fConns;
        flushPending();
        break;
    }

    case kCmdRead: {
        UInt32 max = cmd->a[1];

        if (max > cmd->dataLen)
            max = cmd->dataLen;
        c = handleToConn(cmd->client, cmd->a[0]);
        if (!c || c->state == kConnListen || c->state == kConnConnecting) { r = kIOReturnNotOpen; break; }
        if (c->rxLen) {
            UInt32 n = c->rxLen < max ? c->rxLen : max, first = kConnBuf - c->rxHead;

            if (first > n)
                first = n;
            bcopy(c->rxBuf + c->rxHead, cmd->data, first);
            if (n > first)
                bcopy(c->rxBuf, cmd->data + first, n - first);
            /* Tell a peer that has used up its credit that there is room again, before it must
             * ask. */
            if (c->rxLen >= kConnBuf * 3 / 4 || c->fwdCnt + n - c->fwdSent >= kConnBuf / 4)
                c->pending |= kPendCredit;
            c->rxHead = (c->rxHead + n) % kConnBuf;
            c->rxLen -= n;
            c->fwdCnt += n;
            cmd->out[0] = n;
            flushPending();
        } else if (c->shutdownPeer & kShutdownSend) {
            cmd->out[0] = 0;                /* the end of the stream */
        } else if (c->state == kConnReset) {
            r = kIOReturnIOError;
        } else {
            r = kIOReturnNotReady;
        }
        break;
    }

    /* As much as the peer's credit and the transmit slots allow, without waiting: NotReady if
     * none. The caller loops. */
    case kCmdWrite: {
        UInt32 sent = 0, len = cmd->dataLen;

        c = handleToConn(cmd->client, cmd->a[0]);
        if (!c || c->state == kConnListen || c->state == kConnConnecting) { r = kIOReturnNotOpen; break; }
        if (c->state != kConnConnected || (c->shutdownPeer & kShutdownRcv) ||
            (c->shutdownMine & kShutdownSend)) {
            r = c->state == kConnReset && !c->shutdownPeer ? kIOReturnIOError : kIOReturnNotWritable;
            break;
        }
        while (sent < len) {
            UInt32 chunk = len - sent, credit = writeCredit(c);

            if (chunk > credit)
                chunk = credit;
            if (chunk > kPayloadMax)
                chunk = kPayloadMax;
            if (!chunk) {
                c->pending |= kPendCreditReq;
                flushPending();
                break;
            }
            if (!sendConn(c, kOpRW, 0, cmd->data + sent, chunk, false))
                break;
            c->txCnt += chunk;
            sent += chunk;
        }
        cmd->out[0] = sent;
        if (!sent && len)
            r = kIOReturnNotReady;
        break;
    }

    case kCmdShutdown: {
        UInt32 how = cmd->a[1] & 3;

        c = handleToConn(cmd->client, cmd->a[0]);
        if (!c || c->state == kConnListen || c->state == kConnConnecting) { r = kIOReturnNotOpen; break; }
        if (c->state == kConnConnected && how) {
            /* "receive" here is the user's: the peer is told it may not send; the wire's flags
             * name the shutdown as the sender of the packet sees it */
            UInt8 flags = 0;

            if (how & 1) flags |= kShutdownRcv;
            if (how & 2) flags |= kShutdownSend;
            shutdownRelease(c, flags);
            flushPending();
        }
        break;
    }

    case kCmdClose: {
        c = handleToConn(cmd->client, cmd->a[0]);
        if (!c) { r = kIOReturnNotOpen; break; }
        closeFor(c);
        break;
    }

    case kCmdCloseAll:
        for (i = 0; i < kMaxConns; i++)
            if (fConns[i].state != kConnFree && fConns[i].owner == cmd->client)
                closeFor(&fConns[i]);
        break;

    case kCmdInfo:
        cmd->out[0] = fCidHi;
        cmd->out[1] = fCidLo;
        for (i = 0; i < kMaxConns; i++)
            if (fConns[i].state != kConnFree && fConns[i].owner == cmd->client)
                cmd->out[2]++;
        break;

    case kCmdPoll: {
        UInt32 flags = 0, credit = 0;

        c = handleToConn(cmd->client, cmd->a[0]);
        if (!c) { r = kIOReturnNotOpen; break; }
        if (c->state == kConnListen) {
            if (c->acceptCount)
                flags |= VSOCK_P_ACCEPT;
        } else {
            if (c->state == kConnConnected)
                flags |= VSOCK_P_CONNECTED;
            if (c->shutdownPeer & kShutdownSend)
                flags |= VSOCK_P_EOF;
            if (c->state == kConnReset)
                flags |= VSOCK_P_RESET;
            if (c->shutdownPeer & kShutdownRcv)
                flags |= VSOCK_P_NOSEND;
            if (c->state == kConnConnected)
                credit = writeCredit(c);
            cmd->out[0] = c->rxLen;
        }
        cmd->out[1] = credit;
        cmd->out[2] = flags;
        break;
    }

    default:
        r = kIOReturnUnsupported;
        break;
    }
    cmd->result = r;
    return r;
}

/* The user closes a socket: the handle is gone at once; what the peer must be told goes out as
 * the transmit queue allows. */
void io_virtio_VirtioVsock::closeFor(VsockConn *c)
{
    UInt32 i;

    switch (c->state) {
    case kConnListen:
        /* connections nobody accepted */
        for (i = 0; i < c->acceptCount; i++) {
            VsockConn *n = &fConns[c->acceptq[i]];

            n->listener = 0;
            n->owner = 0;
            n->state = kConnClosing;
            n->pending = kPendRst;
            n->ticks = kClosingTicks;
        }
        freeConn(c);
        flushPending();
        break;
    case kConnConnected:
        c->owner = 0;
        c->state = kConnClosing;
        c->shutdownMine = kShutdownRcv | kShutdownSend;
        c->pending = kPendShutdown;
        c->ticks = kClosingTicks;
        c->rxLen = 0;
        flushPending();
        break;
    case kConnConnecting:
        c->owner = 0;
        c->state = kConnClosing;
        c->pending = kPendRst;
        c->ticks = kClosingTicks;
        flushPending();
        break;
    default:
        freeConn(c);
        break;
    }
}

/* The user client has gone: every socket it had is closed. */
void io_virtio_VirtioVsock::clientGone(void *client)
{
    VsockCmd cmd;

    if (fDead || !fGate)
        return;
    bzero(&cmd, sizeof(cmd));
    cmd.op = kCmdCloseAll;
    cmd.client = client;
    fGate->runAction(commandAction, &cmd);
}

/* ------------------------------------------------------------------------------------------ */
/* the service                                                                                 */
/* ------------------------------------------------------------------------------------------ */

bool io_virtio_VirtioVsock::start(IOService *provider)
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

#if defined(__i386__) || defined(__x86_64__)
    fWindowLock = IOSimpleLockAlloc();
    if (!fWindowLock)
        goto fail;
#endif
    if (!deviceProbe())
        goto fail;
    /* Registers reached through the configuration window take several accesses each, which the
     * interrupt filter reading the ISR would interleave with the work loop's. On Intel each access
     * is made whole under fWindowLock with interrupts off, so the filter reads the ISR through the
     * window like any other register, and the device is interrupt-driven: until 0.2.3 it was polled
     * every 10 ms wherever its BAR was not mapped, which was 10.4, 10.5 and 10.8 to 10.11 (the
     * gauntlet on 69b7e43, 2026-10-13). PowerPC's BAR is always mapped; it keeps the old rule. */
#if !(defined(__i386__) || defined(__x86_64__))
    if (!fISRReg.virt && !fPolled) {
        IOLog("VirtioVsock: the registers are reached through the PCI configuration window; polling every %d ms\n", kPollMS);
        fPolled = true;
    }
#endif

    fTimer = IOTimerEventSource::timerEventSource(this, timeoutAction);
    if (!fTimer || fWorkLoop->addEventSource(fTimer) != kIOReturnSuccess)
        goto fail;
    if (!fPolled) {
        fInterrupt = IOFilterInterruptEventSource::filterInterruptEventSource(this, interruptAction,
                                                                              interruptFilter, provider, 0);
        if (!fInterrupt || fWorkLoop->addEventSource(fInterrupt) != kIOReturnSuccess) {
            IOLog("VirtioVsock: the device's interrupt cannot be had; polling every %d ms instead\n", kPollMS);
            if (fInterrupt)
                fInterrupt->release();
            fInterrupt = 0;
            fPolled = true;
        }
    }

    fNextPort = kPortFirst + (fCidLo & 0xFF);
    deviceStart();
    fRunning = true;
    if (fInterrupt)
        fInterrupt->enable();
    fTimer->setTimeoutMS(fPolled ? kPollMS : kWatchdogMS);

    IOLog("VirtioVsock: cid %u, %d receive and %d transmit slots, %s%s\n", (unsigned int) fCidLo,
          fRx.slots, fTx.slots, fPolled ? "polled" : "interrupt-driven",
          fISRReg.virt ? "" : ", through the configuration window");

    registerService();
    return true;

fail:
    IOLog("VirtioVsock: could not start\n");
    stop(provider);
    return false;
}

void io_virtio_VirtioVsock::stop(IOService *provider)
{
    fDead = true;
    fRunning = false;
    if (fTimer)
        fTimer->cancelTimeout();
    if (fInterrupt)
        fInterrupt->disable();
    if (fPCI && fCommon.bar < 6 && (fCommon.virt || fCfgWindowOff))
        deviceReset();
    if (fPCI && fPCI->isOpen(this))
        fPCI->close(this);
    super::stop(provider);
}

void io_virtio_VirtioVsock::free()
{
    int i;

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
    for (i = 0; i < kMaxConns; i++)
        if (fConns[i].rxBuf)
            IOFree(fConns[i].rxBuf, kConnBuf);
    queueFree(&fRx);
    queueFree(&fTx);
    queueFree(&fEv);
    if (fTxFree)
        IOFree(fTxFree, fTx.slots * sizeof(UInt16));
    for (i = 0; i < 6; i++)
        if (fMaps[i])
            fMaps[i]->release();
    if (fWindowLock)
        IOSimpleLockFree(fWindowLock);
    if (fPCI)
        fPCI->release();
    if (fWorkLoop)
        fWorkLoop->release();
    super::free();
}

IOWorkLoop *io_virtio_VirtioVsock::getWorkLoop() const
{
    return fWorkLoop;
}

/* ------------------------------------------------------------------------------------------ */
/* the user client                                                                             */
/* ------------------------------------------------------------------------------------------ */

#undef super
#define super IOUserClient
OSDefineMetaClassAndStructors(io_virtio_VirtioVsockClient, IOUserClient)

bool io_virtio_VirtioVsockClient::initWithTask(task_t owningTask, void *securityID, UInt32 type)
{
    if (!super::init())
        return false;
    fDriver = 0;
    return true;
}

bool io_virtio_VirtioVsockClient::start(IOService *provider)
{
    static const IOExternalMethod methods[kVsockMethodCount] = {
        /* kVsockListen   */ { 0, (IOMethod) &io_virtio_VirtioVsockClient::listen,   kIOUCScalarIScalarO, 2, 1 },
        /* kVsockAccept   */ { 0, (IOMethod) &io_virtio_VirtioVsockClient::accept,   kIOUCScalarIScalarO, 2, 4 },
        /* kVsockConnect  */ { 0, (IOMethod) &io_virtio_VirtioVsockClient::connect,  kIOUCScalarIScalarO, 4, 1 },
        /* kVsockRead     */ { 0, (IOMethod) &io_virtio_VirtioVsockClient::read,     kIOUCStructIStructO, sizeof(VsockReadIn), sizeof(VsockReadOut) },
        /* kVsockWrite    */ { 0, (IOMethod) &io_virtio_VirtioVsockClient::write,    kIOUCStructIStructO, sizeof(VsockWriteIn), sizeof(VsockWriteOut) },
        /* kVsockShutdown */ { 0, (IOMethod) &io_virtio_VirtioVsockClient::shutdown, kIOUCScalarIScalarO, 2, 0 },
        /* kVsockClose    */ { 0, (IOMethod) &io_virtio_VirtioVsockClient::close,    kIOUCScalarIScalarO, 1, 0 },
        /* kVsockInfo     */ { 0, (IOMethod) &io_virtio_VirtioVsockClient::info,     kIOUCScalarIScalarO, 0, 3 },
        /* kVsockPoll     */ { 0, (IOMethod) &io_virtio_VirtioVsockClient::poll,     kIOUCScalarIScalarO, 1, 3 }
    };
    int i;

    fDriver = OSDynamicCast(io_virtio_VirtioVsock, provider);
    if (!fDriver)
        return false;
    if (!super::start(provider))
        return false;
    for (i = 0; i < kVsockMethodCount; i++) {
        fMethods[i] = methods[i];
        fMethods[i].object = this;
    }
    return true;
}

IOReturn io_virtio_VirtioVsockClient::clientClose()
{
    if (fDriver) {
        io_virtio_VirtioVsock *d = fDriver;

        fDriver = 0;
        d->clientGone(this);
    }
    if (!isInactive())
        terminate();
    return kIOReturnSuccess;
}

IOReturn io_virtio_VirtioVsockClient::clientDied()
{
    return clientClose();
}

IOExternalMethod *io_virtio_VirtioVsockClient::getExternalMethodForIndex(UInt32 index)
{
    return index < kVsockMethodCount ? &fMethods[index] : 0;
}

IOReturn io_virtio_VirtioVsockClient::listen(UInt32 port, UInt32 backlog, UInt32 *handle)
{
    UInt32 out[4];
    IOReturn r;

    if (!fDriver)
        return kIOReturnNotAttached;
    r = fDriver->command(kCmdListen, this, port, backlog, 0, 0, 0, 0, out);
    *handle = out[0];
    return r;
}

IOReturn io_virtio_VirtioVsockClient::accept(UInt32 handle, UInt32 nonblock, UInt32 *newHandle, UInt32 *cidHi,
                                           UInt32 *cidLo, UInt32 *port)
{
    UInt32 out[4];
    IOReturn r;

    if (!fDriver)
        return kIOReturnNotAttached;
    r = fDriver->command(kCmdAccept, this, handle, nonblock, 0, 0, 0, 0, out);
    *newHandle = out[0]; *cidHi = out[1]; *cidLo = out[2]; *port = out[3];
    return r;
}

IOReturn io_virtio_VirtioVsockClient::connect(UInt32 cidHi, UInt32 cidLo, UInt32 port, UInt32 timeoutMS,
                                            UInt32 *handle)
{
    UInt32 out[4];
    IOReturn r;

    if (!fDriver)
        return kIOReturnNotAttached;
    r = fDriver->command(kCmdConnect, this, cidHi, cidLo, port, timeoutMS, 0, 0, out);
    *handle = out[0];
    return r;
}

IOReturn io_virtio_VirtioVsockClient::read(VsockReadIn *in, VsockReadOut *out, IOByteCount inSize,
                                         IOByteCount *outSize)
{
    UInt32 res[4];
    IOReturn r;

    if (!fDriver)
        return kIOReturnNotAttached;
    if (inSize != sizeof(VsockReadIn) || *outSize != sizeof(VsockReadOut))
        return kIOReturnBadArgument;
    r = fDriver->command(kCmdRead, this, in->handle, in->maxLength, in->nonblock, 0, out->data,
                         VSOCK_XFER_MAX, res);
    out->length = r == kIOReturnSuccess ? res[0] : 0;
    return r;
}

IOReturn io_virtio_VirtioVsockClient::write(VsockWriteIn *in, VsockWriteOut *out, IOByteCount inSize,
                                          IOByteCount *outSize)
{
    UInt32 res[4];
    IOReturn r;

    if (!fDriver)
        return kIOReturnNotAttached;
    if (inSize != sizeof(VsockWriteIn) || in->length > VSOCK_XFER_MAX)
        return kIOReturnBadArgument;
    r = fDriver->command(kCmdWrite, this, in->handle, in->nonblock, 0, 0, in->data, in->length, res);
    out->written = res[0];
    return r;
}

IOReturn io_virtio_VirtioVsockClient::shutdown(UInt32 handle, UInt32 how)
{
    if (!fDriver)
        return kIOReturnNotAttached;
    return fDriver->command(kCmdShutdown, this, handle, how, 0, 0, 0, 0, 0);
}

IOReturn io_virtio_VirtioVsockClient::close(UInt32 handle)
{
    if (!fDriver)
        return kIOReturnNotAttached;
    return fDriver->command(kCmdClose, this, handle, 0, 0, 0, 0, 0, 0);
}

IOReturn io_virtio_VirtioVsockClient::info(UInt32 *cidHi, UInt32 *cidLo, UInt32 *count)
{
    UInt32 out[4];
    IOReturn r;

    if (!fDriver)
        return kIOReturnNotAttached;
    r = fDriver->command(kCmdInfo, this, 0, 0, 0, 0, 0, 0, out);
    *cidHi = out[0]; *cidLo = out[1]; *count = out[2];
    return r;
}

IOReturn io_virtio_VirtioVsockClient::poll(UInt32 handle, UInt32 *readable, UInt32 *writable, UInt32 *flags)
{
    UInt32 out[4];
    IOReturn r;

    if (!fDriver)
        return kIOReturnNotAttached;
    r = fDriver->command(kCmdPoll, this, handle, 0, 0, 0, 0, 0, out);
    *readable = out[0]; *writable = out[1]; *flags = out[2];
    return r;
}
