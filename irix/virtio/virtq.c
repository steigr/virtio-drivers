/*
 * Split virtqueue: see virtq.h.
 *
 * Ring fields are stored a byte at a time in little-endian order, which needs
 * no knowledge of the CPU's byte order and no aligned access.
 */
#include "virtq.h"

static void
put16(unsigned char *p, vq_u16 v)
{
	p[0] = (unsigned char)v;
	p[1] = (unsigned char)(v >> 8);
}

static void
put32(unsigned char *p, vq_u32 v)
{
	p[0] = (unsigned char)v;
	p[1] = (unsigned char)(v >> 8);
	p[2] = (unsigned char)(v >> 16);
	p[3] = (unsigned char)(v >> 24);
}

static void
put64(unsigned char *p, vq_u64 v)
{
	put32(p, (vq_u32)v);
	put32(p + 4, (vq_u32)(v >> 32));
}

static vq_u16
get16(const unsigned char *p)
{
	return (vq_u16)(p[0] | (p[1] << 8));
}

static vq_u32
get32(const unsigned char *p)
{
	return (vq_u32)p[0] | ((vq_u32)p[1] << 8) | ((vq_u32)p[2] << 16) |
	    ((vq_u32)p[3] << 24);
}

#define DESC(vq, i)		((vq)->desc + 16 * (unsigned)(i))
#define AVAIL_FLAGS(vq)		((vq)->avail)
#define AVAIL_IDX(vq)		((vq)->avail + 2)
#define AVAIL_RING(vq, i)	((vq)->avail + 4 + 2 * (unsigned)(i))
#define USED_IDX(vq)		((vq)->used + 2)
#define USED_RING(vq, i)	((vq)->used + 4 + 8 * (unsigned)(i))

static void
vq_wb(struct virtq *vq, void *addr, unsigned long len)
{
	if (vq->ops && vq->ops->wb)
		vq->ops->wb(addr, len);
}

static void
vq_inv(struct virtq *vq, void *addr, unsigned long len)
{
	if (vq->ops && vq->ops->inv)
		vq->ops->inv(addr, len);
}

static void
vq_mb(struct virtq *vq)
{
	if (vq->ops && vq->ops->mb)
		vq->ops->mb();
}

int
virtq_init(struct virtq *vq, unsigned int num, void *desc, void *avail,
	   void *used, void **cookie, const struct vq_sync_ops *ops)
{
	unsigned int i;

	if (num == 0 || num > 32768 || (num & (num - 1)) != 0)
		return -1;
	vq->num = (vq_u16)num;
	vq->free_head = 0;
	vq->num_free = (vq_u16)num;
	vq->avail_idx = 0;
	vq->last_used = 0;
	vq->unpublished = 0;
	vq->desc = desc;
	vq->avail = avail;
	vq->used = used;
	vq->cookie = cookie;
	vq->ops = ops;

	/* The free list is chained through the descriptors' next fields. */
	for (i = 0; i < num; i++) {
		cookie[i] = 0;
		put16(DESC(vq, i) + 14, (vq_u16)(i + 1 < num ? i + 1 : 0));
	}
	vq_wb(vq, desc, VQ_DESC_BYTES(num));
	vq_wb(vq, avail, VQ_AVAIL_BYTES(num));
	vq_wb(vq, used, VQ_USED_BYTES(num));
	vq_inv(vq, used, VQ_USED_BYTES(num));
	return 0;
}

int
virtq_add_chain(struct virtq *vq, const struct vq_seg *segs, unsigned int n,
		void *cookie)
{
	vq_u16 head, i, flags;
	unsigned int k;
	unsigned char *d;

	if (n == 0 || n > vq->num_free)
		return -1;
	head = i = vq->free_head;
	for (k = 0; k < n; k++) {
		d = DESC(vq, i);
		put64(d, segs[k].addr);
		put32(d + 8, segs[k].len);
		flags = segs[k].device_writes ? VRING_DESC_F_WRITE : 0;
		if (k + 1 < n)
			flags |= VRING_DESC_F_NEXT;
		put16(d + 12, flags);
		/* d + 14, next, already leads to the next free descriptor. */
		vq_wb(vq, d, 16);
		i = get16(d + 14);
	}
	vq->free_head = i;
	vq->num_free = (vq_u16)(vq->num_free - n);
	vq->cookie[head] = cookie;

	put16(AVAIL_RING(vq, (vq->avail_idx + vq->unpublished) & (vq->num - 1)),
	    head);
	vq->unpublished++;
	return 0;
}

int
virtq_add(struct virtq *vq, vq_u64 addr, vq_u32 len, int device_writes,
	  void *cookie)
{
	struct vq_seg seg;

	seg.addr = addr;
	seg.len = len;
	seg.device_writes = device_writes;
	return virtq_add_chain(vq, &seg, 1, cookie);
}

int
virtq_publish(struct virtq *vq)
{
	int n = vq->unpublished;

	if (n == 0)
		return 0;
	/* The whole avail ring: the slots may have wrapped past its end. */
	vq_wb(vq, vq->avail, VQ_AVAIL_BYTES(vq->num));
	vq_mb(vq);
	vq->avail_idx = (vq_u16)(vq->avail_idx + n);
	vq->unpublished = 0;
	put16(AVAIL_IDX(vq), vq->avail_idx);
	vq_wb(vq, AVAIL_IDX(vq), 2);
	vq_mb(vq);
	return n;
}

void *
virtq_get_used(struct virtq *vq, vq_u32 *written)
{
	vq_u16 idx, head, tail;
	unsigned char *e;
	void *cookie;

	vq_inv(vq, vq->used, VQ_USED_BYTES(vq->num));
	idx = get16(USED_IDX(vq));
	if (idx == vq->last_used)
		return 0;
	vq_mb(vq);
	e = USED_RING(vq, vq->last_used & (vq->num - 1));
	head = (vq_u16)get32(e);
	vq->last_used++;
	/* A device that reports a descriptor it does not hold: skip the entry. */
	if (head >= vq->num || vq->cookie[head] == 0)
		return virtq_get_used(vq, written);
	if (written)
		*written = get32(e + 4);
	cookie = vq->cookie[head];
	vq->cookie[head] = 0;
	/* The whole chain goes back, in the order its next fields already give. */
	tail = head;
	vq->num_free++;
	while (get16(DESC(vq, tail) + 12) & VRING_DESC_F_NEXT) {
		tail = get16(DESC(vq, tail) + 14);
		vq->num_free++;
	}
	put16(DESC(vq, tail) + 14, vq->free_head);
	vq->free_head = head;
	return cookie;
}

void
virtq_set_no_interrupt(struct virtq *vq, int no_interrupt)
{
	put16(AVAIL_FLAGS(vq), no_interrupt ? VRING_AVAIL_F_NO_INTERRUPT : 0);
	vq_wb(vq, AVAIL_FLAGS(vq), 2);
	vq_mb(vq);
}
