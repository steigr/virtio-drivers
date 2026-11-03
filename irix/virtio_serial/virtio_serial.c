/*
 * virtio-serial (virtio-console with VIRTIO_CONSOLE_F_MULTIPORT) for IRIX 6.5.
 *
 * Queue layout: 0/1 receive and transmit for port 0, 2/3 control receive and
 * transmit, then 2p+2 / 2p+3 for port p.  "Receive" is the guest's view.
 *
 * Ports are discovered once, polling the control queue in vs_attach,
 * before the interrupt is connected: a device announces its ports in answer to
 * DEVICE_READY and the driver needs process context to answer them.  The rings
 * ask for no interrupts until then, since a line asserted with no handler behind
 * it wedges the machine.  After that the only control messages handled are the
 * host end opening and closing, which need no reply, so the interrupt handler
 * never sends.
 *
 * A slot whose line reaches no handler (VIRTIO_NO_INTR) keeps its rings quiet
 * for good and is serviced from a timeout instead.
 *
 * Locking: sc_lock is a spinlock taken at plhi and covers the queues and all
 * port state.  It is never held across uiomove or a sleep.  A port's readers
 * and writers are each serialized by a sleeping mutex.
 *
 * Cache: ring memory and buffers stay cached.  CPU stores are written back
 * before the device may read them; buffers the device fills are invalidated
 * both when handed over and when returned, because an R10000 can speculatively
 * load a line back into the cache in between.  KM_CACHEALIGN keeps every buffer
 * off cache lines shared with anything else.
 */
#include <sys/types.h>
#include <sys/cmn_err.h>
#include <sys/conf.h>
#include <sys/cred.h>
#include <sys/ddi.h>
#include <sys/debug.h>
#include <sys/errno.h>
#include <sys/file.h>
#include <sys/filio.h>
#include <sys/hwgraph.h>
#include <sys/iograph.h>
#include <sys/kmem.h>
#include <sys/ksynch.h>
#include <sys/mload.h>
#include <sys/poll.h>
#include <sys/sema.h>
#include <sys/systm.h>
#include <sys/uio.h>

#include "vioser.h"
#include "virtio_bus.h"
#include "virtq.h"

/* Driver entry points and flags the kernel looks up by prefix. */
int		vioser_devflag = D_MP;
char		*vioser_mversion = M_VERSION;

#define VIRTIO_F_VERSION_1_HI	0x1		/* bit 32, in feature word 1 */
#define VIRTIO_CONSOLE_F_MULTIPORT 0x2		/* bit 1, in feature word 0 */

/* Control events (virtio 1.x 5.3.6.2). */
#define CTL_DEVICE_READY	0
#define CTL_DEVICE_ADD		1
#define CTL_DEVICE_REMOVE	2
#define CTL_PORT_READY		3
#define CTL_CONSOLE_PORT	4
#define CTL_RESIZE		5
#define CTL_PORT_OPEN		6
#define CTL_PORT_NAME		7
#define CTL_HDR_BYTES		8

#define VS_MAX_PORTS		31
#define VS_QSIZE		16	/* entries in every ring */
#define VS_PORT_BUFS		8	/* per direction, per port */
#define VS_BUF_BYTES		4096
#define VS_CTL_BUFS		VS_QSIZE
#define VS_CTL_BYTES		256

/* How long vs_attach polls for the port handshake, in milliseconds. */
#define VS_HANDSHAKE_MS		5000
/* With every port answered, how long the control queue must stay quiet. */
#define VS_QUIET_MS		200
/* Service interval on a slot without an interrupt. */
#define VS_POLL_US		10000

#define CTLQ_RX			2
#define CTLQ_TX			3
#define PORT_RXQ(p)		((p) == 0 ? 0 : 2 * (p) + 2)
#define PORT_TXQ(p)		((p) == 0 ? 1 : 2 * (p) + 3)

struct vs_buf {
	struct vs_buf	*b_next;
	unsigned char	*b_data;
	__uint64_t	b_bus;
	unsigned int	b_cap;
	unsigned int	b_len;		/* bytes the device wrote */
	unsigned int	b_off;		/* bytes already read */
};

struct vs_queue {
	struct virtq	q_vq;
	void		*q_rings;
	size_t		q_ring_bytes;
	void		*q_cookies[VS_QSIZE];
};

struct vs_port {
	struct vioser_softc *p_sc;
	unsigned int	p_id;
	unsigned int	p_flags;	/* VIOSER_F_* */
	int		p_present;	/* the device announced this port */
	int		p_answered;	/* we sent PORT_READY */
	int		p_open;
	char		p_name[VIOSER_NAME_MAX];
	vertex_hdl_t	p_vertex;
	int		p_edge;		/* linked under virtio-ports */
	struct vs_buf	*p_bufs;	/* all of them, for freeing */
	struct vs_buf	*p_rx_head;	/* filled, not yet read */
	struct vs_buf	*p_rx_tail;
	struct vs_buf	*p_tx_free;
	sv_t		*p_rsv;
	sv_t		*p_wsv;
	mutex_t		*p_rmutex;
	mutex_t		*p_wmutex;
	struct pollhead	*p_ph;
};

