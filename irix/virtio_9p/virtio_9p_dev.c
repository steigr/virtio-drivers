/*
 * The virtio-9p device: one queue, a mount tag in config space, and requests
 * that are a chain of a message for the device and room for its reply.
 *
 * A small pool of requests, each with its own buffers, is made when the device
 * attaches, so that the paging path never allocates.  A request waiting for its
 * reply sleeps, uninterruptibly: the device always answers, and the buffer
 * cache must not lose a write to a signal.
 *
 * On a slot whose line reaches no handler (VIRTIO_NO_INTR) the ring stays quiet
 * and replies are collected by polling: briefly straight after a request, which
 * catches nearly all of them, then from a timeout.
 *
 * Cache: as for virtio_serial, buffers stay cached.  A message is written back before
 * the device reads it, and the reply buffer is invalidated both when handed
 * over and when it comes back.
 */
#include <sys/types.h>
#include <sys/cmn_err.h>
#include <sys/ddi.h>
#include <sys/debug.h>
#include <sys/errno.h>
#include <sys/kmem.h>
#include <sys/ksynch.h>
#include <sys/sema.h>
#include <sys/systm.h>
#include <sys/atomic_ops.h>
#include <ksys/behavior.h>
#include <sys/vfs.h>
#include <sys/vnode.h>

#include "virtio_9p_impl.h"
#include "virtio_bus.h"
#include "virtq.h"

#define VIRTIO_F_VERSION_1_HI	0x1	/* bit 32, in feature word 1 */
#define VIRTIO_9P_F_MOUNT_TAG	0x1	/* bit 0, in feature word 0 */

#define PD_NREQ		4
#define PD_QSIZE	16		/* at least two descriptors a request */
#define PD_TAG_MAX	64
#define PD_POLL_US	10000		/* the timeout, without an interrupt */
#define PD_SPIN_US	5
#define PD_SPINS	100		/* after a request, without an interrupt */

struct pd_req {
	struct p9_req	pr_r;		/* first: the client hands this back */
	__uint64_t	pr_txbus;
	__uint64_t	pr_rxbus;
	sv_t		*pr_sv;
	int		pr_done;
	struct pd_req	*pr_next;
};

struct p9dev {
	const struct virtio_bus_ops *pd_ops;
	void		*pd_bus;
	lock_t		*pd_lock;
	sv_t		*pd_freesv;
	struct virtq	pd_vq;
	void		*pd_cookies[PD_QSIZE];
	void		*pd_rings;
	size_t		pd_ring_bytes;
	struct pd_req	pd_reqs[PD_NREQ];
	struct pd_req	*pd_free;
	struct p9_client pd_client;
	char		pd_tag[PD_TAG_MAX + 1];
	int		pd_intr_connected;
	int		pd_polled;
	int		pd_poll_stop;	/* under pd_lock: do not re-arm */
	toid_t		pd_poll_id;	/* under pd_lock */
	int		pd_ready;	/* Tversion done */
};

static struct p9dev *p9_dev;
static p9_u32 pd_nextfid = 1;
static int pd_holds;

/* -------- cache maintenance -------- */

static void
pd_wb(void *addr, unsigned long len)
{
	dki_dcache_wb(addr, (int)len);
}

static void
pd_inv(void *addr, unsigned long len)
{
	dki_dcache_inval(addr, (int)len);
}

static void
pd_mb(void)
{
	flushbus();
}

static const struct vq_sync_ops pd_sync_ops = { pd_wb, pd_inv, pd_mb };

/* -------- requests -------- */

/* Collect what the device has finished.  pd_lock held. */
static void
pd_service(struct p9dev *pd)
{
	struct pd_req *r;
	vq_u32 len;

	while ((r = virtq_get_used(&pd->pd_vq, &len)) != 0) {
		r->pr_r.rxlen = len;
		r->pr_done = 1;
		SV_SIGNAL(r->pr_sv);
	}
}

