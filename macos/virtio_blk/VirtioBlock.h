/*
 * VirtioBlock: QEMU's legacy virtio block device (virtio-blk-pci, 1af4:1001) as a disk, for
 * Mac OS X 10.0 to 10.3 on PowerPC and 10.4 to 10.15 on Intel.
 *
 * QEMU's mac99 gives a PowerPC guest an IDE controller and little else a disk can sit on; this is
 * the paravirtual alternative. The driver is an IOBlockStorageDevice that matches the PCI device
 * itself, standing where the nub of an ATA or SCSI driver stands: IOBlockStorageDriver
 * (IOStorageFamily) attaches to it and makes the IOMedia, the partition schemes find the
 * partitions, and the BSD layer makes /dev/disk<n> of them.
 *
 * Built three times from this one source: GCC 2.95 (December 2001 Developer Tools) for 10.0 and
 * 10.1, GCC 3 (December 2002 Developer Tools) for PowerPC 10.2 on -- as VirtioConsole and VirtioNet
 * are -- and Xcode 3.2.6 on Snow Leopard for Intel, one kext of an i386 slice (the 10.4u SDK: every
 * 32-bit kernel, 10.4 to 10.7) and an x86_64 slice (the 10.6 SDK: every 64-bit kernel, 10.6 to
 * 10.15). So no templates, exceptions, RTTI or C++ library, and only what 10.0's kernel,
 * IOPCIFamily and IOStorageFamily export. No BSD symbol is used, so the gcc2 build declares no
 * com.apple.kernel.bsd (see ../virtio9p).
 *
 * What differs between them is kept to a few lines, each under the #if that says why:
 * - Byte order. A legacy device keeps its rings and configuration in the guest's order, big-endian
 *   on PowerPC and little-endian on Intel, so the structures below are written with whole 64-bit
 *   fields rather than halves, and the capacity is read into memory as bytes.
 * - The barrier: PowerPC's sync, x86's mfence.
 * - Physical addresses: getPhysicalSegment on PowerPC (10.0 has nothing else), getPhysicalSegment64
 *   on i386 (above 4 GB with PAE), getPhysicalSegment with kIOMemoryMapperNone on x86_64.
 * - The 64-bit kernels' IOBlockStorageDevice (__LP64__): doAsyncReadWrite takes 64-bit block
 *   numbers, attributes and a completion pointer; doSyncReadWrite and reportMaxRead/WriteTransfer
 *   are gone (the maximum transfer is a property); the write-cache calls are pure.
 *
 * The device side, from the virtio 0.9.5 (legacy PCI) specification and the virtio 1.x "Block
 * Device" chapter's legacy notes (elliotnunn/classicvirtio's device-block.c, which does this for
 * Mac OS 9, has the same request layout):
 *
 * - Features: RO (the disk is read-only) and FLUSH (the write cache can be flushed) only. Sizes and
 *   segment counts are not negotiated; the driver keeps to what any queue can hold. Sectors are
 *   512 bytes whatever the device's BLK_SIZE says, which is what the partition schemes expect.
 * - One request queue. A request is a chain of descriptors: the 16-byte header (type, priority,
 *   sector), then the data, one descriptor for each physically contiguous piece of the buffer
 *   (device-writable for a read), then the one status byte, device-writable. A flush is the header
 *   and the status alone.
 * - Each of a handful of slots owns a fixed block of 34 descriptors, and 32 bytes of header and
 *   status in a page shared by all, so a used element's id names the slot. With every slot in
 *   flight, requests wait in a list and go out as slots come back.
 * - The data is never copied: IOStorage hands the driver an IOMemoryDescriptor, which is wired
 *   (prepare) and walked by physical segment. At most 30 pages go out in one chain, which cannot
 *   be more than 32 segments even when it starts in the middle of a page; a larger request goes
 *   out in such chunks one after the other on its slot (reportMaxReadTransfer says 30 pages, and
 *   10.0's IOBlockStorageDriver sends larger requests regardless).
 * - Rings, header and configuration in the guest's (big-endian) byte order, as legacy devices keep
 *   them. The capacity is read a byte at a time, as VirtioNet reads its MAC address.
 * - Capacity beyond 2^32 sectors is not reachable: the block number of doAsyncReadWrite is 32 bits
 *   in these releases (2 TB).
 *
 * Interrupt-driven where the platform routes the device's INTx (a filter interrupt event source
 * reads and so clears the ISR at primary interrupt time, and the work loop does the rest), polled
 * from a work-loop timer where it does not, or when the personality says Polled. Everything that
 * touches the rings runs on the driver's own work loop, entered through a command gate by the
 * threads that submit requests.
 */