struct vioser_softc {
	const struct virtio_bus_ops *sc_ops;
	void		*sc_bus;
	lock_t		*sc_lock;
	mutex_t		*sc_openlock;	/* serializes open and close */
	unsigned int	sc_nports;
	unsigned int	sc_nqueues;
	struct vs_queue	*sc_queues;
	struct vs_port	*sc_ports;
	struct vs_buf	*sc_ctl_bufs;	/* VS_CTL_BUFS * 2, for freeing */
	struct vs_buf	*sc_ctl_tx_free;
	int		sc_intr_connected;
	int		sc_attached;	/* handshake done: ports are final */
	int		sc_polled;	/* no interrupt: serviced by sc_poll_id */
	int		sc_poll_stop;	/* under sc_lock: do not re-arm */
	toid_t		sc_poll_id;	/* under sc_lock */
};

/* -------- cache maintenance -------- */

static void
vs_wb(void *addr, unsigned long len)
{
	dki_dcache_wb(addr, (int)len);
}

static void
vs_inv(void *addr, unsigned long len)
{
	dki_dcache_inval(addr, (int)len);
}

static void
vs_mb(void)
{
	flushbus();
}

static const struct vq_sync_ops vs_sync_ops = { vs_wb, vs_inv, vs_mb };

/* -------- allocation -------- */

static void *
vs_dma_alloc(size_t len)
{
	return kmem_zalloc(len, KM_SLEEP | KM_PHYSCONTIG | KM_CACHEALIGN);
}

/* Free what vs_buf_array made. */
static void
vs_buf_array_free(struct vs_buf *b, unsigned int count)
{
	unsigned int i;

	for (i = 0; i < count; i++) {
		kmem_free(b[i].b_data, b[i].b_cap);
	}
	kmem_free(b, count * sizeof(*b));
}

/* An array of count buffers, so they can be freed together. */
static struct vs_buf *
vs_buf_array(struct vioser_softc *sc, unsigned int count, unsigned int cap)
{
	struct vs_buf *a;
	unsigned int i;

	a = kmem_zalloc(count * sizeof(*a), KM_SLEEP);
	for (i = 0; i < count; i++) {
		a[i].b_data = vs_dma_alloc(cap);
		a[i].b_cap = cap;
		a[i].b_bus = sc->sc_ops->dma_addr(sc->sc_bus, a[i].b_data, cap);
	}
	return a;
}

/* -------- queues -------- */

/*
 * One block holding the three rings, each starting on its own cache line so
 * that the device-written used ring shares no line with the driver's stores.
 * 128 bytes is the largest secondary line these machines have.
 */
#define VS_LINE			128
#define VS_ROUND(n)		(((n) + VS_LINE - 1) & ~(size_t)(VS_LINE - 1))

static int
vs_queue_init(struct vioser_softc *sc, unsigned int qi)
{
	struct vs_queue *q = &sc->sc_queues[qi];
	size_t dlen, alen, ulen;
	unsigned char *mem;
	unsigned int max;
	const struct virtio_bus_ops *ops = sc->sc_ops;

	max = ops->queue_max(sc->sc_bus, qi);
	if (max < VS_QSIZE) {
		cmn_err(CE_WARN, "vioser: queue %d holds only %d entries",
		    qi, max);
		return EIO;
	}
	dlen = VS_ROUND(VQ_DESC_BYTES(VS_QSIZE));
	alen = VS_ROUND(VQ_AVAIL_BYTES(VS_QSIZE));
	ulen = VS_ROUND(VQ_USED_BYTES(VS_QSIZE));
	q->q_ring_bytes = dlen + alen + ulen;
	mem = vs_dma_alloc(q->q_ring_bytes);
	q->q_rings = mem;
	if (virtq_init(&q->q_vq, VS_QSIZE, mem, mem + dlen, mem + dlen + alen,
	    q->q_cookies, &vs_sync_ops) != 0)
		return EINVAL;
	virtq_set_no_interrupt(&q->q_vq, 1);
	return ops->queue_setup(sc->sc_bus, qi, VS_QSIZE,
	    ops->dma_addr(sc->sc_bus, mem, dlen),
	    ops->dma_addr(sc->sc_bus, mem + dlen, alen),
	    ops->dma_addr(sc->sc_bus, mem + dlen + alen, ulen));
}

static void
vs_kick(struct vioser_softc *sc, unsigned int qi)
{
	if (virtq_publish(&sc->sc_queues[qi].q_vq) > 0)
		sc->sc_ops->notify(sc->sc_bus, qi);
}

/* Give a buffer to the device to fill.  sc_lock held, or still single threaded. */
static void
vs_post_rx(struct vioser_softc *sc, unsigned int qi, struct vs_buf *b)
{
	dki_dcache_inval(b->b_data, (int)b->b_cap);
	b->b_len = 0;
	b->b_off = 0;
	(void)virtq_add(&sc->sc_queues[qi].q_vq, b->b_bus, b->b_cap, 1, b);
}

/* -------- control queue -------- */

static void
vs_put_le32(unsigned char *p, __uint32_t v)
{
	p[0] = (unsigned char)v;
	p[1] = (unsigned char)(v >> 8);
	p[2] = (unsigned char)(v >> 16);
	p[3] = (unsigned char)(v >> 24);
}

static void
vs_ctl_tx_reclaim(struct vioser_softc *sc)
{
	struct vs_buf *b;

	while ((b = virtq_get_used(&sc->sc_queues[CTLQ_TX].q_vq, 0)) != 0) {
		b->b_next = sc->sc_ctl_tx_free;
		sc->sc_ctl_tx_free = b;
	}
}

