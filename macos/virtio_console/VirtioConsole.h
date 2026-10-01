/*
 * VirtioConsole: QEMU's legacy virtio console as a serial port, for Mac OS X 10.0 and later on
 * PowerPC.
 *
 * QEMU's mac99 gives a PowerPC guest no serial port its releases can use for the guest agent, and
 * 10.0 no USB either; every release drives PCI. This driver claims a legacy virtio-serial-pci
 * (1af4:1003) with one port -- port 0, a virtconsole, on queues 0 and 1 -- negotiates no features,
 * and presents the port through IOSerialFamily, so that it appears as /dev/cu.virtio-* and
 * /dev/tty.virtio-* and the Perl agent serves it as it serves FTDI's /dev/cu.usbserial-*.
 *
 * Built twice from this one source: with GCC 2.95 (December 2001 Developer Tools) for 10.0 and
 * 10.1, and with GCC 3 (December 2002 Developer Tools) for 10.2 on, because the two compilers'
 * C++ ABIs differ and a kernel extension links against the kernel's. So: no templates, no
 * exceptions, no RTTI, no C++ library -- IOKit's own macros and classes only, and only what 10.0's
 * kernel, IOPCIFamily and IOSerialFamily 1.0.4 export.
 *
 * The serial side follows the structure of Apple's Apple16X50Serial (Apple16X50UARTSync.cpp,
 * Apple sample-code licence): every entry point runs gated on the driver's own work loop, and a
 * caller that has to wait sleeps on the command gate with commandSleep(), which 10.0's kernel
 * exports. The line is virtual: baud rate, parity and flow control are accepted and remembered and
 * change nothing; carrier, CTS and DSR are always asserted.
 *
 * Polled: a timer on the work loop moves bytes between the virtqueues and the port's queues.
 * The device's interrupt (OpenBIOS routes it; QEMU showed IRQ 30 beside the sungem's 29) is not
 * used yet, and the rings ask the device for none.
 */

#ifndef _GR_STEI_VIRTIOCONSOLE_H
#define _GR_STEI_VIRTIOCONSOLE_H

#include <IOKit/IOService.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IOCommandGate.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <IOKit/serial/IOSerialDriverSync.h>
#include <IOKit/serial/IORS232SerialStreamSync.h>

/* A split virtqueue, legacy layout: descriptors, then the available ring, then -- on the next
 * page -- the used ring, in one physically contiguous allocation whose page number the device is
 * given. Legacy virtio on a big-endian guest keeps the rings in the guest's byte order, so nothing
 * here is swapped. */
struct VirtqDesc {
    UInt32 addrHigh;
    UInt32 addrLow;
    UInt32 len;
    UInt16 flags;
    UInt16 next;
};

struct VirtqUsedElem {
    UInt32 id;
    UInt32 len;
};

struct Virtq {
    UInt16                  index;      /* the queue's number on the device */
    UInt16                  num;        /* its size, as the device reports it */
    UInt8                  *mem;        /* the rings */
    IOPhysicalAddress       memPhys;
    vm_size_t               memSize;
    volatile VirtqDesc     *desc;
    volatile UInt16        *availFlags;
    volatile UInt16        *availIdx;
    volatile UInt16        *availRing;
    volatile UInt16        *usedFlags;
    volatile UInt16        *usedIdx;
    volatile VirtqUsedElem *usedRing;
    UInt16                  lastUsed;   /* the used index this driver has consumed up to */
    UInt16                  nextAvail;  /* the available index it has published up to */
    UInt8                  *bufs;       /* its data buffers, also physically contiguous */
    IOPhysicalAddress       bufsPhys;
    vm_size_t               bufsSize;
    UInt32                  bufSize;    /* one buffer's size */
    UInt16                  bufCount;
};

/* A byte ring for one direction of the port. */
struct ByteQueue {
    UInt8  *buf;
    UInt32  size;
    UInt32  head;   /* next byte out */
    UInt32  count;
    UInt32  lowWater;
    UInt32  highWater;
};

class io_virtio_VirtioConsole : public IOSerialDriverSync
{
    OSDeclareDefaultStructors(io_virtio_VirtioConsole)

public:
    virtual bool start(IOService *provider);
    virtual void stop(IOService *provider);
    virtual void free();

