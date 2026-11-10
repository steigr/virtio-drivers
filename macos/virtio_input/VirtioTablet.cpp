/*
 * VirtioTablet: see VirtioTablet.h for what this is and why it is written the way it is.
 *
 * The transport is VirtioVsock.cpp's (virtio 1.x over PCI, after elliotnunn/classicvirtio's
 * transport-ndrv.c), cut down to the one queue the device fills; the pointing side follows what
 * IOHIPointing's dispatch routines expect of a subclass (xnu-123 and IOHIDFamily-34/-86).
 */

#include "VirtioTablet.h"

#include <IOKit/IOLib.h>
#include <libkern/OSAtomic.h>

extern "C" {
#include <kern/clock.h>
}

#define super IOHIPointing
OSDefineMetaClassAndStructors(io_virtio_VirtioTablet, IOHIPointing)

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
    kFeatureVersion1   = 1,         /* bit 32, as bit 0 of the second word */
    kDescWrite         = 2,
    kAvailNoInterrupt  = 1
};

/* struct virtio_input_config, the device configuration */
enum {
    kCfgSelect         = 0,
    kCfgSubsel         = 1,
    kCfgSize           = 2,
    kCfgData           = 8,         /* the union: a string, a bitmap, an absinfo or the ids */
    kCfgAbsMin         = 8,
    kCfgAbsMax         = 12
};

enum {
    kSelIdName         = 0x01,
    kSelEvBits         = 0x11,
    kSelAbsInfo        = 0x12
};

/* Linux's input event codes, as the device uses them. */
enum {
    kEvSyn             = 0x00,
    kEvKey             = 0x01,
    kEvRel             = 0x02,
    kEvAbs             = 0x03,

    kSynReport         = 0,
    kSynDropped        = 3,

    kRelX              = 0x00,
    kRelY              = 0x01,
    kRelHWheel         = 0x06,
    kRelWheel          = 0x08,

    kAbsX              = 0x00,
    kAbsY              = 0x01,

    kBtnLeft           = 0x110,
    kBtnRight          = 0x111,
    kBtnMiddle         = 0x112,
    kBtnSide           = 0x113,
    kBtnExtra          = 0x114,
    kBtnTouch          = 0x14A,
    kBtnGearDown       = 0x150,     /* the wheel, as QEMU before 4.0 sent it */
    kBtnGearUp         = 0x151
};

enum {
    kCommandMemory     = 0x0002,
    kCommandBusMaster  = 0x0004,
    kConfigCommand     = 0x04,
    kConfigStatus      = 0x04,      /* the status is its upper half */
    kConfigCapPtr      = 0x34,
    kStatusCapList     = 0x10
};

enum {
    kPage              = 4096,
    kEventSize         = 8,
    kSlotsMax          = 128,       /* QEMU's eventq has 64 */
    kPollMS            = 10,
    kWatchdogMS        = 100,
    kQuietTicksMax     = 20,
    kReactivateFirstMS = 5000,      /* see reactivate(): 5, 10, 20, 40, 80 and 160 s after start */
    kReactivateMax     = 6,
    kButtons           = 3
};

static inline UInt32 roundPage(UInt32 n) { return (n + kPage - 1) & ~(UInt32)(kPage - 1); }

static inline void barrier() { __asm__ __volatile__ ("sync" : : : "memory"); }

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

/* Byte-reversing loads and stores, for little-endian registers in the BAR. */
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

/* ------------------------------------------------------------------------------------------ */
/* the transport, as VirtioVsock's                                                             */
/* ------------------------------------------------------------------------------------------ */

UInt32 io_virtio_VirtioTablet::regRead(TabletRegion *r, UInt32 off, int size)
{
    if (r->virt) {
        volatile UInt8 *p = r->virt + off;
        if (size == 4)
            return ld32le(p);
        if (size == 2)
            return ld16le(p);
        return *p;
    }
    if (!fCfgWindowOff)
        return 0;
    fPCI->configWrite32(fCfgWindowOff + 4, r->bar);
    fPCI->configWrite32(fCfgWindowOff + 8, r->offset + off);
    fPCI->configWrite32(fCfgWindowOff + 12, size);
    {
        UInt32 v = fPCI->configRead32(fCfgWindowOff + 16);
        return size == 4 ? v : (v & ((1UL << (8 * size)) - 1));
    }
}