/*
 * Send a control message.  Process context only: when every control buffer is
 * with the device this waits for it, which takes no longer than the device's
 * next service pass.
 */
static int
vs_ctl_send(struct vioser_softc *sc, unsigned int id, unsigned int event,
	    unsigned int value)
{
	struct vs_buf *b;
	int s, tries;

	s = LOCK(sc->sc_lock, plhi);
	for (tries = 0; ; tries++) {
		vs_ctl_tx_reclaim(sc);
		if ((b = sc->sc_ctl_tx_free) != 0)
			break;
		if (tries >= VS_HANDSHAKE_MS) {
			UNLOCK(sc->sc_lock, s);
			cmn_err(CE_WARN, "vioser: control queue stuck");
			return EIO;
		}
		UNLOCK(sc->sc_lock, s);
		us_delay(1000);
		s = LOCK(sc->sc_lock, plhi);
	}
	sc->sc_ctl_tx_free = b->b_next;
	vs_put_le32(b->b_data, id);
	b->b_data[4] = (unsigned char)event;
	b->b_data[5] = (unsigned char)(event >> 8);
	b->b_data[6] = (unsigned char)value;
	b->b_data[7] = (unsigned char)(value >> 8);
	dki_dcache_wb(b->b_data, CTL_HDR_BYTES);
	(void)virtq_add(&sc->sc_queues[CTLQ_TX].q_vq, b->b_bus, CTL_HDR_BYTES,
	    0, b);
	vs_kick(sc, CTLQ_TX);
	UNLOCK(sc->sc_lock, s);
	return 0;
}

/*
 * One control message from the device.  sc_lock held; may not send, since this
 * also runs in the interrupt handler.
 */
static void
vs_ctl_handle(struct vioser_softc *sc, struct vs_buf *b)
{
	unsigned char *m = b->b_data;
	__uint32_t id;
	unsigned int event, value, len;
	struct vs_port *p;

	if (b->b_len < CTL_HDR_BYTES)
		return;
	id = (__uint32_t)m[0] | ((__uint32_t)m[1] << 8) |
	    ((__uint32_t)m[2] << 16) | ((__uint32_t)m[3] << 24);
	event = m[4] | (m[5] << 8);
	value = m[6] | (m[7] << 8);
	if (id >= sc->sc_nports) {
		cmn_err(CE_WARN, "vioser: control event %d for port %d of %d",
		    event, id, sc->sc_nports);
		return;
	}
	p = &sc->sc_ports[id];

	switch (event) {
	case CTL_DEVICE_ADD:
		/* Ports are published once, in vs_attach. */
		if (sc->sc_attached)
			cmn_err(CE_WARN, "vioser: port %d added late, ignored",
			    id);
		else
			p->p_present = 1;
		break;
	case CTL_DEVICE_REMOVE:
		cmn_err(CE_WARN, "vioser: port %d removed, ignored", id);
		break;
	case CTL_CONSOLE_PORT:
		if (value != 0)
			p->p_flags |= VIOSER_F_CONSOLE;
		break;
	case CTL_PORT_NAME:
		len = b->b_len - CTL_HDR_BYTES;
		if (len >= VIOSER_NAME_MAX)
			len = VIOSER_NAME_MAX - 1;
		bcopy(m + CTL_HDR_BYTES, p->p_name, len);
		p->p_name[len] = 0;
		break;
	case CTL_PORT_OPEN:
		if (value != 0)
			p->p_flags |= VIOSER_F_HOST_OPEN;
		else
			p->p_flags &= ~VIOSER_F_HOST_OPEN;
		/* A reader waiting on a host that left sees end of file. */
		SV_BROADCAST(p->p_rsv);
		break;
	default:
		break;
	}
}

/* Consume what the device sent on the control queue.  sc_lock held. */
static int
vs_ctl_rx(struct vioser_softc *sc)
{
	struct virtq *vq = &sc->sc_queues[CTLQ_RX].q_vq;
	struct vs_buf *b;
	vq_u32 len;
	int n = 0;

	while ((b = virtq_get_used(vq, &len)) != 0) {
		dki_dcache_inval(b->b_data, (int)b->b_cap);
		b->b_len = len <= b->b_cap ? len : b->b_cap;
		vs_ctl_handle(sc, b);
		vs_post_rx(sc, CTLQ_RX, b);
		n++;
	}
	if (n != 0)
		vs_kick(sc, CTLQ_RX);
	return n;
}

/*
 * Poll the control queue through DEVICE_READY, DEVICE_ADD and PORT_READY until
 * every port has been answered and the device has gone quiet, so that names
 * have arrived before the device nodes are made.
 */
