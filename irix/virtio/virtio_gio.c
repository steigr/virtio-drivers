/*
 * virtio-mmio v2 transport, for machines with a GIO bus and no PCI: Indy
 * (IP24, built as IP22), Indigo2 (IP22) and Indigo2 IMPACT (IP28).
 *
 * The device sits in a GIO expansion slot.  Registers are 32-bit numbers read
 * with lw and sw; config space, from 0x100, is little-endian bytes read one at
 * a time.  A slot with nothing in it answers with a bus error, which badaddr
 * turns into a failed probe.
 */
#if defined(IP22) || defined(IP28)

#include <sys/types.h>
#include <sys/cmn_err.h>
#include <sys/debug.h>
#include <sys/errno.h>
#include <sys/kmem.h>
#include <sys/systm.h>
#include <sys/ddi.h>
#include <sys/sbd.h>
#include <sys/immu.h>
#include <sys/mc.h>
#include <sys/IP22.h>

#include "virtio_bus.h"

/*
 * The Indigo2's GIO fan-out handlers (ip22_gio{0,1,2}_intr) dispatch the
 * graphics slot and slot 0 only: IP22.h has GIO_SLOT_1 as "Indy only", and
 * IP28's ip22_gio1_intr tests only EXTIO_SG_IRQ_1 and EXTIO_S0_IRQ_1.
 */
#ifdef IP28
#define SLOT1_HAS_INTERRUPT	0
#else
#define SLOT1_HAS_INTERRUPT	(!is_fullhouse())
#endif

#define MMIO_MAGIC		0x000
#define MMIO_VERSION		0x004
#define MMIO_DEVICE_ID		0x008
#define MMIO_DEVICE_FEATURES	0x010
#define MMIO_DEVICE_FEATURES_SEL 0x014
#define MMIO_DRIVER_FEATURES	0x020
#define MMIO_DRIVER_FEATURES_SEL 0x024
#define MMIO_QUEUE_SEL		0x030
#define MMIO_QUEUE_NUM_MAX	0x034
#define MMIO_QUEUE_NUM		0x038
#define MMIO_QUEUE_READY	0x044
#define MMIO_QUEUE_NOTIFY	0x050
#define MMIO_INTERRUPT_STATUS	0x060
#define MMIO_INTERRUPT_ACK	0x064
#define MMIO_STATUS		0x070
#define MMIO_QUEUE_DESC_LOW	0x080
#define MMIO_QUEUE_DESC_HIGH	0x084
#define MMIO_QUEUE_DRIVER_LOW	0x090
#define MMIO_QUEUE_DRIVER_HIGH	0x094
#define MMIO_QUEUE_DEVICE_LOW	0x0a0
#define MMIO_QUEUE_DEVICE_HIGH	0x0a4
#define MMIO_CONFIG		0x100

#define MMIO_MAGIC_VALUE	0x74726976	/* "virt" */

/* The physical bases of the GIO expansion slots, with their IRIX slot numbers. */
static struct {
	__uint32_t	phys;
	int		slot;
} gio_slots[] = {
	{ 0x1f400000, GIO_SLOT_0 },
	{ 0x1f600000, GIO_SLOT_1 },
};
#define NGIO_SLOTS	(sizeof(gio_slots) / sizeof(gio_slots[0]))

/*
 * An Indigo2 expansion slot's device drives the GIO_INTERRUPT_1 fan-out.  An Indy
 * expansion slot has one line; that setgiovector takes it at this level has not
 * been run.
 */
#define GIO_LEVEL	GIO_INTERRUPT_1

struct gio_bus {
	volatile unsigned char	*base;
	int			slot;
};

static struct gio_bus			gio_bus;
static const struct virtio_driver	*gio_drv;
static void				*gio_sc;

#define R32(b, off) \
	(*(volatile __uint32_t *)((b)->base + (off)))

static __uint32_t
gio_get_features(void *bus, unsigned int sel)
{
	struct gio_bus *b = bus;

	R32(b, MMIO_DEVICE_FEATURES_SEL) = sel;
	return R32(b, MMIO_DEVICE_FEATURES);
}

static void
gio_set_features(void *bus, unsigned int sel, __uint32_t val)
{
	struct gio_bus *b = bus;

	R32(b, MMIO_DRIVER_FEATURES_SEL) = sel;
	R32(b, MMIO_DRIVER_FEATURES) = val;
}

static unsigned int
gio_get_status(void *bus)
{
	struct gio_bus *b = bus;

	return R32(b, MMIO_STATUS);
}

static void
gio_set_status(void *bus, unsigned int status)
{
	struct gio_bus *b = bus;

	R32(b, MMIO_STATUS) = status;
}

static unsigned int
gio_queue_max(void *bus, unsigned int q)
{
	struct gio_bus *b = bus;

	R32(b, MMIO_QUEUE_SEL) = q;
	return R32(b, MMIO_QUEUE_NUM_MAX);
}

