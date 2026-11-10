#
# What every IRIX virtio driver's Makefile includes after setting OBJDIR: the
# kernel CFLAGS for CPUBOARD, the MIPSpro wrapper, and the transport layer
# (split rings, virtio-mmio on GIO, virtio-pci) compiled into $(OBJDIR).
#
# CPUBOARD defaults to this machine's.  Cross-board builds work where the
# headers do: smake CPUBOARD=IP30 compiles the virtio-pci transport on an IP28.
CPUBOARD != uname -m
include /var/sysgen/Makefile.kernloadio

VIRTIO  = ../virtio
TRANS   = $(OBJDIR)/virtq.o $(OBJDIR)/virtio_gio.o $(OBJDIR)/virtio_pci.o
VIRTIO_HDRS = $(VIRTIO)/virtio_bus.h $(VIRTIO)/virtq.h

# ABI for ld -r follows the CFLAGS the sample makefile picked.
#if $(CPUBOARD)=="IP20" || $(CPUBOARD)=="IP22" || $(CPUBOARD)=="IP32"
LDABI   = -n32
#else
LDABI   = -64
#endif

# 1174: a parameter never used; 1552: a variable set and never read.  The DDI's
# prototypes make both unavoidable.  3201 is the same for parameters on 7.4.
WOFF    = -woff 1174,1552,3201
LOCAL_CFLAGS = $(CFLAGS) $(WOFF) -I. -I$(VIRTIO)

# Through the MIPSpro wrapper, if irix/tools/install-wrapper has been run.
#if exists(/usr/local/mipspro/bin/cc)
CC = /usr/local/mipspro/bin/cc
#else
CC = cc
#endif

$(OBJDIR)/virtq.o: $(VIRTIO)/virtq.c $(VIRTIO_HDRS)
	mkdir -p $(OBJDIR)
	$(CC) $(LOCAL_CFLAGS) -c -o $@ $(VIRTIO)/virtq.c

$(OBJDIR)/virtio_gio.o: $(VIRTIO)/virtio_gio.c $(VIRTIO_HDRS)
	mkdir -p $(OBJDIR)
	$(CC) $(LOCAL_CFLAGS) -c -o $@ $(VIRTIO)/virtio_gio.c

$(OBJDIR)/virtio_pci.o: $(VIRTIO)/virtio_pci.c $(VIRTIO_HDRS)
	mkdir -p $(OBJDIR)
	$(CC) $(LOCAL_CFLAGS) -c -o $@ $(VIRTIO)/virtio_pci.c