static int
vs_handshake(struct vioser_softc *sc)
{
	unsigned int i, unanswered;
	int s, ms, err, n, quiet = 0;

	if ((err = vs_ctl_send(sc, 0, CTL_DEVICE_READY, 1)) != 0)
		return err;
	for (ms = 0; ms < VS_HANDSHAKE_MS; ms++) {
		s = LOCK(sc->sc_lock, plhi);
		n = vs_ctl_rx(sc);
		UNLOCK(sc->sc_lock, s);
		quiet = n != 0 ? 0 : quiet + 1;

		unanswered = 0;
		for (i = 0; i < sc->sc_nports; i++) {
			struct vs_port *p = &sc->sc_ports[i];

			if (!p->p_present) {
				unanswered++;
			} else if (!p->p_answered) {
				p->p_answered = 1;
				if ((err = vs_ctl_send(sc, i, CTL_PORT_READY,
				    1)) != 0)
					return err;
				quiet = 0;
			}
		}
		if (unanswered == 0 && quiet >= VS_QUIET_MS)
			return 0;
		us_delay(1000);
	}
	cmn_err(CE_WARN, "vioser: %d of %d ports never announced", unanswered,
	    sc->sc_nports);
	return unanswered == sc->sc_nports ? EIO : 0;
}

/* -------- data path -------- */

/* Collect filled receive buffers and finished transmits.  sc_lock held. */
static void
vs_port_service(struct vioser_softc *sc, struct vs_port *p, int *rx, int *tx)
{
	struct vs_buf *b;
	vq_u32 len;

	*rx = *tx = 0;
	if (p->p_bufs == 0)
		return;
	while ((b = virtq_get_used(&sc->sc_queues[PORT_RXQ(p->p_id)].q_vq,
	    &len)) != 0) {
		dki_dcache_inval(b->b_data, (int)b->b_cap);
		b->b_len = len <= b->b_cap ? len : b->b_cap;
		b->b_off = 0;
		b->b_next = 0;
		if (b->b_len == 0) {
			vs_post_rx(sc, PORT_RXQ(p->p_id), b);
			vs_kick(sc, PORT_RXQ(p->p_id));
			continue;
		}
		if (p->p_rx_tail != 0)
			p->p_rx_tail->b_next = b;
		else
			p->p_rx_head = b;
		p->p_rx_tail = b;
		*rx = 1;
	}
	while ((b = virtq_get_used(&sc->sc_queues[PORT_TXQ(p->p_id)].q_vq,
	    0)) != 0) {
		b->b_next = p->p_tx_free;
		p->p_tx_free = b;
		*tx = 1;
	}
}

static void
vs_service(struct vioser_softc *sc)
{
	unsigned int i;
	int s, rx, tx;
	unsigned int hup, ready, writable;

	s = LOCK(sc->sc_lock, plhi);
	(void)vs_ctl_rx(sc);
	vs_ctl_tx_reclaim(sc);
	hup = ready = writable = 0;
	for (i = 0; i < sc->sc_nports; i++) {
		struct vs_port *p = &sc->sc_ports[i];

		if (!p->p_present)
			continue;
		vs_port_service(sc, p, &rx, &tx);
		if (rx) {
			SV_BROADCAST(p->p_rsv);
			ready |= 1u << i;
		}
		if (tx) {
			SV_BROADCAST(p->p_wsv);
			writable |= 1u << i;
		}
		if (!(p->p_flags & VIOSER_F_HOST_OPEN))
			hup |= 1u << i;
	}
	UNLOCK(sc->sc_lock, s);

	for (i = 0; i < sc->sc_nports; i++) {
		struct vs_port *p = &sc->sc_ports[i];

		if (ready & (1u << i))
			pollwakeup(p->p_ph, POLLIN | POLLRDNORM);
		if (writable & (1u << i))
			pollwakeup(p->p_ph, POLLOUT);
		if (hup & (1u << i))
			pollwakeup(p->p_ph, POLLHUP);
	}
}

static void
vs_intr(void *arg)
{
	struct vioser_softc *sc = arg;

	if (sc->sc_ops->intr_ack(sc->sc_bus) != 0)
		vs_service(sc);
}

static void
vs_poll_tick(struct vioser_softc *sc)
{
	int s;

	vs_service(sc);
	s = LOCK(sc->sc_lock, plhi);
	if (!sc->sc_poll_stop)
		sc->sc_poll_id = itimeout(vs_poll_tick, sc,
		    drv_usectohz(VS_POLL_US), pltimeout);
	UNLOCK(sc->sc_lock, s);
}

/* -------- attach and detach -------- */

/* Publish a port in the hardware graph. */
static int
vs_port_publish(struct vs_port *p)
{
	char path[64];
	vertex_hdl_t dir;
	char *c;

	sprintf(path, "virtio-serial/port%d", p->p_id);
	if (hwgraph_char_device_add(hwgraph_root, path, "vioser_", &p->p_vertex)
	    != GRAPH_SUCCESS)
		return EIO;
	device_info_set(vhdl_to_dev(p->p_vertex), p);
	hwgraph_chmod(p->p_vertex, 0660);

	if (p->p_name[0] == 0)
		return 0;
	/* The name is a single path component. */
	for (c = p->p_name; *c != 0; c++)
		if (*c == '/')
			*c = '_';
	if (hwgraph_path_add(hwgraph_root, "virtio-ports", &dir) !=
	    GRAPH_SUCCESS) {
		cmn_err(CE_WARN, "vioser: no /hw/virtio-ports for %s",
		    p->p_name);
		return 0;
	}
	if (hwgraph_edge_add(dir, p->p_vertex, p->p_name) == GRAPH_SUCCESS)
		p->p_edge = 1;
	hwgraph_vertex_unref(dir);
	return 0;
}