static struct p9_req *
pd_get(void *ctx)
{
	struct p9dev *pd = ctx;
	struct pd_req *r;
	int s;

	s = LOCK(pd->pd_lock, plhi);
	while ((r = pd->pd_free) == 0) {
		SV_WAIT(pd->pd_freesv, pd->pd_lock, s);
		s = LOCK(pd->pd_lock, plhi);
	}
	pd->pd_free = r->pr_next;
	UNLOCK(pd->pd_lock, s);
	return &r->pr_r;
}

static void
pd_put(void *ctx, struct p9_req *pr)
{
	struct p9dev *pd = ctx;
	struct pd_req *r = (struct pd_req *)pr;
	int s;

	s = LOCK(pd->pd_lock, plhi);
	r->pr_next = pd->pd_free;
	pd->pd_free = r;
	SV_SIGNAL(pd->pd_freesv);
	UNLOCK(pd->pd_lock, s);
}

static int
pd_rpc(void *ctx, struct p9_req *pr, p9_u32 txlen)
{
	struct p9dev *pd = ctx;
	struct pd_req *r = (struct pd_req *)pr;
	struct vq_seg segs[2];
	int s, spins;

	dki_dcache_wb(pr->tx, (int)txlen);
	dki_dcache_inval(pr->rx, VIRTIO_9P_MSIZE);
	segs[0].addr = r->pr_txbus;
	segs[0].len = txlen;
	segs[0].device_writes = 0;
	segs[1].addr = r->pr_rxbus;
	segs[1].len = VIRTIO_9P_MSIZE;
	segs[1].device_writes = 1;

	s = LOCK(pd->pd_lock, plhi);
	r->pr_done = 0;
	/* The ring holds two descriptors for every request in the pool. */
	(void)virtq_add_chain(&pd->pd_vq, segs, 2, r);
	if (virtq_publish(&pd->pd_vq) > 0)
		pd->pd_ops->notify(pd->pd_bus, 0);
	if (pd->pd_polled) {
		for (spins = 0; !r->pr_done && spins < PD_SPINS; spins++) {
			pd_service(pd);
			if (r->pr_done)
				break;
			UNLOCK(pd->pd_lock, s);
			us_delay(PD_SPIN_US);
			s = LOCK(pd->pd_lock, plhi);
		}
	}
	while (!r->pr_done) {
		SV_WAIT(r->pr_sv, pd->pd_lock, s);
		s = LOCK(pd->pd_lock, plhi);
	}
	UNLOCK(pd->pd_lock, s);

	dki_dcache_inval(pr->rx, VIRTIO_9P_MSIZE);
	return pr->rxlen <= VIRTIO_9P_MSIZE ? 0 : EIO;
}

static const struct p9_transport pd_transport = { pd_get, pd_rpc, pd_put };

/* -------- bringing the device up and down -------- */

static void
pd_poll_tick(struct p9dev *pd)
{
	int s;

	s = LOCK(pd->pd_lock, plhi);
	pd_service(pd);
	if (!pd->pd_poll_stop)
		pd->pd_poll_id = itimeout(pd_poll_tick, pd,
		    drv_usectohz(PD_POLL_US), pltimeout);
	UNLOCK(pd->pd_lock, s);
}

static void
pd_intr(void *arg)
{
	struct p9dev *pd = arg;
	int s;

	if (pd->pd_ops->intr_ack(pd->pd_bus) == 0)
		return;
	s = LOCK(pd->pd_lock, plhi);
	pd_service(pd);
	UNLOCK(pd->pd_lock, s);
}

static void
pd_teardown(struct p9dev *pd)
{
	unsigned int i;
	toid_t id;
	int s;

	if (pd->pd_polled) {
		s = LOCK(pd->pd_lock, plhi);
		pd->pd_poll_stop = 1;
		id = pd->pd_poll_id;
		UNLOCK(pd->pd_lock, s);
		(void)untimeout_wait(id);
	}
	/* A reset stops the device using any ring or buffer. */
	pd->pd_ops->set_status(pd->pd_bus, 0);
	if (pd->pd_intr_connected)
		pd->pd_ops->intr_disconnect(pd->pd_bus, pd);
	for (i = 0; i < PD_NREQ; i++) {
		struct pd_req *r = &pd->pd_reqs[i];

		if (r->pr_r.tx != 0)
			kmem_free(r->pr_r.tx, VIRTIO_9P_MSIZE);
		if (r->pr_r.rx != 0)
			kmem_free(r->pr_r.rx, VIRTIO_9P_MSIZE);
		if (r->pr_sv != 0)
			SV_DEALLOC(r->pr_sv);
	}
	if (pd->pd_rings != 0)
		kmem_free(pd->pd_rings, pd->pd_ring_bytes);
	if (pd->pd_freesv != 0)
		SV_DEALLOC(pd->pd_freesv);
	if (pd->pd_lock != 0)
		LOCK_DEALLOC(pd->pd_lock);
	kmem_free(pd, sizeof(*pd));
}

