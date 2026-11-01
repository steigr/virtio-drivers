#!smake
#
# Build vioser.o, the loadable virtio-serial driver, inside IRIX with MIPSpro.
# /var/sysgen/Makefile.kernloadio supplies the kernel CFLAGS for CPUBOARD.
#
#   make                  build into $(OBJDIR)
#   make load / unload    ml ld / ml unld the module
#   make install          load at every boot (make uninstall undoes it)
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
OBJS    = $(OBJDIR)/vioser.o $(OBJDIR)/virtq.o $(OBJDIR)/vioser_gio.o \
	  $(OBJDIR)/vioser_pci.o
HDRS    = vioser.h vioser_impl.h virtq.h

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

all: $(MODULE)

$(MODULE): $(OBJS)
	mkdir -p $(OBJDIR)/mod
	ld $(LDABI) -r -o $(MODULE) $(OBJS)

$(OBJDIR)/vioser.o: vioser.c $(HDRS)
	mkdir -p $(OBJDIR)
	$(CC) $(LOCAL_CFLAGS) -c -o $@ vioser.c

$(OBJDIR)/virtq.o: virtq.c $(HDRS)
	mkdir -p $(OBJDIR)
	$(CC) $(LOCAL_CFLAGS) -c -o $@ virtq.c

$(OBJDIR)/vioser_gio.o: vioser_gio.c $(HDRS)
	mkdir -p $(OBJDIR)
	$(CC) $(LOCAL_CFLAGS) -c -o $@ vioser_gio.c

$(OBJDIR)/vioser_pci.o: vioser_pci.c $(HDRS)
	mkdir -p $(OBJDIR)
	$(CC) $(LOCAL_CFLAGS) -c -o $@ vioser_pci.c

clean:
	rm -rf $(OBJDIR)

load: $(MODULE)
	ml ld -v -c $(MODULE) -p vioser_

unload:
	id=`ml list -l | awk '/vioser/ { print $$2 }'`; \
	if [ -n "$$id" ]; then ml unld $$id; fi

install: $(MODULE)
	mkdir -p /usr/lib/vioser
	cp $(MODULE) /usr/lib/vioser/vioser.o
	cp vioser.init /etc/init.d/vioser
	chmod a+x /etc/init.d/vioser
	ln -sf ../init.d/vioser /etc/rc2.d/S98vioser
	ln -sf ../init.d/vioser /etc/rc0.d/K02vioser

uninstall:
	rm -f /etc/rc2.d/S98vioser /etc/rc0.d/K02vioser /etc/init.d/vioser
	rm -rf /usr/lib/vioser

.PHONY: all clean load unload install uninstall