void io_virtio_VirtioTablet::regWrite(TabletRegion *r, UInt32 off, int size, UInt32 value)
{
    if (r->virt) {
        volatile UInt8 *p = r->virt + off;
        if (size == 4)
            st32le(p, value);
        else if (size == 2)
            st16le(p, (UInt16) value);
        else {
            *p = (UInt8) value;
            __asm__ __volatile__ ("eieio" : : : "memory");
        }
        return;
    }
    if (!fCfgWindowOff)
        return;
    fPCI->configWrite32(fCfgWindowOff + 4, r->bar);
    fPCI->configWrite32(fCfgWindowOff + 8, r->offset + off);
    fPCI->configWrite32(fCfgWindowOff + 12, size);
    fPCI->configWrite32(fCfgWindowOff + 16, value);
}

UInt8  io_virtio_VirtioTablet::cfgRead8(UInt32 off)             { return (UInt8) regRead(&fCommon, off, 1); }
UInt16 io_virtio_VirtioTablet::cfgRead16(UInt32 off)            { return (UInt16) regRead(&fCommon, off, 2); }
void   io_virtio_VirtioTablet::cfgWrite8(UInt32 off, UInt8 v)   { regWrite(&fCommon, off, 1, v); }
void   io_virtio_VirtioTablet::cfgWrite16(UInt32 off, UInt16 v) { regWrite(&fCommon, off, 2, v); }
void   io_virtio_VirtioTablet::cfgWrite32(UInt32 off, UInt32 v) { regWrite(&fCommon, off, 4, v); }

bool io_virtio_VirtioTablet::mapBar(UInt8 bar)
{
    if (bar > 5)
        return false;
    if (!fMaps[bar])
        fMaps[bar] = fPCI->mapDeviceMemoryWithRegister(0x10 + 4 * bar);
    return fMaps[bar] != 0;
}

/* Walks the capability list for the structures and maps their BARs. Capabilities that name a BAR
 * that could not be mapped are reached through the PCI_CFG window, if the device has one. */
bool io_virtio_VirtioTablet::findCaps()
{
    UInt32 cap, guard = 0;
    bool haveCommon = false, haveNotify = false, haveISR = false, haveDevice = false;
    TabletRegion *all[4];
    int i;

    if (!((fPCI->configRead32(kConfigStatus) >> 16) & kStatusCapList))
        return false;
    cap = fPCI->configRead32(kConfigCapPtr) & 0xFC;
    while (cap && guard++ < 48) {
        UInt32 w0 = fPCI->configRead32(cap);
        UInt8 vndr = (UInt8) w0, next = (UInt8) (w0 >> 8), type = (UInt8) (w0 >> 24);

        if (vndr == 9) {
            TabletRegion r;
            r.bar = (UInt8) fPCI->configRead32(cap + 4);
            r.offset = fPCI->configRead32(cap + 8);
            r.length = fPCI->configRead32(cap + 12);
            r.virt = 0;
            if (type == kCapPCICfg)
                fCfgWindowOff = (UInt16) cap;
            else if (type == kCapCommon && !haveCommon)  { fCommon = r; haveCommon = true; }
            else if (type == kCapNotify && !haveNotify) {
                fNotify = r; haveNotify = true;
                fNotifyMult = fPCI->configRead32(cap + 16);
            }
            else if (type == kCapISR && !haveISR)        { fISRReg = r; haveISR = true; }
            else if (type == kCapDevice && !haveDevice)  { fDevice = r; haveDevice = true; }
        }
        cap = next & 0xFC;
    }
    if (!haveCommon || !haveNotify || !haveISR || !haveDevice)
        return false;

    all[0] = &fCommon; all[1] = &fNotify; all[2] = &fISRReg; all[3] = &fDevice;
    for (i = 0; i < 4; i++) {
        if (mapBar(all[i]->bar) && fMaps[all[i]->bar]->getLength() >= all[i]->offset + all[i]->length) {
            all[i]->virt = (UInt8 *) fMaps[all[i]->bar]->getVirtualAddress() + all[i]->offset;
        } else {
            IOLog("VirtioTablet: BAR %d is not mapped%s\n", all[i]->bar,
                  fCfgWindowOff ? "; using the PCI configuration window" : "");
            if (!fCfgWindowOff)
                return false;
        }
    }
    return true;
}