#define PD_LINE		128
#define PD_ROUND(n)	(((n) + PD_LINE - 1) & ~(size_t)(PD_LINE - 1))

static int
pd_queue_init(struct p9dev *pd)
{
	const struct virtio_bus_ops *ops = pd->pd_ops;
	size_t dlen, alen, ulen;
	unsigned char *mem;

	if (ops->queue_max(pd->pd_bus, 0) < PD_QSIZE)
		return EIO;
	dlen = PD_ROUND(VQ_DESC_BYTES(PD_QSIZE));
	alen = PD_ROUND(VQ_AVAIL_BYTES(PD_QSIZE));
	ulen = PD_ROUND(VQ_USED_BYTES(PD_QSIZE));
	pd->pd_ring_bytes = dlen + alen + ulen;
	mem = kmem_zalloc(pd->pd_ring_bytes, KM_SLEEP | KM_PHYSCONTIG | KM_CACHEALIGN);
	pd->pd_rings = mem;
	if (virtq_init(&pd->pd_vq, PD_QSIZE, mem, mem + dlen, mem + dlen + alen,
	    pd->pd_cookies, &pd_sync_ops) != 0)
		return EINVAL;
	virtq_set_no_interrupt(&pd->pd_vq, 1);
	return ops->queue_setup(pd->pd_bus, 0, PD_QSIZE,
	    ops->dma_addr(pd->pd_bus, mem, dlen),
	    ops->dma_addr(pd->pd_bus, mem + dlen, alen),
	    ops->dma_addr(pd->pd_bus, mem + dlen + alen, ulen));
}

static int
pd_negotiate(struct p9dev *pd)
{
	const struct virtio_bus_ops *ops = pd->pd_ops;
	void *bus = pd->pd_bus;

	ops->set_status(bus, 0);
	ops->set_status(bus, VIRTIO_S_ACKNOWLEDGE);
	ops->set_status(bus, VIRTIO_S_ACKNOWLEDGE | VIRTIO_S_DRIVER);
	if (!(ops->get_features(bus, 1) & VIRTIO_F_VERSION_1_HI) ||
	    !(ops->get_features(bus, 0) & VIRTIO_9P_F_MOUNT_TAG))
		return ENODEV;
	ops->set_features(bus, 0, VIRTIO_9P_F_MOUNT_TAG);
	ops->set_features(bus, 1, VIRTIO_F_VERSION_1_HI);
	ops->set_status(bus, VIRTIO_S_ACKNOWLEDGE | VIRTIO_S_DRIVER |
	    VIRTIO_S_FEATURES_OK);
	if (!(ops->get_status(bus) & VIRTIO_S_FEATURES_OK))
		return ENODEV;
	return 0;
}