static void
vs_port_unpublish(struct vs_port *p)
{
	vertex_hdl_t dir, parent, v;
	char path[64];

	if (p->p_vertex == GRAPH_VERTEX_NONE || p->p_vertex == 0)
		return;
	if (p->p_edge && hwgraph_traverse(hwgraph_root, "virtio-ports", &dir) ==
	    GRAPH_SUCCESS) {
		(void)hwgraph_edge_remove(dir, p->p_name, &v);
		hwgraph_vertex_unref(dir);
	}
	sprintf(path, "port%d", p->p_id);
	if (hwgraph_traverse(hwgraph_root, "virtio-serial", &parent) ==
	    GRAPH_SUCCESS) {
		(void)hwgraph_edge_remove(parent, path, &v);
		hwgraph_vertex_unref(parent);
	}
	device_info_set(vhdl_to_dev(p->p_vertex), 0);
	(void)hwgraph_vertex_destroy(p->p_vertex);
	p->p_vertex = GRAPH_VERTEX_NONE;
}

static void
vs_teardown(struct vioser_softc *sc)
{
	unsigned int i;
	vertex_hdl_t v;
	toid_t id;
	int s;

	if (sc->sc_polled) {
		/* A tick that already re-armed left its new id here. */
		s = LOCK(sc->sc_lock, plhi);
		sc->sc_poll_stop = 1;
		id = sc->sc_poll_id;
		UNLOCK(sc->sc_lock, s);
		(void)untimeout_wait(id);
	}
	/* A reset stops the device using any ring or buffer. */
	sc->sc_ops->set_status(sc->sc_bus, 0);
	if (sc->sc_intr_connected)
		sc->sc_ops->intr_disconnect(sc->sc_bus, sc);

	for (i = 0; sc->sc_ports != 0 && i < sc->sc_nports; i++) {
		struct vs_port *p = &sc->sc_ports[i];

		vs_port_unpublish(p);
		if (p->p_bufs != 0)
			vs_buf_array_free(p->p_bufs, VS_PORT_BUFS * 2);
		if (p->p_ph != 0)
			phfree(p->p_ph);
		if (p->p_rsv != 0)
			SV_DEALLOC(p->p_rsv);
		if (p->p_wsv != 0)
			SV_DEALLOC(p->p_wsv);
		if (p->p_rmutex != 0)
			MUTEX_DEALLOC(p->p_rmutex);
		if (p->p_wmutex != 0)
			MUTEX_DEALLOC(p->p_wmutex);
	}
	/* The directories, once nothing hangs off them. */
	if (hwgraph_traverse(hwgraph_root, "virtio-ports", &v) == GRAPH_SUCCESS) {
		(void)hwgraph_edge_remove(hwgraph_root, "virtio-ports", 0);
		hwgraph_vertex_unref(v);
		(void)hwgraph_vertex_destroy(v);
	}
	if (hwgraph_traverse(hwgraph_root, "virtio-serial", &v) == GRAPH_SUCCESS) {
		(void)hwgraph_edge_remove(hwgraph_root, "virtio-serial", 0);
		hwgraph_vertex_unref(v);
		(void)hwgraph_vertex_destroy(v);
	}
	if (sc->sc_ports != 0)
		kmem_free(sc->sc_ports, sc->sc_nports * sizeof(*sc->sc_ports));
	for (i = 0; sc->sc_queues != 0 && i < sc->sc_nqueues; i++) {
		if (sc->sc_queues[i].q_rings != 0)
			kmem_free(sc->sc_queues[i].q_rings,
			    sc->sc_queues[i].q_ring_bytes);
	}
	if (sc->sc_queues != 0)
		kmem_free(sc->sc_queues, sc->sc_nqueues * sizeof(*sc->sc_queues));
	if (sc->sc_ctl_bufs != 0)
		vs_buf_array_free(sc->sc_ctl_bufs, VS_CTL_BUFS * 2);
	if (sc->sc_openlock != 0)
		MUTEX_DEALLOC(sc->sc_openlock);
	if (sc->sc_lock != 0)
		LOCK_DEALLOC(sc->sc_lock);
	kmem_free(sc, sizeof(*sc));
}

static int
vs_negotiate(struct vioser_softc *sc)
{
	const struct virtio_bus_ops *ops = sc->sc_ops;
	__uint32_t f0, f1;

	ops->set_status(sc->sc_bus, 0);
	ops->set_status(sc->sc_bus, VIRTIO_S_ACKNOWLEDGE);
	ops->set_status(sc->sc_bus, VIRTIO_S_ACKNOWLEDGE | VIRTIO_S_DRIVER);

	f0 = ops->get_features(sc->sc_bus, 0);
	f1 = ops->get_features(sc->sc_bus, 1);
	if (!(f1 & VIRTIO_F_VERSION_1_HI)) {
		cmn_err(CE_WARN, "vioser: device is not virtio 1.x");
		return ENODEV;
	}
	if (!(f0 & VIRTIO_CONSOLE_F_MULTIPORT)) {
		cmn_err(CE_WARN, "vioser: device has no multiport feature");
		return ENODEV;
	}
	ops->set_features(sc->sc_bus, 0, VIRTIO_CONSOLE_F_MULTIPORT);
	ops->set_features(sc->sc_bus, 1, VIRTIO_F_VERSION_1_HI);
	ops->set_status(sc->sc_bus, VIRTIO_S_ACKNOWLEDGE | VIRTIO_S_DRIVER |
	    VIRTIO_S_FEATURES_OK);
	if (!(ops->get_status(sc->sc_bus) & VIRTIO_S_FEATURES_OK)) {
		cmn_err(CE_WARN, "vioser: device refused our features");
		return ENODEV;
	}
	return 0;
}

