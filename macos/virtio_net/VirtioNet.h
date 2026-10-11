/*
 * VirtioNet: QEMU's legacy virtio network device (virtio-net-pci, 1af4:1000) as an Ethernet
 * controller, for Mac OS X 10.0 to 10.3 on PowerPC and 10.4 to 10.15 on Intel.
 *
 * QEMU's mac99 gives a PowerPC guest a sungem, which these releases drive; this is the paravirtual
 * alternative, a second (or only) interface that needs no NIC emulated register by register. The
 * driver is an IOEthernetController: IONetworkingFamily makes the interface (en<n>) of it, and the
 * BSD stack, configd and the Network preferences treat it as any other Ethernet port.
 *
 * Built three times from this one source. On PowerPC twice, as VirtioConsole is: GCC 2.95 (December
 * 2001 Developer Tools) for 10.0 and 10.1, GCC 3 (December 2002 Developer Tools) for 10.2 on. On
 * Intel once, against the kernel programming interfaces 10.4 introduced (ABI kpi, VIRTIONET_KPI): an
 * i386 slice for the 32-bit kernels of 10.4 to 10.7 and an x86_64 one for the 64-bit kernels of 10.6
 * on, built on 10.6 with Xcode 3.2.6. What differs between them is kept to a few lines: the barrier,
 * the width of a physical address, and how an mbuf is walked -- by its fields before 10.4, through
 * the mbuf KPI from 10.4 on, where struct mbuf is no longer a kext's to see. So no templates,
 * exceptions, RTTI or C++ library, and only what 10.0's kernel, IOPCIFamily and IONetworkingFamily
 * 1.0.7 export. No BSD symbol is used either -- mbufs are allocated and freed through
 * IONetworkController and walked by their fields -- so that the gcc2 build, which cannot declare
 * com.apple.kernel.bsd (10.0's and 10.1's are mutually incompatible, see ../ppc/virtio9p), needs none.
 *
 * The device side, from the virtio 0.9.5 (legacy PCI) specification and the virtio 1.x "Network
 * Device" chapter's legacy notes:
 *
 * - Features: MAC (the address in the device's configuration) and STATUS (link up/down) only.
 *   Without MRG_RXBUF the per-packet header is the 10-byte struct virtio_net_hdr; without CSUM and
 *   the GSO features it is all zero going out and ignored coming in. Without CTRL_VQ there is no
 *   receive filtering: QEMU's device starts promiscuous, and the guest sees every frame on the
 *   link, as on a hub. Without ANY_LAYOUT a legacy driver must give the header a descriptor of its
 *   own, so every packet is a chain of two: header, then frame.
 * - Queue 0 receives, queue 1 transmits. Each has fixed slots, 2 KiB apiece, two to a physically
 *   contiguous page: the header at the slot's start, the frame 16 bytes in. Slot n is always the
 *   chain of descriptors 2n and 2n+1, so a used element's id names its slot. Frames are copied
 *   between slots and mbufs; under emulation the copy is cheap beside the exits around it.
 * - Rings and configuration in the guest's own byte order, as legacy devices keep them: big-endian on
 *   PowerPC, little-endian on Intel, and in either case nothing is swapped.
 *
 * Interrupt-driven where the platform routes the device's INTx (a filter interrupt event source
 * reads and so clears the ISR at primary interrupt time, and the work loop does the rest), polled
 * from a work-loop timer where it does not, or when the personality says Polled. Everything that
 * touches the rings -- outputPacket through an IOGatedOutputQueue, the interrupt action, the timer,
 * enable and disable through IONetworkController's command gate -- runs on the driver's own work
 * loop, so nothing more is locked.
 */

#ifndef _GR_STEI_VIRTIONET_H
#define _GR_STEI_VIRTIONET_H

#include <IOKit/IOService.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/IOFilterInterruptEventSource.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <IOKit/network/IOEthernetController.h>
#include <IOKit/network/IOEthernetInterface.h>
#include <IOKit/network/IOGatedOutputQueue.h>

/* A packet as IONetworkingFamily hands it over: struct mbuf before 10.4, the opaque mbuf_t of the
 * mbuf KPI from 10.4 on. The virtual functions that take one must be declared with the type the
 * family declares them with, or they do not override and are never called. */
#ifdef VIRTIONET_KPI
#include <sys/kpi_mbuf.h>
typedef mbuf_t VirtioNetPacket;
#else
struct mbuf;
typedef struct mbuf *VirtioNetPacket;
#endif

/* A split virtqueue, legacy layout; see ../ppc/virtioconsole/VirtioConsole.h. The address is 64
 * bits in the guest's order, which on a 32-bit kernel only ever has its low half set. */
struct VirtioNetDesc {
    UInt64 addr;
    UInt32 len;
    UInt16 flags;
    UInt16 next;
};

struct VirtioNetUsedElem {
    UInt32 id;
    UInt32 len;
};

