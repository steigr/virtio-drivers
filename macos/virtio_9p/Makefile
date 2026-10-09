# Builds Virtio9P.kext and mount_9p inside a PowerPC Mac OS X guest, with that guest's own
# Developer Tools, the way ../ppc/virtioconsole is built (see its Makefile for where every compiler
# flag comes from and the failure behind each):
#
#   make ABI=gcc2   on 10.1 with the December 2001 tools (GCC 2.95): for 10.0 and 10.1
#   make ABI=gcc3   on 10.2 with the December 2002 tools (GCC 3):    for 10.2 and 10.3
#
# Leaves build/<ABI>/Virtio9P.kext and build/<ABI>/mount_9p.

VERSION ?= 0.1.0
ABI     ?= gcc2

# As for VirtioConsole, the GCC 2.95 build must see 10.0's Kernel.framework and kmod libraries,
# not 10.1's, or the kext imports what 10.0's kernel lacks:
#   make ABI=gcc2 KERNEL_HEADERS=<its Kernel.framework/Headers> KMOD_LIB_DIR=<its libkmod*.a, libcc_kext.a>
KERNEL_HEADERS ?= /System/Library/Frameworks/Kernel.framework/Headers
KMOD_LIB_DIR   ?=

# P9FS_ABI_GCC2 picks the page-list calls 10.0 has (p9fs_vnops.c).
CFLAGS_gcc2 = -DP9FS_ABI_GCC2
CFLAGS_gcc3 =
CFLAGS = -arch ppc -fno-common -nostdinc -O2 -Wall -I$(KERNEL_HEADERS) -I$(KERNEL_HEADERS)/bsd \
         -fno-builtin -finline -fno-keep-inline-functions -force_cpusubtype_ALL -msoft-float -mlong-branch \
         -static -DKERNEL -DKERNEL_PRIVATE -DDRIVER_PRIVATE -DAPPLE -DNeXT \
         -DVIRTIO9P_VERSION='"$(VERSION)"' $(CFLAGS_$(ABI))
CXXFLAGS_gcc2 = -fvtable-thunks -findirect-virtual-calls
CXXFLAGS_gcc3 = -fapple-kext
CXXFLAGS = $(CFLAGS) -fno-rtti -fno-exceptions -fcheck-new $(CXXFLAGS_$(ABI))

BUILD = build/$(ABI)
KEXT  = $(BUILD)/Virtio9P.kext

# The filesystem is C, compiled as the BSD kernel's own code is; the IOKit driver is C++.
FSOBJS = $(BUILD)/p9fs_proto.o $(BUILD)/p9fs_node.o $(BUILD)/p9fs_vnops.o $(BUILD)/p9fs_vfsops.o
OBJS   = $(BUILD)/Virtio9P_info.o $(BUILD)/Virtio9P.o $(FSOBJS)
FSHDRS = p9fs.h p9fs_args.h p9_transport.h

all: $(KEXT) $(BUILD)/mount_9p

$(BUILD)/Virtio9P.o: Virtio9P.cpp Virtio9P.h p9_transport.h
	mkdir -p $(BUILD)
	cc $(CXXFLAGS) -x c++ -c Virtio9P.cpp -o $@

$(BUILD)/Virtio9P_info.o: Virtio9P_info.c
	mkdir -p $(BUILD)
	cc $(CFLAGS) -c Virtio9P_info.c -o $@

$(BUILD)/p9fs_proto.o: p9fs_proto.c $(FSHDRS)
	mkdir -p $(BUILD)
	cc $(CFLAGS) -c p9fs_proto.c -o $@

$(BUILD)/p9fs_node.o: p9fs_node.c $(FSHDRS)
	mkdir -p $(BUILD)
	cc $(CFLAGS) -c p9fs_node.c -o $@

$(BUILD)/p9fs_vnops.o: p9fs_vnops.c $(FSHDRS)
	mkdir -p $(BUILD)
	cc $(CFLAGS) -c p9fs_vnops.c -o $@

$(BUILD)/p9fs_vfsops.o: p9fs_vfsops.c $(FSHDRS)
	mkdir -p $(BUILD)
	cc $(CFLAGS) -c p9fs_vfsops.c -o $@

# Objects before the libraries, as for VirtioConsole: otherwise libkmod's __start is taken and
# no constructor registers the driver's class. The kmod's start and stop routines are its own
# (Virtio9P_info.c); libkmodc++ is still what brings in the C++ runtime's pieces.
$(BUILD)/Virtio9P: $(OBJS)
	cc -arch ppc -static -nostdlib -r -o $@ $(OBJS) \
	   $(addprefix -L,$(KMOD_LIB_DIR)) -lkmodc++ -lkmod -lcc_kext

# perl rather than sed: 10.1's sed cannot put a file's lines into a replacement.
# The comments of Info.plist.in are dropped from the built plist: BootX, which loads the kexts a boot
# needs before the root is mounted, ends a <dict> at an XML comment, and then loads nothing of the kext
# (live, VirtioBlock on 10.0.4 booting from virtio-blk, 2026-10-03; BootX-36 drivers.c ParseTagList).
$(KEXT): $(BUILD)/Virtio9P Info.plist.in libraries-$(ABI).xml
	rm -rf $(KEXT)
	mkdir -p $(KEXT)/Contents/MacOS
	cp $(BUILD)/Virtio9P $(KEXT)/Contents/MacOS/Virtio9P
	perl -pe 's/\@VERSION\@/$(VERSION)/g; if (/\@LIBRARIES\@/) { open(L, "libraries-$(ABI).xml") or die; local $$/; $$_ = <L>; }' Info.plist.in | perl -0pe 's/[ \t]*<!--.*?-->\n?//gs' > $(KEXT)/Contents/Info.plist

# An ordinary program, against the guest's own headers and libraries.
$(BUILD)/mount_9p: mount_9p.c p9fs_args.h
	mkdir -p $(BUILD)
	cc -O2 -Wall -o $@ mount_9p.c

clean:
	rm -rf build

.PHONY: all clean