static int
vs_attach(const struct virtio_bus_ops *ops, void *bus, void **scp)
{
	struct vioser_softc *sc;
	unsigned int i, nports;
	int err;

	sc = kmem_zalloc(sizeof(*sc), KM_SLEEP);
	sc->sc_ops = ops;
	sc->sc_bus = bus;
	sc->sc_lock = LOCK_ALLOC(1, plhi, 0, KM_SLEEP);
	sc->sc_openlock = MUTEX_ALLOC(MUTEX_DEFAULT, KM_SLEEP, "vioser");

	if ((err = vs_negotiate(sc)) != 0)
		goto fail;

	/* max_nr_ports, little-endian at config offset 4. */
	nports = ops->config_read8(bus, 4) | (ops->config_read8(bus, 5) << 8) |
	    (ops->config_read8(bus, 6) << 16) |
	    (ops->config_read8(bus, 7) << 24);
	if (nports == 0 || nports > VS_MAX_PORTS) {
		cmn_err(CE_WARN, "vioser: device has %d ports", nports);
		err = ENODEV;
		goto fail;
	}
	sc->sc_nports = nports;
	sc->sc_nqueues = 2 + 2 * nports;
	sc->sc_ports = kmem_zalloc(nports * sizeof(*sc->sc_ports), KM_SLEEP);
	sc->sc_queues = kmem_zalloc(sc->sc_nqueues * sizeof(*sc->sc_queues),
	    KM_SLEEP);
	for (i = 0; i < nports; i++) {
		struct vs_port *p = &sc->sc_ports[i];

		p->p_sc = sc;
		p->p_id = i;
		p->p_vertex = GRAPH_VERTEX_NONE;
		p->p_rsv = SV_ALLOC(SV_DEFAULT, KM_SLEEP, "vioserr");
		p->p_wsv = SV_ALLOC(SV_DEFAULT, KM_SLEEP, "vioserw");
		p->p_rmutex = MUTEX_ALLOC(MUTEX_DEFAULT, KM_SLEEP, "viosrm");
		p->p_wmutex = MUTEX_ALLOC(MUTEX_DEFAULT, KM_SLEEP, "viosrw");
		p->p_ph = phalloc(KM_SLEEP);
	}
	for (i = 0; i < sc->sc_nqueues; i++) {
		if ((err = vs_queue_init(sc, i)) != 0)
			goto fail;
	}

	/* VS_CTL_BUFS for each control direction. */
	sc->sc_ctl_bufs = vs_buf_array(sc, VS_CTL_BUFS * 2, VS_CTL_BYTES);
	for (i = 0; i < VS_CTL_BUFS; i++) {
		vs_post_rx(sc, CTLQ_RX, &sc->sc_ctl_bufs[i]);
		sc->sc_ctl_bufs[VS_CTL_BUFS + i].b_next = sc->sc_ctl_tx_free;
		sc->sc_ctl_tx_free = &sc->sc_ctl_bufs[VS_CTL_BUFS + i];
	}

	ops->set_status(bus, VIRTIO_S_ACKNOWLEDGE | VIRTIO_S_DRIVER |
	    VIRTIO_S_FEATURES_OK | VIRTIO_S_DRIVER_OK);
	vs_kick(sc, CTLQ_RX);

	if ((err = vs_handshake(sc)) != 0)
		goto fail;

	sc->sc_attached = 1;
	for (i = 0; i < nports; i++) {
		struct vs_port *p = &sc->sc_ports[i];

		if (!p->p_present)
			continue;
		if ((err = vs_port_publish(p)) != 0)
			goto fail;
		cmn_err(CE_CONT, "vioser: port %d%s%s%s\n", i,
		    p->p_name[0] != 0 ? " " : "", p->p_name,
		    (p->p_flags & VIOSER_F_CONSOLE) ? " (console)" : "");
	}

	err = ops->intr_connect(bus, sc);
	if (err == VIRTIO_NO_INTR) {
		cmn_err(CE_NOTE, "vioser: no interrupt on this slot, polling");
		sc->sc_polled = 1;
		sc->sc_poll_id = itimeout(vs_poll_tick, sc,
		    drv_usectohz(VS_POLL_US), pltimeout);
		*scp = sc;
		return 0;
	}
	if (err != 0)
		goto fail;
	sc->sc_intr_connected = 1;
	/* Interrupts from here on; a sweep for what came before they were on. */
	(void)ops->intr_ack(bus);
	for (i = 0; i < sc->sc_nqueues; i++)
		virtq_set_no_interrupt(&sc->sc_queues[i].q_vq, 0);
	vs_service(sc);
	*scp = sc;
	return 0;

fail:
	ops->set_status(bus, VIRTIO_S_FAILED);
	vs_teardown(sc);
	return err;
}

