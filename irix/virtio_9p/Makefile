#!smake
#
# Build virtio_9p.o, a loadable 9P filesystem over virtio-9p, and mount_9p,
# inside IRIX with MIPSpro.  ../virtio/virtio.mk supplies the kernel CFLAGS and
# the transport.
#
#   smake                  build into $(OBJDIR)
#   smake load / unload    ml ld / ml unld the module, as filesystem type 9p
#   smake install          load it at every boot (smake uninstall undoes it)
#
# /share may be NFS and noexec, so objects go to local disk.
OBJDIR  = /tmp/virtio_9p.$(CPUBOARD)
include ../virtio/virtio.mk

# One module object, so ml can load it: the parts, linked with ld -r.
MODULE  = $(OBJDIR)/mod/virtio_9p.o
OBJS    = $(OBJDIR)/p9.o $(OBJDIR)/virtio_9p_dev.o $(OBJDIR)/virtio_9p_vfs.o \
	  $(OBJDIR)/virtio_9p_vnops.o $(TRANS)
HDRS    = p9.h virtio_9p_impl.h $(VIRTIO_HDRS)
MOUNT9P = $(OBJDIR)/mount_9p

all: $(MODULE) $(MOUNT9P)

$(MODULE): $(OBJS)
	mkdir -p $(OBJDIR)/mod
	ld $(LDABI) -r -o $(MODULE) $(OBJS)

$(OBJDIR)/p9.o: p9.c $(HDRS)
	mkdir -p $(OBJDIR)
	$(CC) $(LOCAL_CFLAGS) -c -o $@ p9.c

$(OBJDIR)/virtio_9p_dev.o: virtio_9p_dev.c $(HDRS)
	mkdir -p $(OBJDIR)
	$(CC) $(LOCAL_CFLAGS) -c -o $@ virtio_9p_dev.c

$(OBJDIR)/virtio_9p_vfs.o: virtio_9p_vfs.c $(HDRS)
	mkdir -p $(OBJDIR)
	$(CC) $(LOCAL_CFLAGS) -c -o $@ virtio_9p_vfs.c

$(OBJDIR)/virtio_9p_vnops.o: virtio_9p_vnops.c $(HDRS)
	mkdir -p $(OBJDIR)
	$(CC) $(LOCAL_CFLAGS) -c -o $@ virtio_9p_vnops.c

# A user program: the same on every board, built for the oldest ABI that runs it.
$(MOUNT9P): mount_9p.c
	mkdir -p $(OBJDIR)
	$(CC) -n32 -mips3 -o $@ mount_9p.c

clean:
	rm -rf $(OBJDIR)

load: $(MODULE)
	ml ld -v -j $(MODULE) -p virtio_9p_ -a 9p

unload:
	id=`ml list -l | awk '/virtio_9p/ { print $$2 }'`; \
	if [ -n "$$id" ]; then ml unld $$id; fi

install: $(MODULE) $(MOUNT9P)
	mkdir -p /usr/lib/virtio_9p
	cp $(MODULE) /usr/lib/virtio_9p/virtio_9p.o
	cp $(MOUNT9P) /usr/etc/mount_9p
	chmod a+x /usr/etc/mount_9p
	cp virtio_9p.init /etc/init.d/virtio_9p
	chmod a+x /etc/init.d/virtio_9p
	ln -sf ../init.d/virtio_9p /etc/rc2.d/S98virtio_9p
	ln -sf ../init.d/virtio_9p /etc/rc0.d/K02virtio_9p

uninstall:
	rm -f /etc/rc2.d/S98virtio_9p /etc/rc0.d/K02virtio_9p /etc/init.d/virtio_9p
	rm -f /usr/etc/mount_9p
	rm -rf /usr/lib/virtio_9p

.PHONY: all clean load unload install uninstall