struct VirtioNetQueue {
    UInt16                      index;      /* the queue's number on the device */
    UInt16                      num;        /* its size, as the device reports it */
    UInt8                      *mem;        /* the rings */
    IOPhysicalAddress           memPhys;
    vm_size_t                   memSize;
    volatile VirtioNetDesc     *desc;
    volatile UInt16            *availFlags;
    volatile UInt16            *availIdx;
    volatile UInt16            *availRing;
    volatile UInt16            *usedFlags;
    volatile UInt16            *usedIdx;
    volatile VirtioNetUsedElem *usedRing;
    UInt16                      lastUsed;   /* the used index consumed up to */
    UInt16                      nextAvail;  /* the available index published up to */
    UInt16                      slots;      /* packet slots, each a chain of two descriptors */
    UInt8                     **pages;      /* the slots' pages, two slots to a page, */
    IOPhysicalAddress          *pagesPhys;  /* and where they are for the device */
    UInt16                      pageCount;
};

class io_virtio_VirtioNet : public IOEthernetController
{
    OSDeclareDefaultStructors(io_virtio_VirtioNet)

public:
    /* IOService */
    virtual bool       start(IOService *provider);
    virtual void       stop(IOService *provider);
    virtual void       free();
    virtual bool       terminate(IOOptionBits options = 0);
    virtual IOWorkLoop *getWorkLoop() const;

    /* IONetworkController */
    virtual bool       createWorkLoop();
    virtual IOReturn   enable(IONetworkInterface *netif);
    virtual IOReturn   disable(IONetworkInterface *netif);
    virtual UInt32     outputPacket(VirtioNetPacket m, void *param);
    virtual IOOutputQueue *createOutputQueue();
    virtual bool       configureInterface(IONetworkInterface *netif);
    virtual const OSString *newVendorString() const;
    virtual const OSString *newModelString() const;
    virtual IOReturn   selectMedium(const IONetworkMedium *medium);
    virtual void       getPacketBufferConstraints(IOPacketBufferConstraints *constraints) const;

    /* IOEthernetController */
    virtual IOReturn   getHardwareAddress(IOEthernetAddress *addr);
    virtual IOReturn   setPromiscuousMode(bool active);
    virtual IOReturn   setMulticastMode(bool active);
    virtual IOReturn   setMulticastList(IOEthernetAddress *addrs, UInt32 count);

private:
    /* the device */
    bool     deviceProbe();
    bool     deviceStart();
    void     deviceReset();
    bool     queueAlloc(VirtioNetQueue *q, UInt16 index, UInt16 maxSlots);
    void     queueFree(VirtioNetQueue *q);
    void     queueStart(VirtioNetQueue *q, bool deviceWrites);
    UInt8   *slotAddress(VirtioNetQueue *q, UInt16 slot);
    void     queuePublish(VirtioNetQueue *q, UInt16 slot);
    void     queueNotify(VirtioNetQueue *q);
    UInt16   readStatus();

    /* the work, on the work loop */
    void     service();
    bool     serviceRx();
    void     serviceTx();
    void     updateLink();
    void     interruptOccurred(int count);
    bool     interruptAttach();
    void     interruptDetach();
    void     timeoutOccurred();

    static bool interruptFilter(OSObject *owner, IOFilterInterruptEventSource *src);
    static void interruptAction(OSObject *owner, IOInterruptEventSource *src, int count);
    static void timeoutAction(OSObject *owner, IOTimerEventSource *src);

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
    IOFilterInterruptEventSource *fInterrupt;   /* 0 when polled, and while the interface is down */
    IOTimerEventSource           *fTimer;
    IOEthernetInterface          *fNetif;
    IOOutputQueue                *fOutputQueue;
    IONetworkStats               *fNetStats;
    OSDictionary                 *fMediumDict;
    const IONetworkMedium        *fMedium;      /* the one there is: autoselect */

    VirtioNetQueue                fRx;          /* queue 0, receiveq */
    VirtioNetQueue                fTx;          /* queue 1, transmitq */
    UInt16                       *fTxFree;      /* the free transmit slots, a stack */
    UInt16                        fTxFreeCount;
    bool                          fTxStalled;   /* the output queue is waiting for a slot */

    IOEthernetAddress             fMAC;
    UInt32                        fFeatures;    /* what was negotiated */
    UInt16                        fLinkStatus;  /* the device's, VIRTIO_NET_S_* */
    bool                          fLinkReported;
    bool                          fEnabled;
    bool                          fPolled;
    volatile UInt32               fISR;         /* what the filter read, for the action */
    volatile UInt32               fInterrupts;  /* interrupts taken, */
    UInt32                        fInterruptsSeen;  /* as of the last watchdog tick */
    UInt32                        fQuietTicks;  /* ticks that found work no interrupt had announced */
    UInt32                        fLinkTicks;   /* polled: ticks since the link status was read */
};

#endif /* !_GR_STEI_VIRTIONET_H */
