# Builds Virtio9P.kext and mount_9p inside a Mac OS X guest, with that guest's own Developer
# Tools, the way ../virtio_console is built (see its Makefile for where every compiler flag of
# the PowerPC builds comes from and the failure behind each):
#
#   make ABI=gcc2   on 10.1 with the December 2001 tools (GCC 2.95): for 10.0 and 10.1, PowerPC
#   make ABI=gcc3   on 10.2 with the December 2002 tools (GCC 3):    for 10.2 and 10.3, PowerPC
#   make ABI=kpi    on 10.6 with Xcode 3.2.6:                        for 10.4 to 10.13, Intel, and 10.4 and 10.5, PowerPC
#
# Leaves build/<ABI>/Virtio9P.kext and build/<ABI>/mount_9p. The PowerPC builds are of the BSD
# frontend (p9fs_bsd_*.c), the Intel one of the KPI frontend (p9fs_kpi_*.c); the device and the
# protocol are every build's (p9fs.h).

VERSION ?= 0.3.1
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

ifeq ($(ABI),kpi)
all: kpi
else
all: $(KEXT) $(BUILD)/mount_9p
endif

# The filesystem is C, compiled as the BSD kernel's own code is; the IOKit driver is C++.
FSOBJS = $(BUILD)/p9fs_proto.o $(BUILD)/p9fs_bsd_node.o $(BUILD)/p9fs_bsd_vnops.o $(BUILD)/p9fs_bsd_vfsops.o
OBJS   = $(BUILD)/Virtio9P_info.o $(BUILD)/Virtio9P.o $(FSOBJS)
FSHDRS = p9fs.h p9fs_args.h p9_transport.h

$(BUILD)/Virtio9P.o: Virtio9P.cpp Virtio9P.h p9_transport.h
	mkdir -p $(BUILD)
	cc $(CXXFLAGS) -x c++ -c Virtio9P.cpp -o $@

$(BUILD)/Virtio9P_info.o: Virtio9P_info.c
	mkdir -p $(BUILD)
	cc $(CFLAGS) -c Virtio9P_info.c -o $@

$(BUILD)/p9fs_proto.o: p9fs_proto.c $(FSHDRS)
	mkdir -p $(BUILD)
	cc $(CFLAGS) -c p9fs_proto.c -o $@

$(BUILD)/p9fs_bsd_node.o: p9fs_bsd_node.c $(FSHDRS)
	mkdir -p $(BUILD)
	cc $(CFLAGS) -c p9fs_bsd_node.c -o $@

$(BUILD)/p9fs_bsd_vnops.o: p9fs_bsd_vnops.c $(FSHDRS)
	mkdir -p $(BUILD)
	cc $(CFLAGS) -c p9fs_bsd_vnops.c -o $@

$(BUILD)/p9fs_bsd_vfsops.o: p9fs_bsd_vfsops.c $(FSHDRS)
	mkdir -p $(BUILD)
	cc $(CFLAGS) -c p9fs_bsd_vfsops.c -o $@

ifneq ($(ABI),kpi)
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
endif

# --- KPI, 10.4 to 10.13: one kext, three slices (ppc for PowerPC 10.4 and 10.5) -------------------------------------------------
#
# i386 for the 32-bit kernels of 10.4 to 10.7, against 10.4's KPI with GCC 4.0, linked as the
# relocatable object (MH_OBJECT) every 32-bit kextload takes; x86_64 for the 64-bit kernels of
# 10.6 on (the only ones from 10.8), against 10.6's with GCC 4.2, linked as the MH_KEXT_BUNDLE
# they require. 10.5 renamed ubc_sync_range ubc_msync (P9FS_UBC_MSYNC, p9fs_kpi.h). The SDKs are
# where Xcode.mpkg puts them, or where its packages installed one by one do.
KPI_SDKS       ?= $(firstword $(wildcard /Developer/SDKs /SDKs))
KPI_SDK_I386   ?= $(KPI_SDKS)/MacOSX10.4u.sdk
KPI_SDK_X86_64 ?= $(firstword $(wildcard $(KPI_SDKS)/MacOSX10.6.sdk) /)
KPI_CC_I386    ?= gcc-4.0
KPI_CC_X86_64  ?= gcc-4.2
KPI_KH_I386     = $(KPI_SDK_I386)/System/Library/Frameworks/Kernel.framework/Headers
KPI_KH_X86_64   = $(KPI_SDK_X86_64)/System/Library/Frameworks/Kernel.framework/Headers
KPI_TRACE       = $(if $(TRACE),-DVIRTIO9P_TRACE)
KPI_CFLAGS      = $(KPI_TRACE) -mkernel -nostdinc -fno-builtin -fno-common -finline -fno-keep-inline-functions -Os -Wall \
                  -DKERNEL -DKERNEL_PRIVATE -DDRIVER_PRIVATE -DAPPLE -DNeXT -DP9FS_KPI \
                  -DVIRTIO9P_VERSION='"$(VERSION)"'