void io_virtio_VirtioTablet::deviceReset()
{
    int i;

    cfgWrite8(kComStatus, 0);
    for (i = 0; i < 1000 && cfgRead8(kComStatus) != 0; i++)
        IODelay(100);
}

void io_virtio_VirtioTablet::queuePublish(UInt16 slot)
{
    volatile UInt8 *avail = fRing + fAvailOff;

    put16(avail + 4 + 2 * (fNextAvail % fNum), slot);
    barrier();
    fNextAvail++;
    put16(avail + 2, fNextAvail);
}

void io_virtio_VirtioTablet::queueNotify()
{
    barrier();
    regWrite(&fNotify, (UInt32) fNotifyOff * fNotifyMult, 2, 0);
}

/* ------------------------------------------------------------------------------------------ */
/* the device's configuration                                                                  */
/* ------------------------------------------------------------------------------------------ */

/* Asks the device configuration for one thing; returns how many bytes of the answer there are. */
UInt8 io_virtio_VirtioTablet::inputQuery(UInt8 select, UInt8 subsel)
{
    regWrite(&fDevice, kCfgSelect, 1, select);
    regWrite(&fDevice, kCfgSubsel, 1, subsel);
    return (UInt8) regRead(&fDevice, kCfgSize, 1);
}

/* Whether the device sends events of this type and code. */
bool io_virtio_VirtioTablet::inputBit(UInt8 type, UInt16 code)
{
    UInt8 size = inputQuery(kSelEvBits, type);

    if (code / 8 >= size)
        return false;
    return (regRead(&fDevice, kCfgData + code / 8, 1) >> (code % 8)) & 1;
}

bool io_virtio_VirtioTablet::inputAxis(UInt8 axis, SInt32 *min, SInt32 *max)
{
    if (inputQuery(kSelAbsInfo, axis) < 8)
        return false;
    *min = (SInt32) regRead(&fDevice, kCfgAbsMin, 4);
    *max = (SInt32) regRead(&fDevice, kCfgAbsMax, 4);
    return *max > *min;
}

/* An absolute axis as IOHIDSystem takes it: from 0 to an extent that fits its 16-bit Bounds. A
 * range wider than that (QEMU's is 0..32767 and fits) loses its low bits. */
bool io_virtio_VirtioTablet::axisSetup(UInt8 axis, SInt32 *base, UInt8 *shift, SInt16 *extent)
{
    SInt32 min, max;
    UInt32 range;

    if (!inputAxis(axis, &min, &max))
        return false;
    range = (UInt32) (max - min);
    *base = min;
    *shift = 0;
    while ((range >> *shift) > 32767)
        (*shift)++;
    *extent = (SInt16) (range >> *shift);
    return true;
}

/* Resets the device and brings it up to FEATURES_OK: VIRTIO_F_VERSION_1 and nothing else. */
bool io_virtio_VirtioTablet::deviceNegotiate()
{
    UInt32 f1;

    deviceReset();
    cfgWrite8(kComStatus, kStatusAcknowledge);
    cfgWrite8(kComStatus, kStatusAcknowledge | kStatusDriver);

    cfgWrite32(kComDeviceFeatureSel, 1);
    f1 = regRead(&fCommon, kComDeviceFeature, 4);
    if (!(f1 & kFeatureVersion1)) {
        IOLog("VirtioTablet: the device does not offer VIRTIO_F_VERSION_1\n");
        return false;
    }
    cfgWrite32(kComDriverFeatureSel, 0);
    cfgWrite32(kComDriverFeature, 0);
    cfgWrite32(kComDriverFeatureSel, 1);
    cfgWrite32(kComDriverFeature, kFeatureVersion1);
    cfgWrite8(kComStatus, kStatusAcknowledge | kStatusDriver | kStatusFeaturesOK);
    if (!(cfgRead8(kComStatus) & kStatusFeaturesOK)) {
        IOLog("VirtioTablet: the device did not accept the features\n");
        return false;
    }
    return true;
}