#ifndef _GR_STEI_VIRTIOBLOCK_H
#define _GR_STEI_VIRTIOBLOCK_H

#include <IOKit/IOService.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IOCommandGate.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/IOFilterInterruptEventSource.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <IOKit/storage/IOBlockStorageDevice.h>

/* The split virtqueue, legacy layout; see ../ppc/virtioconsole/VirtioConsole.h. The address is one
 * 64-bit field, which is right in either byte order. */
struct VirtioBlockDesc {
    UInt64 addr;
    UInt32 len;
    UInt16 flags;
    UInt16 next;
};

struct VirtioBlockUsedElem {
    UInt32 id;
    UInt32 len;
};

/* struct virtio_blk_outhdr in the guest's byte order, and the status byte behind it. */
struct VirtioBlockMeta {
    UInt32 type;
    UInt32 ioprio;
    UInt64 sector;
    volatile UInt8 status;
    UInt8  pad[15];
};

/* The physical address of a piece of memory, as wide as the platform's. */
#ifdef __ppc__
typedef IOPhysicalAddress VirtioBlockPhys;
#else
typedef UInt64 VirtioBlockPhys;
#endif

/* One request from IOStorage, from doAsyncReadWrite or doSynchronizeCache until its completion
 * has been called. */
struct VirtioBlockRequest {
    VirtioBlockRequest  *next;          /* the list of those waiting for a slot */
    IOMemoryDescriptor  *buffer;        /* retained and prepared; 0 for a flush */
    IOStorageCompletion  completion;
    UInt32               type;          /* VIRTIO_BLK_T_* */
    UInt64               block;
    UInt32               blocks;
    UInt32               done;          /* blocks the device has finished */
    UInt32               chunk;         /* blocks of the chain in flight */
};

class io_virtio_VirtioBlock : public IOBlockStorageDevice
{
    OSDeclareDefaultStructors(io_virtio_VirtioBlock)

public:
    /* IOService */
    virtual bool       start(IOService *provider);
    virtual void       stop(IOService *provider);
    virtual void       free();

    /* IOBlockStorageDevice */
#ifdef __LP64__
    virtual IOReturn   doAsyncReadWrite(IOMemoryDescriptor *buffer, UInt64 block, UInt64 nblks,
                                        IOStorageAttributes *attributes, IOStorageCompletion *completion);
    virtual IOReturn   getWriteCacheState(bool *enabled);
    virtual IOReturn   setWriteCacheState(bool enabled);
#else
    virtual IOReturn   doAsyncReadWrite(IOMemoryDescriptor *buffer, UInt32 block, UInt32 nblks,
                                        IOStorageCompletion completion);
    virtual IOReturn   doSyncReadWrite(IOMemoryDescriptor *buffer, UInt32 block, UInt32 nblks);
    virtual IOReturn   reportMaxReadTransfer(UInt64 blockSize, UInt64 *max);
    virtual IOReturn   reportMaxWriteTransfer(UInt64 blockSize, UInt64 *max);
#endif
    virtual IOReturn   doEjectMedia(void);
    virtual IOReturn   doFormatMedia(UInt64 byteCapacity);
    virtual UInt32     doGetFormatCapacities(UInt64 *capacities, UInt32 capacitiesMaxCount) const;
    virtual IOReturn   doLockUnlockMedia(bool doLock);
    virtual IOReturn   doSynchronizeCache(void);
    virtual char      *getVendorString(void);
    virtual char      *getProductString(void);
    virtual char      *getRevisionString(void);
    virtual char      *getAdditionalDeviceInfoString(void);
    virtual IOReturn   reportBlockSize(UInt64 *blockSize);
    virtual IOReturn   reportEjectability(bool *isEjectable);
    virtual IOReturn   reportLockability(bool *isLockable);
    virtual IOReturn   reportMaxValidBlock(UInt64 *maxBlock);
    virtual IOReturn   reportMediaState(bool *mediaPresent, bool *changedState);
    virtual IOReturn   reportPollRequirements(bool *pollRequired, bool *pollIsExpensive);
    virtual IOReturn   reportRemovability(bool *isRemovable);
    virtual IOReturn   reportWriteProtection(bool *isWriteProtected);

private:
    /* the device */
    bool     deviceStart();
    void     deviceReset();
    bool     queueAlloc();
    void     queueFree();