    /* IOSerialDriverSync */
    virtual IOReturn acquirePort(bool sleep, void *refCon);
    virtual IOReturn releasePort(void *refCon);
    virtual IOReturn setState(UInt32 state, UInt32 mask, void *refCon);
    virtual UInt32   getState(void *refCon);
    virtual IOReturn watchState(UInt32 *state, UInt32 mask, void *refCon);
    virtual UInt32   nextEvent(void *refCon);
    virtual IOReturn executeEvent(UInt32 event, UInt32 data, void *refCon);
    virtual IOReturn requestEvent(UInt32 event, UInt32 *data, void *refCon);
    virtual IOReturn enqueueEvent(UInt32 event, UInt32 data, bool sleep, void *refCon);
    virtual IOReturn dequeueEvent(UInt32 *event, UInt32 *data, bool sleep, void *refCon);
    virtual IOReturn enqueueData(UInt8 *buffer, UInt32 size, UInt32 *count, bool sleep, void *refCon);
    virtual IOReturn dequeueData(UInt8 *buffer, UInt32 size, UInt32 *count, UInt32 min, void *refCon);

private:
    /* the device */
    bool     deviceStart();
    void     deviceReset();
    bool     queueSetup(Virtq *q, UInt16 index, UInt32 bufSize, UInt16 bufCount, bool deviceWrites);
    void     queueFree(Virtq *q);
    void     queuePublish(Virtq *q, UInt16 id);
    void     poll();
    bool     pollRx();
    bool     pollTx();

    /* the port, gated */
    IOReturn acquirePortGated(bool sleep);
    IOReturn releasePortGated();
    void     setStateGated(UInt32 state, UInt32 mask);
    IOReturn watchStateGated(UInt32 *state, UInt32 mask);
    IOReturn executeEventGated(UInt32 event, UInt32 data);
    IOReturn requestEventGated(UInt32 event, UInt32 *data);
    IOReturn enqueueDataGated(UInt8 *buffer, UInt32 size, UInt32 *count, bool sleep);
    IOReturn dequeueDataGated(UInt8 *buffer, UInt32 size, UInt32 *count, UInt32 min);
    IOReturn dequeueEventGated(UInt32 *event, UInt32 *data, bool sleep);
    UInt32   queueState();
    void     activate(bool on);

    static IOReturn acquirePortAction(OSObject *owner, void *arg0, void *, void *, void *);
    static IOReturn releasePortAction(OSObject *owner, void *, void *, void *, void *);
    static IOReturn setStateAction(OSObject *owner, void *arg0, void *arg1, void *, void *);
    static IOReturn watchStateAction(OSObject *owner, void *arg0, void *arg1, void *, void *);
    static IOReturn nextEventAction(OSObject *owner, void *, void *, void *, void *);
    static IOReturn executeEventAction(OSObject *owner, void *arg0, void *arg1, void *, void *);
    static IOReturn requestEventAction(OSObject *owner, void *arg0, void *arg1, void *, void *);
    static IOReturn enqueueDataAction(OSObject *owner, void *arg0, void *arg1, void *arg2, void *arg3);
    static IOReturn dequeueDataAction(OSObject *owner, void *arg0, void *arg1, void *arg2, void *arg3);
    static IOReturn dequeueEventAction(OSObject *owner, void *arg0, void *arg1, void *arg2, void *);
    static void     pollAction(OSObject *owner, IOTimerEventSource *sender);

    IOPCIDevice             *fPCI;
    IOMemoryMap             *fIO;
    IOWorkLoop              *fWorkLoop;
    IOCommandGate           *fGate;
    IOTimerEventSource      *fTimer;
    IORS232SerialStreamSync *fNub;

    Virtq      fRx;            /* queue 0, the port's receiveq */
    Virtq      fTx;            /* queue 1, its transmitq */
    bool       fTxBusy;        /* a transmit descriptor is with the device */

    ByteQueue  fRxQ;
    ByteQueue  fTxQ;

    UInt32     fState;
    UInt32     fWatchMask;
    bool       fAcquired;
    bool       fStopping;
    bool       fDeviceUp;

    /* what the line was set to, reported back as set */
    UInt32     fDataRate;
    UInt32     fDataSize;
    UInt32     fParity;
    UInt32     fStopBits;
    UInt32     fFlowControl;
    UInt32     fXOn;
    UInt32     fXOff;
    UInt32     fDelay;
    UInt32     fDataLatency;
    UInt32     fMinLatency;
};

#endif /* !_GR_STEI_VIRTIOCONSOLE_H */