/* Brings the device up to FEATURES_OK and finds out what it is: an absolute pointer (ABS_X and
 * ABS_Y with their ranges), a relative one (REL_X and REL_Y), or neither -- a keyboard, which is
 * declined. Allocates the event queue. */
bool io_virtio_VirtioTablet::deviceProbe()
{
    UInt32 cmd;
    char name[32];
    UInt8 n, i;

    cmd = fPCI->configRead16(kConfigCommand);
    fPCI->configWrite16(kConfigCommand, cmd | kCommandMemory | kCommandBusMaster);

    if (!findCaps()) {
        IOLog("VirtioTablet: the device has no usable modern virtio PCI capabilities\n");
        return false;
    }
    if (!deviceNegotiate())
        goto failed;

    n = inputQuery(kSelIdName, 0);
    if (n > sizeof(name) - 1)
        n = sizeof(name) - 1;
    for (i = 0; i < n; i++)
        name[i] = (char) regRead(&fDevice, kCfgData + i, 1);
    name[n] = 0;
    setProperty("DeviceName", name);

    if (inputBit(kEvAbs, kAbsX) && inputBit(kEvAbs, kAbsY) &&
        axisSetup(kAbsX, &fBaseX, &fShiftX, &fBounds.maxx) &&
        axisSetup(kAbsY, &fBaseY, &fShiftY, &fBounds.maxy)) {
        fAbsolute = true;
        fBounds.minx = 0;
        fBounds.miny = 0;
    } else if (inputBit(kEvRel, kRelX) && inputBit(kEvRel, kRelY)) {
        fAbsolute = false;
    } else {
        IOLog("VirtioTablet: \"%s\" is no pointing device; leaving it alone\n", name);
        goto failed;
    }
    IOLog("VirtioTablet: \"%s\", %s", name, fAbsolute ? "absolute" : "relative");
    if (fAbsolute)
        IOLog(" (%d..%d x %d..%d)", (int) fBaseX, (int) (fBaseX + ((SInt32) fBounds.maxx << fShiftX)),
              (int) fBaseY, (int) (fBaseY + ((SInt32) fBounds.maxy << fShiftY)));
    IOLog("%s\n", inputBit(kEvRel, kRelWheel) ? ", with a wheel" : "");
    setProperty("Absolute", fAbsolute);

    cfgWrite16(kComQueueSelect, 0);
    fNum = cfgRead16(kComQueueSize);
    if (fNum < 2) {
        IOLog("VirtioTablet: the event queue has %d entries\n", fNum);
        goto failed;
    }
    fSlots = fNum < kSlotsMax ? fNum : kSlotsMax;
    fAvailOff = 16 * fNum;
    fUsedOff = roundPage(fAvailOff + 6 + 2 * fNum);
    fRingSize = fUsedOff + roundPage(6 + 8 * fNum);
    fRing = (UInt8 *) IOMallocContiguous(fRingSize, kPage, &fRingPhys);
    fBuf = (UInt8 *) IOMallocContiguous(kPage, kPage, &fBufPhys);
    if (!fRing || !fBuf)
        goto failed;
    return true;

failed:
    cfgWrite8(kComStatus, kStatusFailed);
    return false;
}

/* Lays out the event queue, gives every slot to the device and lets it run. */
void io_virtio_VirtioTablet::deviceStart()
{
    UInt16 i;

    bzero(fRing, fRingSize);
    bzero(fBuf, kPage);
    for (i = 0; i < fSlots; i++) {
        volatile UInt8 *d = fRing + 16 * i;
        put32(d, fBufPhys + kEventSize * i);    /* the low half; the high stays zero below 4 GB */
        put32(d + 8, kEventSize);
        put16(d + 12, kDescWrite);
    }
    fLastUsed = 0;
    fNextAvail = 0;
    put16(fRing + fAvailOff, fPolled ? kAvailNoInterrupt : 0);
    barrier();

    cfgWrite16(kComQueueSelect, 0);
    cfgWrite16(kComQueueSize, fNum);
    cfgWrite16(kComQueueMSIX, 0xFFFF);
    cfgWrite32(kComQueueDesc,       fRingPhys);
    cfgWrite32(kComQueueDesc + 4,   0);
    cfgWrite32(kComQueueDriver,     fRingPhys + fAvailOff);
    cfgWrite32(kComQueueDriver + 4, 0);
    cfgWrite32(kComQueueDevice,     fRingPhys + fUsedOff);
    cfgWrite32(kComQueueDevice + 4, 0);
    fNotifyOff = cfgRead16(kComQueueNotifyOff);
    cfgWrite16(kComQueueEnable, 1);
    cfgWrite8(kComStatus, kStatusAcknowledge | kStatusDriver | kStatusFeaturesOK | kStatusDriverOK);

    for (i = 0; i < fSlots; i++)
        queuePublish(i);
    queueNotify();
}