    /* the work; all but queueRequest on the work loop */
    IOReturn queueRequest(IOMemoryDescriptor *buffer, UInt32 type, UInt64 block, UInt64 nblks,
                     IOStorageCompletion completion);
    void     submit(VirtioBlockRequest *req);
    bool     issue(VirtioBlockRequest *req, UInt16 slot);
    void     finish(VirtioBlockRequest *req, IOReturn status);
    void     service();
    void     armTimer();
    void     ackInterrupt();
    void     interruptOccurred(int count);
    void     timeoutOccurred();

    static IOReturn submitAction(OSObject *owner, void *req, void *, void *, void *);
    static IOReturn armAction(OSObject *owner, void *, void *, void *, void *);
    static bool     interruptFilter(OSObject *owner, IOFilterInterruptEventSource *src);
    static void     interruptAction(OSObject *owner, IOInterruptEventSource *src, int count);
    static void     timeoutAction(OSObject *owner, IOTimerEventSource *src);

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
    IOCommandGate                *fGate;
    IOFilterInterruptEventSource *fInterrupt;   /* 0 when polled */
    IOTimerEventSource           *fTimer;

    /* the request queue */
    UInt16                        fNum;         /* its size, as the device reports it */
    UInt8                        *fMem;         /* the rings */
    VirtioBlockPhys               fMemPhys;
    vm_size_t                     fMemSize;
    volatile VirtioBlockDesc     *fDesc;
    volatile UInt16              *fAvailFlags;
    volatile UInt16              *fAvailIdx;
    volatile UInt16              *fAvailRing;
    volatile UInt16              *fUsedFlags;
    volatile UInt16              *fUsedIdx;
    volatile VirtioBlockUsedElem *fUsedRing;
    UInt16                        fLastUsed;    /* the used index consumed up to */
    UInt16                        fNextAvail;   /* the available index published up to */

    UInt16                        fSlots;
    VirtioBlockMeta              *fMeta;        /* one page, a header and status for every slot */
    VirtioBlockPhys               fMetaPhys;
    VirtioBlockRequest          **fInFlight;    /* by slot */
    UInt16                       *fFree;        /* the free slots, a stack */
    UInt16                        fFreeCount;
    VirtioBlockRequest           *fWaitHead;    /* requests waiting for a slot, oldest first */
    VirtioBlockRequest           *fWaitTail;

    UInt64                        fCapacity;    /* in 512-byte sectors */
    UInt32                        fFeatures;    /* what was negotiated */
    bool                          fRunning;
    bool                          fMediaReported;   /* reportMediaState has said the media is there */
    bool                          fPolled;
    bool                          fTimerArmed;
    UInt32                        fTimerMS;     /* the period it was armed with */
    UInt32                        fStrays;      /* interrupts the device raised while polled */
    volatile UInt32               fISR;         /* what the filter read, for the action */
    volatile UInt32               fInterrupts;  /* interrupts taken, */
    UInt32                        fInterruptsSeen;  /* as of the last watchdog tick */
    UInt32                        fQuietTicks;  /* ticks that found work no interrupt had announced */
};

#endif /* !_GR_STEI_VIRTIOBLOCK_H */
