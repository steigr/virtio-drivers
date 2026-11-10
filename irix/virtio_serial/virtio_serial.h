/*
 * virtio-serial for IRIX: the interface user programs see.
 *
 * Each port is a character device, /hw/virtio-serial/port<N>, and a named port
 * is also /hw/virtio-ports/<name>.  One program can have a port open at a time.
 *
 * read() blocks for data and returns 0 when the host end is not connected.
 * write() blocks for a free buffer.  poll() reports POLLIN, POLLOUT and POLLHUP.
 * Bytes pass through untouched: a port is not a tty.
 */
#ifndef _VIOSER_H_
#define _VIOSER_H_

#include <sys/ioctl.h>

#define VIOSER_NAME_MAX		64

#define VIOSER_F_CONSOLE	0x1	/* the device calls this port its console */
#define VIOSER_F_HOST_OPEN	0x2	/* a program is connected to the host end */

struct vioser_info {
	unsigned int	vi_port;
	unsigned int	vi_flags;
	char		vi_name[VIOSER_NAME_MAX];	/* "" for an unnamed port */
};

#define VIOSER_GETINFO	_IOR('V', 1, struct vioser_info)

#endif /* _VIOSER_H_ */
