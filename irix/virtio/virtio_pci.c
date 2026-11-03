/*
 * virtio-pci (modern) transport, for the Octane (IP30): vendor 0x1af4, device
 * 0x1040 + the virtio device ID, on the Bridge.
 *
 * The four structures are found by walking the vendor capabilities.  PIO is
 * mapped as word values, so a 32-bit access reads the device's number; a
 * narrower access has to aim at the byte lane a big-endian CPU drives, hence
 * the XORs in the accessors below.  Rings and buffers are byte streams.
 *
 * Compile-tested only: there has been no IP30 IRIX system to run it on.
 */
#ifdef IP30

#include <sys/types.h>
#include <sys/cmn_err.h>
#include <sys/ddi.h>
#include <sys/debug.h>
#include <sys/errno.h>
#include <sys/hwgraph.h>
#include <sys/iobus.h>
#include <sys/iograph.h>
#include <sys/kmem.h>
#include <sys/systm.h>
#include <sys/PCI/PCI_defs.h>
#include <sys/PCI/pciio.h>

#include "virtio_bus.h"

#define VIRTIO_PCI_VENDOR	0x1af4
#define VIRTIO_PCI_MODERN_BASE	0x1040

#define PCI_CAP_VENDOR		0x09
/* cfg_type in a virtio_pci_cap. */
#define VIRTIO_PCI_CAP_COMMON	1
#define VIRTIO_PCI_CAP_NOTIFY	2
#define VIRTIO_PCI_CAP_ISR	3
#define VIRTIO_PCI_CAP_DEVICE	4

/* struct virtio_pci_common_cfg */
#define CC_DF_SEL	0x00
#define CC_DF		0x04
#define CC_GF_SEL	0x08
#define CC_GF		0x0c
#define CC_NUM_QUEUES	0x12
#define CC_STATUS	0x14
#define CC_Q_SELECT	0x16
#define CC_Q_SIZE	0x18
#define CC_Q_ENABLE	0x1c
#define CC_Q_NOTIFY_OFF	0x1e
#define CC_Q_DESC	0x20
#define CC_Q_AVAIL	0x28
#define CC_Q_USED	0x30

struct pci_bus {
	vertex_hdl_t		conn;
	pciio_intr_t		intr;
	volatile unsigned char	*common;
	volatile unsigned char	*isr;
	volatile unsigned char	*device;
	volatile unsigned char	*notify;
	__uint32_t		notify_mult;
	void			*sc;
	struct pci_bus		*next;
};

static struct pci_bus *pci_buses;
static const struct virtio_driver *pci_drv;

#define R32(p, off)	(*(volatile __uint32_t *)((p) + (off)))
#define R16(p, off)	(*(volatile unsigned short *)((p) + ((off) ^ 2)))
#define R8(p, off)	(*(volatile unsigned char *)((p) + ((off) ^ 3)))

static __uint32_t
pci_get_features(void *bus, unsigned int sel)
{
	struct pci_bus *b = bus;

	R32(b->common, CC_DF_SEL) = sel;
	return R32(b->common, CC_DF);
}

static void
pci_set_features(void *bus, unsigned int sel, __uint32_t val)
{
	struct pci_bus *b = bus;

	R32(b->common, CC_GF_SEL) = sel;
	R32(b->common, CC_GF) = val;
}

static unsigned int
pci_get_status(void *bus)
{
	struct pci_bus *b = bus;

	return R8(b->common, CC_STATUS);
}

static void
pci_set_status(void *bus, unsigned int status)
{
	struct pci_bus *b = bus;

	R8(b->common, CC_STATUS) = (unsigned char)status;
}

static unsigned int
pci_queue_max(void *bus, unsigned int q)
{
	struct pci_bus *b = bus;

	if (q >= R16(b->common, CC_NUM_QUEUES))
		return 0;
	R16(b->common, CC_Q_SELECT) = (unsigned short)q;
	return R16(b->common, CC_Q_SIZE);
}

static int
pci_queue_setup(void *bus, unsigned int q, unsigned int num, __uint64_t desc,
		__uint64_t avail, __uint64_t used)
{
	struct pci_bus *b = bus;

	R16(b->common, CC_Q_SELECT) = (unsigned short)q;
	R16(b->common, CC_Q_SIZE) = (unsigned short)num;
	R32(b->common, CC_Q_DESC) = (__uint32_t)desc;
	R32(b->common, CC_Q_DESC + 4) = (__uint32_t)(desc >> 32);
	R32(b->common, CC_Q_AVAIL) = (__uint32_t)avail;
	R32(b->common, CC_Q_AVAIL + 4) = (__uint32_t)(avail >> 32);
	R32(b->common, CC_Q_USED) = (__uint32_t)used;
	R32(b->common, CC_Q_USED + 4) = (__uint32_t)(used >> 32);
	R16(b->common, CC_Q_ENABLE) = 1;
	return 0;
}

static void
pci_notify(void *bus, unsigned int q)
{
	struct pci_bus *b = bus;
	unsigned int off;

	R16(b->common, CC_Q_SELECT) = (unsigned short)q;
	off = R16(b->common, CC_Q_NOTIFY_OFF) * b->notify_mult;
	R16(b->notify, off) = (unsigned short)q;
}

static unsigned int
pci_config_read8(void *bus, unsigned int off)
{
	struct pci_bus *b = bus;

	return R8(b->device, off);
}

static unsigned int
pci_intr_ack(void *bus)
{
	struct pci_bus *b = bus;

	/* Reading the ISR clears it. */
	return R8(b->isr, 0);
}