static int
gio_queue_setup(void *bus, unsigned int q, unsigned int num, __uint64_t desc,
		__uint64_t avail, __uint64_t used)
{
	struct gio_bus *b = bus;

	R32(b, MMIO_QUEUE_SEL) = q;
	R32(b, MMIO_QUEUE_NUM) = num;
	R32(b, MMIO_QUEUE_DESC_LOW) = (__uint32_t)desc;
	R32(b, MMIO_QUEUE_DESC_HIGH) = (__uint32_t)(desc >> 32);
	R32(b, MMIO_QUEUE_DRIVER_LOW) = (__uint32_t)avail;
	R32(b, MMIO_QUEUE_DRIVER_HIGH) = (__uint32_t)(avail >> 32);
	R32(b, MMIO_QUEUE_DEVICE_LOW) = (__uint32_t)used;
	R32(b, MMIO_QUEUE_DEVICE_HIGH) = (__uint32_t)(used >> 32);
	R32(b, MMIO_QUEUE_READY) = 1;
	return R32(b, MMIO_QUEUE_READY) == 1 ? 0 : EIO;
}

static void
gio_notify(void *bus, unsigned int q)
{
	struct gio_bus *b = bus;

	R32(b, MMIO_QUEUE_NOTIFY) = q;
}

static unsigned int
gio_config_read8(void *bus, unsigned int off)
{
	struct gio_bus *b = bus;

	return b->base[MMIO_CONFIG + off];
}

static unsigned int
gio_intr_ack(void *bus)
{
	struct gio_bus *b = bus;
	__uint32_t isr;

	isr = R32(b, MMIO_INTERRUPT_STATUS);
	if (isr != 0) {
		R32(b, MMIO_INTERRUPT_ACK) = isr;
		flushbus();
	}
	return isr;
}

/*ARGSUSED*/
static __uint64_t
gio_dma_addr(void *bus, void *kva, size_t len)
{
	return (__uint64_t)kvtophys(kva);
}

/*ARGSUSED*/
static void
gio_intr(__psint_t arg, struct eframe_s *ep)
{
	gio_drv->vd_intr((void *)arg);
}

static int
gio_intr_connect(void *bus, void *sc)
{
	struct gio_bus *b = bus;

	if (b->slot == GIO_SLOT_1 && !SLOT1_HAS_INTERRUPT)
		return VIRTIO_NO_INTR;
	setgiovector(GIO_LEVEL, b->slot, gio_intr, (__psint_t)sc);
	return 0;
}

/*ARGSUSED*/
static void
gio_intr_disconnect(void *bus, void *sc)
{
	struct gio_bus *b = bus;

	setgiovector(GIO_LEVEL, b->slot, 0, 0);
}

static const struct virtio_bus_ops gio_ops = {
	gio_get_features,
	gio_set_features,
	gio_get_status,
	gio_set_status,
	gio_queue_max,
	gio_queue_setup,
	gio_notify,
	gio_config_read8,
	gio_intr_ack,
	gio_dma_addr,
	gio_intr_connect,
	gio_intr_disconnect,
};

/* Whether a virtio-mmio v2 device of this kind answers at physical address phys. */
static int
gio_probe(__uint32_t phys, unsigned int id, volatile unsigned char **basep)
{
	volatile unsigned char *base;
	__uint32_t magic;

	base = (volatile unsigned char *)PHYS_TO_K1(phys);
	if (badaddr_val(base + MMIO_MAGIC, 4, &magic) || magic != MMIO_MAGIC_VALUE)
		return 0;
	if (*(volatile __uint32_t *)(base + MMIO_VERSION) != 2)
		return 0;
	if (*(volatile __uint32_t *)(base + MMIO_DEVICE_ID) != id)
		return 0;
	*basep = base;
	return 1;
}

/*ARGSUSED*/
int
virtio_bus_init(const struct virtio_driver *drv, char *pci_prefix)
{
	unsigned int i;

	gio_drv = drv;
	for (i = 0; i < NGIO_SLOTS; i++) {
		if (!gio_probe(gio_slots[i].phys, drv->vd_device_id, &gio_bus.base))
			continue;
		gio_bus.slot = gio_slots[i].slot;
		cmn_err(CE_CONT, "%s: virtio-mmio device at 0x%x\n", drv->vd_name,
		    gio_slots[i].phys);
		return drv->vd_attach(&gio_ops, &gio_bus, &gio_sc);
	}
	return ENODEV;
}

/*ARGSUSED*/
int
virtio_bus_fini(char *pci_prefix)
{
	int err;

	if (gio_sc == 0)
		return 0;
	err = gio_drv->vd_detach(gio_sc);
	if (err == 0)
		gio_sc = 0;
	return err;
}

#endif /* IP22 || IP28 */