KPI_CXXFLAGS    = -fapple-kext -fno-rtti -fno-exceptions -fcheck-new
KPI_SRCS        = Virtio9P_info.c p9fs_proto.c p9fs_kpi_node.c p9fs_kpi_vnops.c p9fs_kpi_vfsops.c
KPI_HDRS        = $(FSHDRS) p9fs_kpi.h Virtio9P.h

ifeq ($(ABI),kpi)
kpi: $(BUILD)/Virtio9P.kext $(BUILD)/mount_9p

# One slice's objects: build/kpi/<arch>/*.o from the C sources and Virtio9P.cpp.
define kpi_objects
$(BUILD)/$(1)/objects: $(KPI_SRCS) Virtio9P.cpp $(KPI_HDRS)
	mkdir -p $(BUILD)/$(1)
	for f in $(KPI_SRCS); do \
		$(2) -arch $(1) $(KPI_CFLAGS) $(4) -I$(3) -I$(3)/bsd -c $$$$f -o $(BUILD)/$(1)/`basename $$$$f .c`.o || exit 1; \
	done
	$(2) -arch $(1) $(KPI_CFLAGS) $(4) $(KPI_CXXFLAGS) -I$(3) -I$(3)/bsd -x c++ -c Virtio9P.cpp -o $(BUILD)/$(1)/Virtio9P.o
	touch $$@
endef
$(eval $(call kpi_objects,i386,$(KPI_CC_I386),$(KPI_KH_I386),-isysroot $(KPI_SDK_I386) -mmacosx-version-min=10.4 -force_cpusubtype_ALL -msoft-float -static))
# ppc for PowerPC 10.4 and 10.5, whose kernels export the KPI too and not 10.3's VFS the PowerPC
# builds above link against (_MALLOC, copyin, desiredvnodes: undefined on an installed 10.4.11,
# 2026-11-22): the same 10.4 SDK, which is universal, and the same GCC 4.0.
$(eval $(call kpi_objects,ppc,$(KPI_CC_I386),$(KPI_KH_I386),-isysroot $(KPI_SDK_I386) -mmacosx-version-min=10.4 -force_cpusubtype_ALL -msoft-float -static -mlong-branch))
$(eval $(call kpi_objects,x86_64,$(KPI_CC_X86_64),$(KPI_KH_X86_64),-isysroot $(KPI_SDK_X86_64) -mmacosx-version-min=10.6 -DP9FS_UBC_MSYNC))

# Objects before the libraries, as for the PowerPC builds.
$(BUILD)/i386/Virtio9P: $(BUILD)/i386/objects
	$(KPI_CC_I386) -arch i386 -isysroot $(KPI_SDK_I386) -static -nostdlib -r -o $@ $(BUILD)/i386/*.o \
		-lkmodc++ -lkmod -lcc_kext

$(BUILD)/ppc/Virtio9P: $(BUILD)/ppc/objects
	$(KPI_CC_I386) -arch ppc -isysroot $(KPI_SDK_I386) -static -nostdlib -r -o $@ $(BUILD)/ppc/*.o \
		-lkmodc++ -lkmod -lcc_kext

$(BUILD)/x86_64/Virtio9P: $(BUILD)/x86_64/objects
	$(KPI_CC_X86_64) -arch x86_64 -isysroot $(KPI_SDK_X86_64) -Xlinker -kext -nostdlib -o $@ $(BUILD)/x86_64/*.o \
		-lkmodc++ -lkmod -lcc_kext

# mount_9p for every Intel release: i386 and x86_64 against 10.4's libSystem, which is all it uses
# and which Tiger has for x86_64 too (it runs that slice on a 64-bit CPU); 10.15 runs no other.
$(BUILD)/mount_9p: mount_9p.c p9fs_args.h
	mkdir -p $(BUILD)
	$(KPI_CC_I386) -arch ppc -arch i386 -arch x86_64 -isysroot $(KPI_SDK_I386) -mmacosx-version-min=10.4 -O2 -Wall \
		-o $@ mount_9p.c

$(BUILD)/Virtio9P.kext: $(BUILD)/ppc/Virtio9P $(BUILD)/i386/Virtio9P $(BUILD)/x86_64/Virtio9P Info.plist.in libraries-kpi.xml
	rm -rf $@
	mkdir -p $@/Contents/MacOS
	lipo -create -output $@/Contents/MacOS/Virtio9P $(BUILD)/ppc/Virtio9P $(BUILD)/i386/Virtio9P $(BUILD)/x86_64/Virtio9P
	perl -pe 's/\@VERSION\@/$(VERSION)/g; if (/\@LIBRARIES\@/) { open(L, "libraries-kpi.xml") or die; local $$/; $$_ = <L>; }' Info.plist.in | perl -0pe 's/[ \t]*<!--.*?-->\n?//gs' > $@/Contents/Info.plist
endif

clean:
	rm -rf build

.PHONY: all clean