static int
vs_detach(void *arg)
{
	struct vioser_softc *sc = arg;
	unsigned int i;

	MUTEX_LOCK(sc->sc_openlock, -1);
	for (i = 0; i < sc->sc_nports; i++) {
		if (sc->sc_ports[i].p_open) {
			MUTEX_UNLOCK(sc->sc_openlock);
			return EBUSY;
		}
	}
	MUTEX_UNLOCK(sc->sc_openlock);
	vs_teardown(sc);
	return 0;
}

/* -------- character device -------- */

static struct vs_port *
vs_port_of(dev_t dev)
{
	return (struct vs_port *)device_info_get(dev);
}

/* First open of a port: its data buffers, half for each direction. */
static void
vs_port_alloc_bufs(struct vioser_softc *sc, struct vs_port *p)
{
	struct vs_buf *a;
	unsigned int i;
	int s;

	a = vs_buf_array(sc, VS_PORT_BUFS * 2, VS_BUF_BYTES);
	s = LOCK(sc->sc_lock, plhi);
	p->p_bufs = a;
	for (i = 0; i < VS_PORT_BUFS; i++) {
		vs_post_rx(sc, PORT_RXQ(p->p_id), &a[i]);
		a[VS_PORT_BUFS + i].b_next = p->p_tx_free;
		p->p_tx_free = &a[VS_PORT_BUFS + i];
	}
	vs_kick(sc, PORT_RXQ(p->p_id));
	UNLOCK(sc->sc_lock, s);
}

/*
 * Give back what a program did not read.  Run at close, and again at open in
 * case the device delivered more before it saw the close.
 */
static void
vs_port_discard_rx(struct vioser_softc *sc, struct vs_port *p)
{
	struct vs_buf *b;
	int s, posted = 0;

	s = LOCK(sc->sc_lock, plhi);
	while ((b = p->p_rx_head) != 0) {
		p->p_rx_head = b->b_next;
		vs_post_rx(sc, PORT_RXQ(p->p_id), b);
		posted = 1;
	}
	p->p_rx_tail = 0;
	if (posted)
		vs_kick(sc, PORT_RXQ(p->p_id));
	UNLOCK(sc->sc_lock, s);
}

/*ARGSUSED*/
int
vioser_open(dev_t *devp, int flag, int otyp, cred_t *crp)
{
	struct vs_port *p = vs_port_of(*devp);
	struct vioser_softc *sc;
	int err;

	if (p == 0)
		return ENODEV;
	sc = p->p_sc;
	MUTEX_LOCK(sc->sc_openlock, -1);
	if (p->p_open) {
		MUTEX_UNLOCK(sc->sc_openlock);
		return EBUSY;
	}
	if (p->p_bufs == 0)
		vs_port_alloc_bufs(sc, p);
	vs_port_discard_rx(sc, p);
	if ((err = vs_ctl_send(sc, p->p_id, CTL_PORT_OPEN, 1)) == 0)
		p->p_open = 1;
	MUTEX_UNLOCK(sc->sc_openlock);
	return err;
}

/*ARGSUSED*/
int
vioser_close(dev_t dev, int flag, int otyp, cred_t *crp)
{
	struct vs_port *p = vs_port_of(dev);
	struct vioser_softc *sc;

	if (p == 0)
		return ENODEV;
	sc = p->p_sc;
	MUTEX_LOCK(sc->sc_openlock, -1);
	(void)vs_ctl_send(sc, p->p_id, CTL_PORT_OPEN, 0);
	vs_port_discard_rx(sc, p);
	p->p_open = 0;
	MUTEX_UNLOCK(sc->sc_openlock);
	return 0;
}

/*ARGSUSED*/
int
vioser_read(dev_t dev, uio_t *uio, cred_t *crp)
{
	struct vs_port *p = vs_port_of(dev);
	struct vioser_softc *sc;
	struct vs_buf *b;
	size_t n;
	int s, err = 0, moved = 0;

	if (p == 0)
		return ENODEV;
	sc = p->p_sc;
	MUTEX_LOCK(p->p_rmutex, -1);
	while (uio->uio_resid > 0) {
		s = LOCK(sc->sc_lock, plhi);
		while ((b = p->p_rx_head) == 0) {
			/* Never sleep with bytes to return. */
			if (moved || !(p->p_flags & VIOSER_F_HOST_OPEN)) {
				UNLOCK(sc->sc_lock, s);
				goto out;
			}
			if (uio->uio_fmode & (FNONBLOCK | FNDELAY)) {
				UNLOCK(sc->sc_lock, s);
				err = EAGAIN;
				goto out;
			}
			if (!SV_WAIT_SIG(p->p_rsv, sc->sc_lock, s)) {
				err = EINTR;
				goto out;
			}
			s = LOCK(sc->sc_lock, plhi);
		}
		UNLOCK(sc->sc_lock, s);

		/* This is the only reader, and the handler only appends. */
		n = b->b_len - b->b_off;
		if (n > uio->uio_resid)
			n = uio->uio_resid;
		if ((err = uiomove(b->b_data + b->b_off, n, UIO_READ, uio)) != 0)
			break;
		moved = 1;
		b->b_off += n;
		if (b->b_off == b->b_len) {
			s = LOCK(sc->sc_lock, plhi);
			p->p_rx_head = b->b_next;
			if (p->p_rx_head == 0)
				p->p_rx_tail = 0;
			vs_post_rx(sc, PORT_RXQ(p->p_id), b);
			vs_kick(sc, PORT_RXQ(p->p_id));
			UNLOCK(sc->sc_lock, s);
		}
	}
out:
	MUTEX_UNLOCK(p->p_rmutex);
	return moved ? 0 : err;
}