static __uint64_t
pci_dma_addr(void *bus, void *kva, size_t len)
{
	struct pci_bus *b = bus;

	return (__uint64_t)pciio_dmatrans_addr(b->conn, 0, kvtophys(kva), len,
	    PCIIO_DMA_DATA | PCIIO_BYTE_STREAM | PCIIO_DMA_A64);
}

static void
pci_intr(intr_arg_t arg)
{
	struct pci_bus *b = (struct pci_bus *)arg;

	/* The line is shared: only act on an interrupt that was ours. */
	if (b->sc != 0)
		pci_drv->vd_intr(b->sc);
}

static int
pci_intr_connect(void *bus, void *sc)
{
	struct pci_bus *b = bus;

	b->sc = sc;
	b->intr = pciio_intr_alloc(b->conn, 0, PCIIO_INTR_LINE_A, b->conn);
	if (b->intr == 0)
		return EIO;
	if (pciio_intr_connect(b->intr, pci_intr, (intr_arg_t)b, 0) != 0) {
		pciio_intr_free(b->intr);
		b->intr = 0;
		return EIO;
	}
	return 0;
}

/*ARGSUSED*/
static void
pci_intr_disconnect(void *bus, void *sc)
{
	struct pci_bus *b = bus;

	if (b->intr != 0) {
		pciio_intr_disconnect(b->intr);
		pciio_intr_free(b->intr);
		b->intr = 0;
	}
	b->sc = 0;
}

static const struct virtio_bus_ops pci_ops = {
	pci_get_features,
	pci_set_features,
	pci_get_status,
	pci_set_status,
	pci_queue_max,
	pci_queue_setup,
	pci_notify,
	pci_config_read8,
	pci_intr_ack,
	pci_dma_addr,
	pci_intr_connect,
	pci_intr_disconnect,
};

static volatile unsigned char *
pci_map(vertex_hdl_t conn, unsigned int bar, unsigned int off, unsigned int len)
{
	return (volatile unsigned char *)pciio_piotrans_addr(conn, 0,
	    PCIIO_SPACE_WIN(bar), off, len, PCIIO_WORD_VALUES);
}

/* Walk the capability list for the four virtio structures. */
static int
pci_find_caps(struct pci_bus *b)
{
	vertex_hdl_t conn = b->conn;
	unsigned int cap, id, type, bar, off, len;
	int guard;

	if (!(pciio_config_get(conn, PCI_CFG_STATUS, 2) & PCI_STAT_CAP_LIST))
		return ENODEV;
	cap = pciio_config_get(conn, PCI_CAPABILITIES_PTR, 1) & 0xfc;
	for (guard = 0; cap != 0 && guard < 48; guard++) {
		id = pciio_config_get(conn, cap, 1);
		if (id == PCI_CAP_VENDOR) {
			type = pciio_config_get(conn, cap + 3, 1);
			bar = pciio_config_get(conn, cap + 4, 1);
			off = pciio_config_get(conn, cap + 8, 4);
			len = pciio_config_get(conn, cap + 12, 4);
			switch (type) {
			case VIRTIO_PCI_CAP_COMMON:
				b->common = pci_map(conn, bar, off, len);
				break;
			case VIRTIO_PCI_CAP_NOTIFY:
				b->notify = pci_map(conn, bar, off, len);
				b->notify_mult =
				    pciio_config_get(conn, cap + 16, 4);
				break;
			case VIRTIO_PCI_CAP_ISR:
				b->isr = pci_map(conn, bar, off, len);
				break;
			case VIRTIO_PCI_CAP_DEVICE:
				b->device = pci_map(conn, bar, off, len);
				break;
			default:
				break;
			}
		}
		cap = pciio_config_get(conn, cap + 1, 1) & 0xfc;
	}
	if (b->common == 0 || b->notify == 0 || b->isr == 0 || b->device == 0)
		return ENODEV;
	return 0;
}

/* The driver's prefix "attach" entry point calls this for each function. */
int
virtio_pci_attach(vertex_hdl_t conn)
{
	struct pci_bus *b;
	void *sc;
	__uint32_t cmd;
	int err;

	b = kmem_zalloc(sizeof(*b), KM_SLEEP);
	b->conn = conn;
	cmd = pciio_config_get(conn, PCI_CFG_COMMAND, 2);
	pciio_config_set(conn, PCI_CFG_COMMAND, 2,
	    cmd | PCI_CMD_MEM_SPACE | PCI_CMD_BUS_MASTER);
	if ((err = pci_find_caps(b)) != 0 ||
	    (err = pci_drv->vd_attach(&pci_ops, b, &sc)) != 0) {
		kmem_free(b, sizeof(*b));
		return err;
	}
	b->sc = sc;
	b->next = pci_buses;
	pci_buses = b;
	return 0;
}

int
virtio_bus_init(const struct virtio_driver *drv, char *pci_prefix)
{
	pci_drv = drv;
	return pciio_driver_register(VIRTIO_PCI_VENDOR,
	    VIRTIO_PCI_MODERN_BASE + drv->vd_device_id, pci_prefix, 0);
}

int
virtio_bus_fini(char *pci_prefix)
{
	struct pci_bus *b;
	int err;

	while ((b = pci_buses) != 0) {
		if ((err = pci_drv->vd_detach(b->sc)) != 0)
			return err;
		pci_buses = b->next;
		kmem_free(b, sizeof(*b));
	}
	pciio_driver_unregister(pci_prefix);
	return 0;
}

#endif /* IP30 */
