/*
 * What the virtio-serial core (vioser.c) and the bus transports share.
 *
 * A transport owns how the device is reached (virtio-mmio on GIO, virtio-pci
 * on the Bridge); the core owns the queues and ports and never touches a
 * register.  Transports give the core a vioser_bus and call vioser_core_attach.
 */
#ifndef _VIOSER_IMPL_H_
#define _VIOSER_IMPL_H_

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

struct vioser_softc;

#define VIOSER_NO_INTR		(-1)

struct vioser_bus_ops {
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
	 * Route the device's interrupt to vioser_intr(sc), or stop doing so.
	 * VIOSER_NO_INTR means this slot's line reaches no handler: the core
	 * keeps the device from raising it and polls instead.
	 */
	int		(*intr_connect)(void *bus, struct vioser_softc *sc);
	void		(*intr_disconnect)(void *bus, struct vioser_softc *sc);
};

/* Bring a device up and publish its ports.  Returns 0 or an errno. */
int	vioser_core_attach(const struct vioser_bus_ops *ops, void *bus,
	    struct vioser_softc **scp);
/* Undo vioser_core_attach.  Returns EBUSY while any port is open. */
int	vioser_core_detach(struct vioser_softc *sc);
/* The transport's interrupt handler calls this. */
void	vioser_intr(struct vioser_softc *sc);

/* Implemented by the transport this kernel was built with. */
int	vioser_bus_init(void);
int	vioser_bus_fini(void);

#endif /* _VIOSER_IMPL_H_ */
