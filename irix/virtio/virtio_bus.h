/*
 * What a virtio driver and the bus transports share.
 *
 * A transport owns how the device is reached (virtio-mmio on GIO, virtio-pci
 * on the Bridge); a driver owns the queues and never touches a register.  One
 * module holds one driver and one transport, chosen by the board it is built
 * for: the driver names itself in a virtio_driver, the transport finds that
 * device and calls attach.
 */
#ifndef _VIRTIO_BUS_H_
#define _VIRTIO_BUS_H_

#include <sys/types.h>

/* Device status bits (virtio 1.x 2.1). */
#define VIRTIO_S_ACKNOWLEDGE	0x01
#define VIRTIO_S_DRIVER		0x02
#define VIRTIO_S_DRIVER_OK	0x04
#define VIRTIO_S_FEATURES_OK	0x08
#define VIRTIO_S_FAILED		0x80

/* Interrupt status bits, common to virtio-mmio and virtio-pci. */
#define VIRTIO_ISR_QUEUE	0x1
#define VIRTIO_ISR_CONFIG	0x2

#define VIRTIO_ID_CONSOLE	3
#define VIRTIO_ID_9P		9

#define VIRTIO_NO_INTR		(-1)

struct virtio_bus_ops {
	/* Feature words: sel 0 is bits 0..31, sel 1 is bits 32..63. */
	__uint32_t	(*get_features)(void *bus, unsigned int sel);
	void		(*set_features)(void *bus, unsigned int sel, __uint32_t val);
	unsigned int	(*get_status)(void *bus);
	void		(*set_status)(void *bus, unsigned int status);
	/* Largest ring queue q takes; 0 if the queue does not exist. */
	unsigned int	(*queue_max)(void *bus, unsigned int q);
	/* Program queue q's rings, given as bus addresses, and enable it. */
	int		(*queue_setup)(void *bus, unsigned int q, unsigned int num,
			    __uint64_t desc, __uint64_t avail, __uint64_t used);
	void		(*notify)(void *bus, unsigned int q);
	/* One byte of device config space. */
	unsigned int	(*config_read8)(void *bus, unsigned int off);
	/* Read and acknowledge the interrupt status: VIRTIO_ISR_* bits. */
	unsigned int	(*intr_ack)(void *bus);
	/* The bus address the device uses for this kernel memory. */
	__uint64_t	(*dma_addr)(void *bus, void *kva, size_t len);
	/*
	 * Route the device's interrupt to the driver's intr(sc), or stop doing
	 * so.  VIRTIO_NO_INTR means this slot's line reaches no handler: the
	 * driver keeps the device from raising it and polls instead.
	 */
	int		(*intr_connect)(void *bus, void *sc);
	void		(*intr_disconnect)(void *bus, void *sc);
};

struct virtio_driver {
	const char	*vd_name;		/* for messages */
	unsigned int	vd_device_id;
	/* Bring a device up.  Returns 0 or an errno. */
	int		(*vd_attach)(const struct virtio_bus_ops *ops, void *bus,
			    void **scp);
	/* Undo vd_attach.  Returns EBUSY while the device is in use. */
	int		(*vd_detach)(void *sc);
	/* The transport's interrupt handler calls this. */
	void		(*vd_intr)(void *sc);
};

/*
 * Implemented by the transport this kernel was built with: virtio_bus_init
 * finds the driver's devices (GIO slots, or PCI functions in the hwgraph) and
 * attaches them; ENODEV when there is none.  pci_prefix is unused.
 */
int	virtio_bus_init(const struct virtio_driver *drv, char *pci_prefix);
int	virtio_bus_fini(char *pci_prefix);

#endif /* _VIRTIO_BUS_H_ */