/* ------------------------------------------------------------------------------------------ */
/* events                                                                                      */
/* ------------------------------------------------------------------------------------------ */

/* Buttons as IOHIPointing's dispatch routines take them: bit 0 the primary, bit 1 the secondary,
 * bit 2 the middle (10.3 tells it apart; 10.0 to 10.2 make any button past the first the
 * secondary), then the rest. */
static UInt32 buttonBit(UInt16 code)
{
    switch (code) {
    case kBtnLeft:   return 0x01;
    case kBtnTouch:  return 0x01;
    case kBtnRight:  return 0x02;
    case kBtnMiddle: return 0x04;
    case kBtnSide:   return 0x08;
    case kBtnExtra:  return 0x10;
    }
    return 0;
}

/* One event of a report: folded into the report's state until its EV_SYN. */
void io_virtio_VirtioTablet::event(UInt16 type, UInt16 code, UInt32 value)
{
    SInt32 v = (SInt32) value;
    UInt32 bit;

    switch (type) {
    case kEvSyn:
        if (code == kSynReport)
            report();
        else if (code == kSynDropped) {     /* the device lost events: the next report starts afresh */
            fDX = fDY = fWheel = fHWheel = 0;
            fMoved = false;
        }
        break;
    case kEvAbs:
        if (code == kAbsX) {
            v = (v - fBaseX) >> fShiftX;
            fX = (SInt16) (v < 0 ? 0 : v > fBounds.maxx ? fBounds.maxx : v);
            fMoved = true;
        } else if (code == kAbsY) {
            v = (v - fBaseY) >> fShiftY;
            fY = (SInt16) (v < 0 ? 0 : v > fBounds.maxy ? fBounds.maxy : v);
            fMoved = true;
        }
        break;
    case kEvRel:
        if (code == kRelX)
            fDX += v, fMoved = true;
        else if (code == kRelY)
            fDY += v, fMoved = true;
        else if (code == kRelWheel)
            fWheel += v;
        else if (code == kRelHWheel)
            fHWheel += v;
        break;
    case kEvKey:
        if (code == kBtnGearUp || code == kBtnGearDown) {
            if (value)
                fWheel += code == kBtnGearUp ? 1 : -1;
        } else if ((bit = buttonBit(code)) != 0) {
            if (value)
                fButtons |= bit;
            else
                fButtons &= ~bit;
        }
        break;
    }
}

/* A complete report: the pointer and buttons, then the wheel, to IOHIDSystem. */
void io_virtio_VirtioTablet::report()
{
    AbsoluteTime ts;

    clock_get_uptime(&ts);
    if (fMoved || fButtons != fLastButtons || fFirst) {
        if (fAbsolute) {
            /* IOHIDSystem scales the point to the screen in place: it gets copies */
            Point at;
            Bounds bounds = fBounds;

            at.x = fX;
            at.y = fY;
            /* in proximity always; the primary button as full pressure, which is what a tablet's tip
             * would say and keeps IOHIPointing's pressure-to-click threshold in step with it */
            dispatchAbsolutePointerEvent(&at, &bounds, fButtons, true, (fButtons & 1) ? 255 : 0,
                                         0, 255, 90, ts);
        } else
            dispatchRelativePointerEvent(fDX, fDY, fButtons, ts);
        fReports++;
    }
    if (fWheel || fHWheel)
        dispatchScrollWheelEvent((short) fWheel, (short) fHWheel, 0, ts);
    fLastButtons = fButtons;
    fDX = fDY = fWheel = fHWheel = 0;
    fMoved = false;
    fFirst = false;
}

