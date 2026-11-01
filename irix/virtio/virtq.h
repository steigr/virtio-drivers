/*
 * Split virtqueue (virtio 1.x), single-descriptor chains only.
 *
 * Everything the device reads or writes is little-endian whatever the CPU is.
 * This file and virtq.c know nothing about the kernel: the owner allocates the
 * three ring areas and passes cache maintenance in through vq_sync_ops, so the
 * same code runs in tests/host on the build host.
 *
 * The machines this targets are not cache coherent for GIO DMA, so the rings
 * stay cached and the owner's callbacks write back and invalidate them.
 * Uncached (K1) rings are not an option: IP26/IP28 ECC memory does not take
 * uncached partial writes outside "slow mode".
 */
#ifndef _VIRTQ_H_
#define _VIRTQ_H_

#ifdef __sgi
typedef unsigned short vq_u16;
typedef unsigned int vq_u32;
typedef unsigned long long vq_u64;
#else
#include <stddef.h>
#include <stdint.h>
typedef uint16_t vq_u16;
typedef uint32_t vq_u32;
typedef uint64_t vq_u64;
#endif

#define VRING_DESC_F_NEXT	1
#define VRING_DESC_F_WRITE	2
#define VRING_AVAIL_F_NO_INTERRUPT 1

struct vring_desc {
	vq_u64	addr;
	vq_u32	len;
	vq_u16	flags;
	vq_u16	next;
};

#define VQ_DESC_BYTES(n)	(16 * (n))
#define VQ_AVAIL_BYTES(n)	(6 + 2 * (n))
#define VQ_USED_BYTES(n)	(6 + 8 * (n))

/*
 * The owner's cache maintenance.  wb makes CPU stores visible to the device;
 * inv makes device stores visible to the CPU.  mb orders the stores either
 * side of it as the device sees them.  Any of them may be null.
 */
struct vq_sync_ops {
	void	(*wb)(void *addr, unsigned long len);
	void	(*inv)(void *addr, unsigned long len);
	void	(*mb)(void);
};

struct virtq {
	vq_u16		num;		/* ring size, a power of two */
	vq_u16		free_head;	/* first free descriptor, chained by next */
	vq_u16		num_free;
	vq_u16		avail_idx;	/* the value published in avail->idx */
	vq_u16		last_used;	/* next used entry to consume */
	vq_u16		unpublished;	/* entries added since the last publish */
	unsigned char	*desc;
	unsigned char	*avail;
	unsigned char	*used;
	void		**cookie;	/* per descriptor, num entries */
	const struct vq_sync_ops *ops;
};

/*
 * Lay the queue out over zeroed ring areas, sized VQ_*_BYTES(num), and a
 * cookie array of num pointers.  Writes back and invalidates the rings.
 * Returns 0, or -1 if num is not a power of two.
 */
int	virtq_init(struct virtq *vq, unsigned int num, void *desc, void *avail,
		   void *used, void **cookie, const struct vq_sync_ops *ops);

/*
 * Queue one buffer at bus address addr.  device_writes says the device fills
 * it rather than reads it.  The owner has already made the buffer's contents
 * visible to the device.  The device does not see it until virtq_publish.
 * Returns 0, or -1 when the ring is full.
 */
int	virtq_add(struct virtq *vq, vq_u64 addr, vq_u32 len, int device_writes,
		  void *cookie);

/* Publish what virtq_add queued.  Returns how many; 0 needs no notify. */
int	virtq_publish(struct virtq *vq);

/*
 * Take the next buffer the device has finished with.  Returns its cookie and
 * sets *written to the bytes the device wrote, or returns 0 when there are no
 * more.  A null cookie must not be passed to virtq_add.
 */
void	*virtq_get_used(struct virtq *vq, vq_u32 *written);

/* Ask the device not to interrupt for this queue, or to interrupt again. */
void	virtq_set_no_interrupt(struct virtq *vq, int no_interrupt);

#endif /* _VIRTQ_H_ */