static int
pd_attach(const struct virtio_bus_ops *ops, void *bus, void **scp)
{
	struct p9dev *pd;
	unsigned int i, taglen;
	int err;

	pd = kmem_zalloc(sizeof(*pd), KM_SLEEP);
	pd->pd_ops = ops;
	pd->pd_bus = bus;
	pd->pd_lock = LOCK_ALLOC(1, plhi, 0, KM_SLEEP);
	pd->pd_freesv = SV_ALLOC(SV_DEFAULT, KM_SLEEP, "p9free");

	if ((err = pd_negotiate(pd)) != 0)
		goto fail;
	taglen = ops->config_read8(bus, 0) | (ops->config_read8(bus, 1) << 8);
	if (taglen == 0 || taglen > PD_TAG_MAX) {
		cmn_err(CE_WARN, "9p: mount tag of %d bytes", taglen);
		err = ENODEV;
		goto fail;
	}
	for (i = 0; i < taglen; i++)
		pd->pd_tag[i] = (char)ops->config_read8(bus, 2 + i);

	for (i = 0; i < PD_NREQ; i++) {
		struct pd_req *r = &pd->pd_reqs[i];

		r->pr_r.tx = kmem_alloc(VIRTIO_9P_MSIZE, KM_SLEEP | KM_PHYSCONTIG |
		    KM_CACHEALIGN);
		r->pr_r.rx = kmem_alloc(VIRTIO_9P_MSIZE, KM_SLEEP | KM_PHYSCONTIG |
		    KM_CACHEALIGN);
		r->pr_r.tag = (p9_u16)i;
		r->pr_txbus = ops->dma_addr(bus, r->pr_r.tx, VIRTIO_9P_MSIZE);
		r->pr_rxbus = ops->dma_addr(bus, r->pr_r.rx, VIRTIO_9P_MSIZE);
		r->pr_sv = SV_ALLOC(SV_DEFAULT, KM_SLEEP, "p9req");
		r->pr_next = pd->pd_free;
		pd->pd_free = r;
	}
	if ((err = pd_queue_init(pd)) != 0)
		goto fail;
	ops->set_status(bus, VIRTIO_S_ACKNOWLEDGE | VIRTIO_S_DRIVER |
	    VIRTIO_S_FEATURES_OK | VIRTIO_S_DRIVER_OK);

	err = ops->intr_connect(bus, pd);
	if (err == VIRTIO_NO_INTR) {
		cmn_err(CE_NOTE, "9p: no interrupt on this slot, polling");
		pd->pd_polled = 1;
		pd->pd_poll_id = itimeout(pd_poll_tick, pd,
		    drv_usectohz(PD_POLL_US), pltimeout);
	} else if (err != 0) {
		goto fail;
	} else {
		pd->pd_intr_connected = 1;
		(void)ops->intr_ack(bus);
		virtq_set_no_interrupt(&pd->pd_vq, 0);
	}

	pd->pd_client.trans = &pd_transport;
	pd->pd_client.ctx = pd;
	pd->pd_client.msize = VIRTIO_9P_MSIZE;
	if ((err = p9_version(&pd->pd_client)) != 0) {
		cmn_err(CE_WARN, "9p: the device refused %s: error %d",
		    P9_DIALECT, err);
		goto fail;
	}
	pd->pd_ready = 1;
	cmn_err(CE_CONT, "9p: share \"%s\", messages of %d bytes\n", pd->pd_tag,
	    pd->pd_client.msize);
	p9_dev = pd;
	*scp = pd;
	return 0;

fail:
	ops->set_status(bus, VIRTIO_S_FAILED);
	pd_teardown(pd);
	return err;
}

static int
pd_detach(void *arg)
{
	struct p9dev *pd = arg;

	if (pd_holds != 0)
		return EBUSY;
	p9_dev = 0;
	pd_teardown(pd);
	return 0;
}

static const struct virtio_driver pd_driver = {
	"9p",
	VIRTIO_ID_9P,
	pd_attach,
	pd_detach,
	pd_intr,
};


/* -------- what the filesystem uses -------- */

struct p9_client *
p9dev_client(void)
{
	return p9_dev != 0 && p9_dev->pd_ready ? &p9_dev->pd_client : 0;
}

const char *
p9dev_tag(void)
{
	return p9_dev != 0 ? p9_dev->pd_tag : "";
}

p9_u32
p9dev_newfid(void)
{
	return (p9_u32)atomicAddUint(&pd_nextfid, 1);
}

void
p9dev_hold(void)
{
	(void)atomicAddInt(&pd_holds, 1);
}

void
p9dev_rele(void)
{
	(void)atomicAddInt(&pd_holds, -1);
}

int
p9dev_init(void)
{
	return virtio_bus_init(&pd_driver, "virtio_9p_");
}

int
p9dev_fini(void)
{
	return virtio_bus_fini("virtio_9p_");
}