/* Takes what the device has written, a slot at a time, and gives each slot straight back. */
void io_virtio_VirtioTablet::service()
{
    volatile UInt8 *used = fRing + fUsedOff;
    bool returned = false;

    if (!fRunning)
        return;
    while (fLastUsed != get16(used + 2)) {
        UInt32 id;
        volatile UInt8 *e;

        barrier();
        id = get32(used + 4 + 8 * (fLastUsed % fNum));
        fLastUsed++;
        if (id >= fSlots)
            continue;
        e = fBuf + kEventSize * id;
        fSeenEvent = true;
        event(get16(e), get16(e + 2), get32(e + 4));
        queuePublish((UInt16) id);
        returned = true;
    }
    if (returned)
        queueNotify();
    if (fReports != fReportsPublished) {
        setProperty("Reports", (unsigned long long) fReports, 32);
        fReportsPublished = fReports;
    }
}

bool io_virtio_VirtioTablet::interruptFilter(OSObject *owner, IOFilterInterruptEventSource *)
{
    io_virtio_VirtioTablet *me = (io_virtio_VirtioTablet *) owner;
    UInt8 isr = (UInt8) me->regRead(&me->fISRReg, 0, 1);    /* reading it acknowledges it */

    if (!isr)
        return false;
    OSBitOrAtomic(isr, (UInt32 *) &me->fISR);
    OSIncrementAtomic((SInt32 *) &me->fInterrupts);
    return true;
}

void io_virtio_VirtioTablet::interruptAction(OSObject *owner, IOInterruptEventSource *, int)
{
    ((io_virtio_VirtioTablet *) owner)->interruptOccurred();
}

void io_virtio_VirtioTablet::interruptOccurred()
{
    UInt32 isr;

    do {
        isr = fISR;
    } while (!OSCompareAndSwap(isr, 0, (UInt32 *) &fISR));
    service();
}

void io_virtio_VirtioTablet::timeoutAction(OSObject *owner, IOTimerEventSource *)
{
    ((io_virtio_VirtioTablet *) owner)->timeoutOccurred();
}

/* Every 100 ms: what an interrupt may have missed. Polled, every 10 ms does the work. If events
 * keep waiting that no interrupt announced -- the platform did not route the line -- the driver
 * polls from then on. */
void io_virtio_VirtioTablet::timeoutOccurred()
{
    if (!fRunning)
        return;
    if (!fPolled && fLastUsed != get16(fRing + fUsedOff + 2) && fInterrupts == fInterruptsSeen) {
        if (++fQuietTicks >= kQuietTicksMax) {
            IOLog("VirtioTablet: events raise no interrupt; polling every %d ms instead\n", kPollMS);
            fInterrupt->disable();
            fPolled = true;
            put16(fRing + fAvailOff, kAvailNoInterrupt);
        }
    } else
        fQuietTicks = 0;
    fInterruptsSeen = fInterrupts;
    service();
    if (!fSeenEvent && fReactivations < kReactivateMax) {
        fReactivateLeftMS -= (SInt32) (fPolled ? kPollMS : kWatchdogMS);
        if (fReactivateLeftMS <= 0) {
            reactivate();
            fReactivations++;
            fReactivateLeftMS = (SInt32) kReactivateFirstMS << fReactivations;
        }
    }
    fTimer->setTimeoutMS(fPolled ? kPollMS : kWatchdogMS);
}

/* Makes the device QEMU's current pointer again, while it has sent nothing yet.
 *
 * QEMU hands a VNC client's (or its window's) pointer to its "current" pointing device, the one the
 * guest activated last, and only an absolute current device makes it send absolute positions. The
 * virtio device is activated when its driver sets DRIVER_OK; on the PMU machines (10.2, 10.3) QEMU's
 * own USB mouse, which mac99 always builds, is activated at the guest's first poll of it, and
 * whichever of the two comes later at boot wins. With the USB mouse current, VNC is relative and
 * the tablet hears nothing (10.3.9, 2026-10-05: query-mice had "QEMU HID Mouse" current, the driver
 * 0 reports, the VNC client's pointer moved nothing, with or without a usb-tablet beside it;
 * mouse_set to the tablet made every move land). A reset takes the device out (QEMU deactivates it)
 * and DRIVER_OK puts it back in front. The USB mouse is activated once per USB reset, so a few tries
 * in the first minutes settle it; the first event ends them. */
