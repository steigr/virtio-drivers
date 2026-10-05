/*
 * VirtioTablet: QEMU's virtio input device as an absolute pointer (virtio-tablet-pci, 1af4:1052)
 * for Mac OS X 10.0 to 10.3 on PowerPC, so that the cursor goes where the host's pointer is -- over
 * VNC or a QEMU window, with no grab and no drift between the two cursors. The relative kind
 * (virtio-mouse-pci) is driven too; a virtio keyboard is left alone.
 *
 * Like VirtioVsock, the device has no legacy interface: QEMU makes virtio-input modern-only (1af4:1052,
 * one 64-bit memory BAR, whatever disable-modern says). So the transport is virtio 1.x's PCI one --
 * capabilities in configuration space, VIRTIO_F_VERSION_1, FEATURES_OK, a split ring whose three
 * parts are placed separately -- and everything the device reads or writes is little-endian,
 * swapped on the way. That code is VirtioVsock's.
 *
 * The device side, from the virtio 1.x "Input Device" chapter: queue 0 (eventq) carries events from
 * the device, each a struct virtio_input_event of eight bytes (le16 type, le16 code, le32 value, the
 * Linux evdev encoding); queue 1 (statusq), LEDs and the like going the other way, is not used. The
 * device configuration answers a select/subsel query: its name, which event types and codes it
 * sends (EV_BITS), and the range of each absolute axis (ABS_INFO). QEMU's tablet sends ABS_X and
 * ABS_Y in 0..32767, BTN_LEFT/RIGHT/MIDDLE/SIDE/EXTRA, REL_WHEEL, and an EV_SYN after each report;
 * it holds back a report until enough buffers are queued for all of it, so every slot is the
 * device's whenever the driver is not reading it.
 *
 * The Mac OS X side is IOHIPointing, the class every pointing device of these releases (ADB, USB,
 * PS/2) derives from: IOHIDSystem finds the driver when it registers, opens it, and takes its
 * events. An absolute report goes in through dispatchAbsolutePointerEvent with the axes' bounds,
 * and IOHIDSystem scales it to the screen (_scaleLocationToCurrentScreen, the same in 10.0's
 * xnu-123 and in IOHIDFamily-34 and -86 of 10.2 and 10.3); relative motion through
 * dispatchRelativePointerEvent, which accelerates it as for a mouse; the wheel through
 * dispatchScrollWheelEvent. Events are taken on the driver's own work loop, interrupt-driven, or
 * polled every 10 ms where no interrupt comes. Until the first event arrives the driver re-asserts
 * the device a few times, so that QEMU, which follows the device activated last, sends its pointer
 * here and not to its USB mouse (see reactivate()).
 *
 * Built twice from this one source, as VirtioConsole is: GCC 2.95 for 10.0 and 10.1, GCC 3 for
 * 10.2 on. So no templates, exceptions, RTTI or C++ library, and only what 10.0's kernel,
 * IOPCIFamily and IOHIPointing export. IOHIPointing lives in a different place in each release
 * (10.0's kernel, 10.1's IOHIDSystem kext, 10.2's and 10.3's IOHIDFamily); see the Makefile for how
 * the kext declares it.
 */

#ifndef _GR_STEI_VIRTIOTABLET_H
#define _GR_STEI_VIRTIOTABLET_H

#include <IOKit/IOService.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/IOFilterInterruptEventSource.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <IOKit/hidsystem/IOHIPointing.h>

/* Where a virtio structure sits: a capability's bar, offset and length, and its mapping. */
struct TabletRegion {
    UInt8               bar;
    UInt32              offset;
    UInt32              length;
    UInt8              *virt;       /* 0 where only configuration-space access works */
};

