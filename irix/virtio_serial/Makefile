#!smake
#
# Build the loadable virtio drivers inside IRIX with MIPSpro: vioser.o for
# virtio-serial and p9fs.o, a filesystem for virtio-9p.
# /var/sysgen/Makefile.kernloadio supplies the kernel CFLAGS for CPUBOARD.
#
#   make                  build into $(OBJDIR)
#   make load / unload    ml ld / ml unld the vioser module
#   make load9p / unload9p  the same for the filesystem
#   make install          load both at every boot (make uninstall undoes it)
#
# CPUBOARD defaults to this machine's.  Cross-board builds work where the
# headers do: CPUBOARD=IP30 make compiles the virtio-pci transport on an IP28.
#
# /share may be NFS and noexec, so objects go to local disk.
CPUBOARD != uname -m
include /var/sysgen/Makefile.kernloadio

OBJDIR  = /tmp/vioser.$(CPUBOARD)
# One module object, so ml can load it: the parts, linked with ld -r.
MODULE  = $(OBJDIR)/mod/vioser.o
MODULE9P = $(OBJDIR)/mod/p9fs.o
TRANS   = $(OBJDIR)/virtq.o $(OBJDIR)/virtio_gio.o $(OBJDIR)/virtio_pci.o
OBJS    = $(OBJDIR)/vioser.o $(TRANS)
OBJS9P  = $(OBJDIR)/p9.o $(OBJDIR)/p9fs_dev.o $(OBJDIR)/p9fs_vfs.o \
	  $(OBJDIR)/p9fs_vnops.o $(TRANS)
HDRS    = vioser.h virtio_bus.h virtq.h p9.h p9fs_impl.h
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

$(OBJDIR)/vioser.o: vioser.c $(HDRS)
	mkdir -p $(OBJDIR)
	$(CC) $(LOCAL_CFLAGS) -c -o $@ vioser.c

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

$(OBJDIR)/p9fs_dev.o: p9fs_dev.c $(HDRS)
	mkdir -p $(OBJDIR)
	$(CC) $(LOCAL_CFLAGS) -c -o $@ p9fs_dev.c

$(OBJDIR)/p9fs_vfs.o: p9fs_vfs.c $(HDRS)
	mkdir -p $(OBJDIR)
	$(CC) $(LOCAL_CFLAGS) -c -o $@ p9fs_vfs.c

$(OBJDIR)/p9fs_vnops.o: p9fs_vnops.c $(HDRS)
	mkdir -p $(OBJDIR)
	$(CC) $(LOCAL_CFLAGS) -c -o $@ p9fs_vnops.c

# A user program: the same on every board, built for the oldest ABI that runs it.
$(MOUNT9P): mount_9p.c
	mkdir -p $(OBJDIR)
	$(CC) -n32 -mips3 -o $@ mount_9p.c

clean:
	rm -rf $(OBJDIR)

load: $(MODULE)
	ml ld -v -c $(MODULE) -p vioser_

unload:
	id=`ml list -l | awk '/vioser/ { print $$2 }'`; \
	if [ -n "$$id" ]; then ml unld $$id; fi

load9p: $(MODULE9P)
	ml ld -v -j $(MODULE9P) -p p9fs_ -a 9p

unload9p:
	id=`ml list -l | awk '/p9fs/ { print $$2 }'`; \
	if [ -n "$$id" ]; then ml unld $$id; fi

install: $(MODULE) $(MODULE9P) $(MOUNT9P)
	mkdir -p /usr/lib/vioser
	cp $(MODULE) /usr/lib/vioser/vioser.o
	cp vioser.init /etc/init.d/vioser
	chmod a+x /etc/init.d/vioser
	ln -sf ../init.d/vioser /etc/rc2.d/S98vioser
	ln -sf ../init.d/vioser /etc/rc0.d/K02vioser
	mkdir -p /usr/lib/p9fs
	cp $(MODULE9P) /usr/lib/p9fs/p9fs.o
	cp $(MOUNT9P) /usr/etc/mount_9p
	chmod a+x /usr/etc/mount_9p
	cp p9fs.init /etc/init.d/p9fs
	chmod a+x /etc/init.d/p9fs
	ln -sf ../init.d/p9fs /etc/rc2.d/S98p9fs
	ln -sf ../init.d/p9fs /etc/rc0.d/K02p9fs

uninstall:
	rm -f /etc/rc2.d/S98vioser /etc/rc0.d/K02vioser /etc/init.d/vioser
	rm -f /etc/rc2.d/S98p9fs /etc/rc0.d/K02p9fs /etc/init.d/p9fs
	rm -f /usr/etc/mount_9p
	rm -rf /usr/lib/vioser /usr/lib/p9fs

.PHONY: all clean load unload load9p unload9p install uninstall
