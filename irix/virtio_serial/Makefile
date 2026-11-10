#!smake
#
# Build virtio_serial.o, the loadable virtio-serial driver, inside IRIX with
# MIPSpro.  ../virtio/virtio.mk supplies the kernel CFLAGS and the transport.
#
#   smake                  build into $(OBJDIR)
#   smake load / unload    ml ld / ml unld the module
#   smake install          load it at every boot (smake uninstall undoes it)
#
# /share may be NFS and noexec, so objects go to local disk.
OBJDIR  = /tmp/virtio_serial.$(CPUBOARD)
include ../virtio/virtio.mk

# One module object, so ml can load it: the parts, linked with ld -r.
MODULE  = $(OBJDIR)/mod/virtio_serial.o
OBJS    = $(OBJDIR)/virtio_serial.o $(TRANS)

all: $(MODULE)

$(MODULE): $(OBJS)
	mkdir -p $(OBJDIR)/mod
	ld $(LDABI) -r -o $(MODULE) $(OBJS)

$(OBJDIR)/virtio_serial.o: virtio_serial.c virtio_serial.h $(VIRTIO_HDRS)
	mkdir -p $(OBJDIR)
	$(CC) $(LOCAL_CFLAGS) -c -o $@ virtio_serial.c

clean:
	rm -rf $(OBJDIR)

load: $(MODULE)
	ml ld -v -c $(MODULE) -p virtio_serial_

unload:
	id=`ml list -l | awk '/virtio_serial/ { print $$2 }'`; \
	if [ -n "$$id" ]; then ml unld $$id; fi

install: $(MODULE)
	mkdir -p /usr/lib/virtio_serial
	cp $(MODULE) /usr/lib/virtio_serial/virtio_serial.o
	cp virtio_serial.init /etc/init.d/virtio_serial
	chmod a+x /etc/init.d/virtio_serial
	ln -sf ../init.d/virtio_serial /etc/rc2.d/S98virtio_serial
	ln -sf ../init.d/virtio_serial /etc/rc0.d/K02virtio_serial

uninstall:
	rm -f /etc/rc2.d/S98virtio_serial /etc/rc0.d/K02virtio_serial /etc/init.d/virtio_serial
	rm -rf /usr/lib/virtio_serial

.PHONY: all clean load unload install uninstall