class io_virtio_VirtioTablet : public IOHIPointing
{
    OSDeclareDefaultStructors(io_virtio_VirtioTablet)

public:
    virtual bool       start(IOService *provider);
    virtual void       stop(IOService *provider);
    virtual void       free();
    virtual IOWorkLoop *getWorkLoop() const;

protected:
    virtual IOItemCount buttonCount();
    virtual IOFixed     resolution();

private:
    /* the transport */
    bool     findCaps();
    bool     deviceNegotiate();
    bool     deviceProbe();
    void     deviceStart();
    void     deviceReset();
    void     teardown();
    UInt32   regRead(TabletRegion *r, UInt32 off, int size);
    void     regWrite(TabletRegion *r, UInt32 off, int size, UInt32 value);
    UInt8    cfgRead8(UInt32 off);
    UInt16   cfgRead16(UInt32 off);
    void     cfgWrite8(UInt32 off, UInt8 v);
    void     cfgWrite16(UInt32 off, UInt16 v);
    void     cfgWrite32(UInt32 off, UInt32 v);
    bool     mapBar(UInt8 bar);
    void     queuePublish(UInt16 slot);
    void     queueNotify();

    /* the device's configuration */
    UInt8    inputQuery(UInt8 select, UInt8 subsel);
    bool     inputBit(UInt8 type, UInt16 code);
    bool     inputAxis(UInt8 axis, SInt32 *min, SInt32 *max);
    bool     axisSetup(UInt8 axis, SInt32 *base, UInt8 *shift, SInt16 *extent);

    /* events */
    void     event(UInt16 type, UInt16 code, UInt32 value);
    void     report();
    void     service();
    void     interruptOccurred();
    void     timeoutOccurred();
    void     reactivate();

    static bool interruptFilter(OSObject *owner, IOFilterInterruptEventSource *src);
    static void interruptAction(OSObject *owner, IOInterruptEventSource *src, int count);
    static void timeoutAction(OSObject *owner, IOTimerEventSource *src);

    /* 10.1's IOHIPointing has a _reserved pointer at the end that 10.0's header, which the gcc2 build
     * is compiled against, does not show: this keeps the driver's own fields clear of it. */
    void                         *fPadForReserved;

    IOPCIDevice                  *fPCI;
    IOMemoryMap                  *fMaps[6];
    IOWorkLoop                   *fWorkLoop;
    IOFilterInterruptEventSource *fInterrupt;   /* 0 when polled */
    IOTimerEventSource           *fTimer;

    TabletRegion                  fCommon, fNotify, fISRReg, fDevice;
    UInt32                        fNotifyMult;
    UInt16                        fCfgWindowOff;    /* the PCI_CFG capability, 0 if none */

    /* eventq: the rings, and one page holding every slot's eight bytes */
    UInt16                        fNum;
    UInt16                        fSlots;
    UInt16                        fNotifyOff;
    UInt8                        *fRing;
    IOPhysicalAddress             fRingPhys;
    vm_size_t                     fRingSize;
    UInt32                        fAvailOff, fUsedOff;
    UInt8                        *fBuf;
    IOPhysicalAddress             fBufPhys;
    UInt16                        fLastUsed;
    UInt16                        fNextAvail;

    /* what the device is, and how its axes map to IOHIDSystem's Bounds (16 bits each way) */
    bool                          fAbsolute;
    SInt32                        fBaseX, fBaseY;   /* the axes' minimums */
    UInt8                         fShiftX, fShiftY; /* range reductions to fit in 16 bits */
    Bounds                        fBounds;

    /* the report being put together, up to its EV_SYN */
    SInt16                        fX, fY;
    SInt32                        fDX, fDY;
    SInt32                        fWheel, fHWheel;
    UInt32                        fButtons;
    UInt32                        fLastButtons;
    bool                          fMoved;
    bool                          fFirst;           /* no report dispatched yet */

    bool                          fRunning;
    bool                          fPolled;
    volatile UInt32               fISR;
    volatile UInt32               fInterrupts;
    UInt32                        fInterruptsSeen;
    UInt32                        fQuietTicks;
    UInt32                        fReports;         /* dispatched, for the Reports property */
    UInt32                        fReportsPublished;
    bool                          fSeenEvent;       /* the device has sent anything at all */
    UInt32                        fReactivations;
    SInt32                        fReactivateLeftMS;
};

#endif /* !_GR_STEI_VIRTIOTABLET_H */
