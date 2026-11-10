#!smake
#
# Build the loadable virtio drivers inside IRIX with MIPSpro: virtio_serial.o for
# virtio-serial and virtio_9p.o, a filesystem for virtio-9p.
# /var/sysgen/Makefile.kernloadio supplies the kernel CFLAGS for CPUBOARD.
#
#   make                  build into $(OBJDIR)
#   make load / unload    ml ld / ml unld the virtio_serial module
#   make load9p / unload9p  the same for the filesystem
#   make install          load both at every boot (make uninstall undoes it)
#
# CPUBOARD defaults to this machine's.  Cross-board builds work where the
# headers do: CPUBOARD=IP30 make compiles the virtio-pci transport on an IP28.
#
# /share may be NFS and noexec, so objects go to local disk.
CPUBOARD != uname -m
include /var/sysgen/Makefile.kernloadio

OBJDIR  = /tmp/virtio_serial.$(CPUBOARD)
# One module object, so ml can load it: the parts, linked with ld -r.
MODULE  = $(OBJDIR)/mod/virtio_serial.o
MODULE9P = $(OBJDIR)/mod/virtio_9p.o
TRANS   = $(OBJDIR)/virtq.o $(OBJDIR)/virtio_gio.o $(OBJDIR)/virtio_pci.o
OBJS    = $(OBJDIR)/virtio_serial.o $(TRANS)
OBJS9P  = $(OBJDIR)/p9.o $(OBJDIR)/virtio_9p_dev.o $(OBJDIR)/virtio_9p_vfs.o \
	  $(OBJDIR)/virtio_9p_vnops.o $(TRANS)
HDRS    = virtio_serial.h virtio_bus.h virtq.h p9.h virtio_9p_impl.h
MOUNT9P = $(OBJDIR)/mount_9p

# ABI for ld -r follows the CFLAGS the sample makefile picked.
#if $(CPUBOARD)=="IP20" || $(CPUBOARD)=="IP22" || $(CPUBOARD)=="IP32"
LDABI   = -n32
#else
LDABI   = -64
#endif

# 1174: a parameter never used; 1552: a variable set and never read.  The DDI's
# prototypes make both unavoidable.  3201 is the same for parameters on 7.4.
WOFF    = -woff 1174,1552,3201
LOCAL_CFLAGS = $(CFLAGS) $(WOFF) -I.

# Through the MIPSpro wrapper, if tools/install-wrapper has been run.
#if exists(/usr/local/mipspro/bin/cc)
CC = /usr/local/mipspro/bin/cc
#else
CC = cc
#endif

all: $(MODULE) $(MODULE9P) $(MOUNT9P)

$(MODULE): $(OBJS)
	mkdir -p $(OBJDIR)/mod
	ld $(LDABI) -r -o $(MODULE) $(OBJS)

$(MODULE9P): $(OBJS9P)
	mkdir -p $(OBJDIR)/mod
	ld $(LDABI) -r -o $(MODULE9P) $(OBJS9P)

$(OBJDIR)/virtio_serial.o: virtio_serial.c $(HDRS)
	mkdir -p $(OBJDIR)
	$(CC) $(LOCAL_CFLAGS) -c -o $@ virtio_serial.c

$(OBJDIR)/virtq.o: virtq.c $(HDRS)
	mkdir -p $(OBJDIR)
	$(CC) $(LOCAL_CFLAGS) -c -o $@ virtq.c

$(OBJDIR)/virtio_gio.o: virtio_gio.c $(HDRS)
	mkdir -p $(OBJDIR)
	$(CC) $(LOCAL_CFLAGS) -c -o $@ virtio_gio.c

$(OBJDIR)/virtio_pci.o: virtio_pci.c $(HDRS)
	mkdir -p $(OBJDIR)
	$(CC) $(LOCAL_CFLAGS) -c -o $@ virtio_pci.c

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
	ml ld -v -c $(MODULE) -p virtio_serial_

unload:
	id=`ml list -l | awk '/virtio_serial/ { print $$2 }'`; \
	if [ -n "$$id" ]; then ml unld $$id; fi

load9p: $(MODULE9P)
	ml ld -v -j $(MODULE9P) -p virtio_9p_ -a 9p

unload9p:
	id=`ml list -l | awk '/virtio_9p/ { print $$2 }'`; \
	if [ -n "$$id" ]; then ml unld $$id; fi

install: $(MODULE) $(MODULE9P) $(MOUNT9P)
	mkdir -p /usr/lib/virtio_serial
	cp $(MODULE) /usr/lib/virtio_serial/virtio_serial.o
	cp virtio_serial.init /etc/init.d/virtio_serial
	chmod a+x /etc/init.d/virtio_serial
	ln -sf ../init.d/virtio_serial /etc/rc2.d/S98virtio_serial
	ln -sf ../init.d/virtio_serial /etc/rc0.d/K02virtio_serial
	mkdir -p /usr/lib/virtio_9p
	cp $(MODULE9P) /usr/lib/virtio_9p/virtio_9p.o
	cp $(MOUNT9P) /usr/etc/mount_9p
	chmod a+x /usr/etc/mount_9p
	cp virtio_9p.init /etc/init.d/virtio_9p
	chmod a+x /etc/init.d/virtio_9p
	ln -sf ../init.d/virtio_9p /etc/rc2.d/S98virtio_9p
	ln -sf ../init.d/virtio_9p /etc/rc0.d/K02virtio_9p

uninstall:
	rm -f /etc/rc2.d/S98virtio_serial /etc/rc0.d/K02virtio_serial /etc/init.d/virtio_serial
	rm -f /etc/rc2.d/S98virtio_9p /etc/rc0.d/K02virtio_9p /etc/init.d/virtio_9p
	rm -f /usr/etc/mount_9p
	rm -rf /usr/lib/virtio_serial /usr/lib/virtio_9p

.PHONY: all clean load unload load9p unload9p install uninstall