void io_virtio_VirtioTablet::reactivate()
{
    IOLog("VirtioTablet: no event yet; making the device QEMU's current pointer again (%d)\n",
          (int) fReactivations + 1);
    if (!deviceNegotiate())
        return;
    deviceStart();
    fDX = fDY = fWheel = fHWheel = 0;
    fMoved = false;
    setProperty("Reactivations", (unsigned long long) (fReactivations + 1), 32);
}

/* ------------------------------------------------------------------------------------------ */
/* the service                                                                                 */
/* ------------------------------------------------------------------------------------------ */

/* Three: IOHIPointing passes the secondary button on only for a device with more than one. */
IOItemCount io_virtio_VirtioTablet::buttonCount()
{
    return kButtons;
}

/* Dots per inch, for the relative kind's acceleration: IOHIPointing's own default, an ADB mouse's.
 * At 400, a USB mouse's, the cursor covered 60 of 150 host pixels (10.1.5, virtio-mouse-pci). */
IOFixed io_virtio_VirtioTablet::resolution()
{
    return 100 << 16;
}

/* The device is brought up before IOHIPointing::start registers the driver, which is what makes
 * IOHIDSystem open it; so a device that is no pointer, or does not come up, is never seen. */
bool io_virtio_VirtioTablet::start(IOService *provider)
{
    OSBoolean *polled;

    fPCI = OSDynamicCast(IOPCIDevice, provider);
    if (!fPCI)
        return false;
    fPCI->retain();
    if (!fPCI->open(this))
        goto fail;

    polled = OSDynamicCast(OSBoolean, getProperty("Polled"));
    fPolled = polled && polled->isTrue();
    fFirst = true;
    fReactivateLeftMS = kReactivateFirstMS;

    fWorkLoop = IOWorkLoop::workLoop();
    if (!fWorkLoop)
        goto fail;
    if (!deviceProbe())
        goto fail;

    fTimer = IOTimerEventSource::timerEventSource(this, timeoutAction);
    if (!fTimer || fWorkLoop->addEventSource(fTimer) != kIOReturnSuccess)
        goto fail;
    if (!fPolled) {
        fInterrupt = IOFilterInterruptEventSource::filterInterruptEventSource(this, interruptAction,
                                                                              interruptFilter, provider, 0);
        if (!fInterrupt || fWorkLoop->addEventSource(fInterrupt) != kIOReturnSuccess) {
            IOLog("VirtioTablet: the device's interrupt cannot be had; polling every %d ms instead\n", kPollMS);
            if (fInterrupt)
                fInterrupt->release();
            fInterrupt = 0;
            fPolled = true;
        }
    }

    if (!super::start(provider))
        goto fail;

    deviceStart();
    fRunning = true;
    if (fInterrupt)
        fInterrupt->enable();
    fTimer->setTimeoutMS(fPolled ? kPollMS : kWatchdogMS);
    IOLog("VirtioTablet: %d event slots, %s\n", fSlots, fPolled ? "polled" : "interrupt-driven");
    return true;

fail:
    IOLog("VirtioTablet: could not start\n");
    teardown();
    return false;
}

/* Stops the device and lets go of the provider; what was allocated goes in free(). */
void io_virtio_VirtioTablet::teardown()
{
    fRunning = false;
    if (fTimer)
        fTimer->cancelTimeout();
    if (fInterrupt)
        fInterrupt->disable();
    if (fPCI && fCommon.bar < 6 && (fCommon.virt || fCfgWindowOff))
        deviceReset();
    if (fPCI && fPCI->isOpen(this))
        fPCI->close(this);
}

void io_virtio_VirtioTablet::stop(IOService *provider)
{
    teardown();
    super::stop(provider);
}

void io_virtio_VirtioTablet::free()
{
    int i;

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
    if (fRing)
        IOFreeContiguous(fRing, fRingSize);
    if (fBuf)
        IOFreeContiguous(fBuf, kPage);
    for (i = 0; i < 6; i++)
        if (fMaps[i])
            fMaps[i]->release();
    if (fPCI)
        fPCI->release();
    if (fWorkLoop)
        fWorkLoop->release();
    super::free();
}

IOWorkLoop *io_virtio_VirtioTablet::getWorkLoop() const
{
    return fWorkLoop;
}