/*ARGSUSED*/
int
vioser_write(dev_t dev, uio_t *uio, cred_t *crp)
{
	struct vs_port *p = vs_port_of(dev);
	struct vioser_softc *sc;
	struct vs_buf *b;
	size_t n;
	int s, err = 0, moved = 0;

	if (p == 0)
		return ENODEV;
	sc = p->p_sc;
	MUTEX_LOCK(p->p_wmutex, -1);
	while (uio->uio_resid > 0) {
		s = LOCK(sc->sc_lock, plhi);
		while ((b = p->p_tx_free) == 0) {
			if (uio->uio_fmode & (FNONBLOCK | FNDELAY)) {
				UNLOCK(sc->sc_lock, s);
				err = EAGAIN;
				goto out;
			}
			if (!SV_WAIT_SIG(p->p_wsv, sc->sc_lock, s)) {
				err = EINTR;
				goto out;
			}
			s = LOCK(sc->sc_lock, plhi);
		}
		p->p_tx_free = b->b_next;
		UNLOCK(sc->sc_lock, s);

		n = uio->uio_resid;
		if (n > b->b_cap)
			n = b->b_cap;
		err = uiomove(b->b_data, n, UIO_WRITE, uio);
		s = LOCK(sc->sc_lock, plhi);
		if (err != 0) {
			b->b_next = p->p_tx_free;
			p->p_tx_free = b;
			UNLOCK(sc->sc_lock, s);
			break;
		}
		moved = 1;
		dki_dcache_wb(b->b_data, (int)n);
		(void)virtq_add(&sc->sc_queues[PORT_TXQ(p->p_id)].q_vq, b->b_bus,
		    (vq_u32)n, 0, b);
		vs_kick(sc, PORT_TXQ(p->p_id));
		UNLOCK(sc->sc_lock, s);
	}
out:
	MUTEX_UNLOCK(p->p_wmutex);
	return moved ? 0 : err;
}

/*ARGSUSED*/
int
vioser_ioctl(dev_t dev, int cmd, void *arg, int mode, cred_t *crp, int *rvalp)
{
	struct vs_port *p = vs_port_of(dev);
	struct vioser_softc *sc;
	struct vioser_info info;
	struct vs_buf *b;
	int s, avail;

	if (p == 0)
		return ENODEV;
	sc = p->p_sc;
	switch (cmd) {
	case VIOSER_GETINFO:
		bzero(&info, sizeof(info));
		info.vi_port = p->p_id;
		s = LOCK(sc->sc_lock, plhi);
		info.vi_flags = p->p_flags;
		UNLOCK(sc->sc_lock, s);
		bcopy(p->p_name, info.vi_name, sizeof(info.vi_name));
		return copyout(&info, arg, sizeof(info)) ? EFAULT : 0;
	case FIONREAD:
		avail = 0;
		s = LOCK(sc->sc_lock, plhi);
		for (b = p->p_rx_head; b != 0; b = b->b_next)
			avail += b->b_len - b->b_off;
		UNLOCK(sc->sc_lock, s);
		return copyout(&avail, arg, sizeof(avail)) ? EFAULT : 0;
	default:
		return ENOTTY;
	}
}

int
vioser_poll(dev_t dev, short events, int anyyet, short *reventsp,
	    struct pollhead **phpp, unsigned int *genp)
{
	struct vs_port *p = vs_port_of(dev);
	struct vioser_softc *sc;
	unsigned int gen;
	short revents = 0;
	int s;

	if (p == 0)
		return ENODEV;
	sc = p->p_sc;
	/* Taken before looking, so a wakeup after the look is not missed. */
	gen = POLLGEN(p->p_ph);
	s = LOCK(sc->sc_lock, plhi);
	if ((events & (POLLIN | POLLRDNORM)) && p->p_rx_head != 0)
		revents |= events & (POLLIN | POLLRDNORM);
	if ((events & POLLOUT) && p->p_tx_free != 0)
		revents |= POLLOUT;
	if (!(p->p_flags & VIOSER_F_HOST_OPEN))
		revents |= POLLHUP;
	UNLOCK(sc->sc_lock, s);

	*reventsp = revents;
	if (revents == 0 && !anyyet) {
		*phpp = p->p_ph;
		*genp = gen;
	}
	return 0;
}

/* -------- module load and unload -------- */

static const struct virtio_driver vs_driver = {
	"vioser",
	VIRTIO_ID_CONSOLE,
	vs_attach,
	vs_detach,
	vs_intr,
};

#ifdef IP30
extern int virtio_pci_attach(vertex_hdl_t);

/* The kernel calls this for each function the driver registered for. */
int
vioser_attach(vertex_hdl_t conn)
{
	return virtio_pci_attach(conn);
}
#endif

/* The driver is loaded with ml(1M); see driver/vioser.init. */
void
vioser_init(void)
{
	int err;

	if ((err = virtio_bus_init(&vs_driver, "vioser_")) != 0 && err != ENODEV)
		cmn_err(CE_WARN, "vioser: attach failed, error %d", err);
}

int
vioser_unload(void)
{
	return virtio_bus_fini("vioser_");
}
