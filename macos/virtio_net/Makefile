# Builds VirtioNet.kext inside a Mac OS X guest, with that guest's own Developer Tools. On PowerPC
# the way ../ppc/virtioconsole is built (see its Makefile for where every compiler flag comes from
# and the failure behind each); on Intel against the kernel programming interfaces of 10.4 on:
#
#   make ABI=gcc2   on 10.1 with the December 2001 tools (GCC 2.95): for 10.0 and 10.1, PowerPC
#   make ABI=gcc3   on 10.2 with the December 2002 tools (GCC 3):    for 10.2 and 10.3, PowerPC
#   make ABI=kpi    on 10.6 with Xcode 3.2.6:                       for 10.4 to 10.15, Intel
#
# Leaves build/<ABI>/VirtioNet.kext; `make ABI=kpi intel` also the two Intel bundles, build/intel.

VERSION ?= 0.2.2
ABI     ?= gcc2

# As for VirtioConsole, the GCC 2.95 build must see 10.0's Kernel.framework and kmod libraries,
# not 10.1's, or the kext imports what 10.0's kernel lacks:
#   make ABI=gcc2 KERNEL_HEADERS=<its Kernel.framework/Headers> KMOD_LIB_DIR=<its libkmod*.a, libcc_kext.a>
KERNEL_HEADERS ?= /System/Library/Frameworks/Kernel.framework/Headers
KMOD_LIB_DIR   ?=

CFLAGS = -arch ppc -fno-common -nostdinc -O2 -Wall -I$(KERNEL_HEADERS) -I$(KERNEL_HEADERS)/bsd \
         -fno-builtin -finline -fno-keep-inline-functions -force_cpusubtype_ALL -msoft-float -mlong-branch \
         -static -DKERNEL -DKERNEL_PRIVATE -DDRIVER_PRIVATE -DAPPLE -DNeXT \
         -DVIRTIONET_VERSION='"$(VERSION)"'
# -fpermissive for GCC 2.95 alone: IOEthernetController.h includes netinet/in.h, whose struct ip_opts
# has a member ip_opts, and GCC 2.95's C++ makes that pedantic complaint an error by default --
# "ANSI C++ forbids data member `ip_opts' with same name as enclosing class", with 10.0's headers
# and with 10.1's own alike (live, 10.1.5, 2026-10-01). -isystem, which would quiet it as a system
# header's, wraps those headers in an implicit extern "C" and breaks every C++ one; the tag and the
# member are one token, so no macro renames one without the other. With -fpermissive it is that one
# warning; GCC 3 accepts the header as it is.
CXXFLAGS_gcc2 = -fvtable-thunks -findirect-virtual-calls -fpermissive
CXXFLAGS_gcc3 = -fapple-kext
CXXFLAGS = $(CFLAGS) -fno-rtti -fno-exceptions -fcheck-new $(CXXFLAGS_$(ABI))

# OSBundleLibraries come from libraries-$(ABI).xml, as VirtioConsole's, with IONetworkingFamily
# in place of IOSerialFamily: 1.0.7 in 10.0's System.kext, 1.1 on 10.1, 1.2 on 10.2, 1.4.0 on
# 10.3, each compatible back to 1.0.0b1 (read off the guests, 2026-10-01). No com.apple.kernel.bsd:
# the driver uses no BSD symbol, and no one version of it satisfies both 10.0 and 10.1
# (../ppc/virtio9p).

BUILD = build/$(ABI)
KEXT  = $(BUILD)/VirtioNet.kext

all: $(KEXT)

# The Intel build: one binary of two slices, against the mbuf, BSD, IOKit, libkern and Mach KPIs
# 10.4 introduced (libraries-kpi.xml, 8.0.0: Darwin 8), which every release since keeps.
#   i386, GCC 4.0 and the 10.4u SDK: the 32-bit kernels of 10.4 to 10.7. A relocatable object
#     (-r), as kexts were before 10.6; 10.4 and 10.5 load nothing else.
#   x86_64, GCC 4.2 and the 10.6 SDK: the 64-bit kernels of 10.6 on, the only kind from 10.8. A
#     kext bundle (-Xlinker -kext, MH_KEXT_BUNDLE), as 10.6 introduced them for that kernel.
# SDKs from Xcode.mpkg are in /Developer/SDKs, from its packages installed one by one in /SDKs.
KPI_SDKS      ?= $(firstword $(wildcard /Developer/SDKs /SDKs))
KPI_CC_I386   ?= gcc-4.0
KPI_CC_X86_64 ?= gcc-4.2
KPI_SDK_I386  ?= $(KPI_SDKS)/MacOSX10.4u.sdk
KPI_SDK_X86_64 ?= $(KPI_SDKS)/MacOSX10.6.sdk
KPI_CFLAGS   = -fno-common -nostdinc -fno-builtin -finline -fno-keep-inline-functions -O2 -Wall -static \
               -DKERNEL -DAPPLE -DNeXT -DVIRTIONET_KPI -DVIRTIONET_VERSION='"$(VERSION)"'
KPI_CXXFLAGS = -x c++ $(KPI_CFLAGS) -fno-rtti -fno-exceptions -fcheck-new -fapple-kext
KPI_SRCS = VirtioNet.cpp VirtioNet_info.c

build/kpi/VirtioNet.i386: $(KPI_SRCS) VirtioNet.h
	mkdir -p build/kpi
	$(KPI_CC_I386) -arch i386 -mmacosx-version-min=10.4 $(KPI_CXXFLAGS) -I$(KPI_SDK_I386)/System/Library/Frameworks/Kernel.framework/Headers \
	   -c VirtioNet.cpp -o build/kpi/VirtioNet.i386.o
	$(KPI_CC_I386) -arch i386 -mmacosx-version-min=10.4 $(KPI_CFLAGS) -I$(KPI_SDK_I386)/System/Library/Frameworks/Kernel.framework/Headers \
	   -c VirtioNet_info.c -o build/kpi/VirtioNet_info.i386.o
	$(KPI_CC_I386) -arch i386 -isysroot $(KPI_SDK_I386) -static -nostdlib -r -o $@ \
	   build/kpi/VirtioNet_info.i386.o build/kpi/VirtioNet.i386.o -lkmodc++ -lkmod -lcc_kext

build/kpi/VirtioNet.x86_64: $(KPI_SRCS) VirtioNet.h
	mkdir -p build/kpi
	$(KPI_CC_X86_64) -arch x86_64 -mmacosx-version-min=10.6 -mkernel $(KPI_CXXFLAGS) -I$(KPI_SDK_X86_64)/System/Library/Frameworks/Kernel.framework/Headers \
	   -c VirtioNet.cpp -o build/kpi/VirtioNet.x86_64.o
	$(KPI_CC_X86_64) -arch x86_64 -mmacosx-version-min=10.6 -mkernel $(KPI_CFLAGS) -I$(KPI_SDK_X86_64)/System/Library/Frameworks/Kernel.framework/Headers \
	   -c VirtioNet_info.c -o build/kpi/VirtioNet_info.x86_64.o
	$(KPI_CC_X86_64) -arch x86_64 -isysroot $(KPI_SDK_X86_64) -mmacosx-version-min=10.6 -Xlinker -kext -nostdlib -o $@ \
	   build/kpi/VirtioNet_info.x86_64.o build/kpi/VirtioNet.x86_64.o -lkmodc++ -lkmod -lcc_kext

build/kpi/VirtioNet: build/kpi/VirtioNet.i386 build/kpi/VirtioNet.x86_64
	lipo -create -output $@ build/kpi/VirtioNet.i386 build/kpi/VirtioNet.x86_64

# What the Intel releases are given, by the oldest release each serves: 10.6/ the universal kext,
# 10.4/ its i386 slice alone. 10.5 refuses the universal one -- its kextload says "Executable file
# doesn't contain kernel extension code" of a binary whose x86_64 slice is a kext bundle, a file
# type it does not know (live, 10.5.8, 2026-10-10), and loads the thinned one -- while 10.6 and
# later need that slice. 10.4's tools pass over it, but are given the same as 10.5.
#   make ABI=kpi intel
intel: $(KEXT)
	rm -rf build/intel
	mkdir -p build/intel/10.4 build/intel/10.6
	cp -R $(KEXT) build/intel/10.6/
	cp -R $(KEXT) build/intel/10.4/
	lipo -thin i386 -output build/intel/10.4/VirtioNet.kext/Contents/MacOS/VirtioNet $(KEXT)/Contents/MacOS/VirtioNet

ifneq ($(ABI),kpi)
$(BUILD)/VirtioNet.o: VirtioNet.cpp VirtioNet.h
	mkdir -p $(BUILD)
	cc $(CXXFLAGS) -x c++ -c VirtioNet.cpp -o $@

$(BUILD)/VirtioNet_info.o: VirtioNet_info.c
	mkdir -p $(BUILD)
	cc $(CFLAGS) -c VirtioNet_info.c -o $@

# Objects before the libraries, as for VirtioConsole: otherwise libkmod's __start is taken and no
# constructor registers the driver's class.
$(BUILD)/VirtioNet: $(BUILD)/VirtioNet.o $(BUILD)/VirtioNet_info.o
	cc -arch ppc -static -nostdlib -r -o $@ $(BUILD)/VirtioNet_info.o $(BUILD)/VirtioNet.o \
	   $(addprefix -L,$(KMOD_LIB_DIR)) -lkmodc++ -lkmod -lcc_kext
endif

# perl rather than sed: 10.1's sed cannot put a file's lines into a replacement.
# The comments of Info.plist.in are dropped from the built plist: BootX, which loads the kexts a boot
# needs before the root is mounted, ends a <dict> at an XML comment, and then loads nothing of the kext
# (live, VirtioBlock on 10.0.4 booting from virtio-blk, 2026-10-03; BootX-36 drivers.c ParseTagList).
$(KEXT): $(BUILD)/VirtioNet Info.plist.in libraries-$(ABI).xml
	rm -rf $(KEXT)
	mkdir -p $(KEXT)/Contents/MacOS
	cp $(BUILD)/VirtioNet $(KEXT)/Contents/MacOS/VirtioNet
	perl -pe 's/\@VERSION\@/$(VERSION)/g; if (/\@LIBRARIES\@/) { open(L, "libraries-$(ABI).xml") or die; local $$/; $$_ = <L>; }' Info.plist.in | perl -0pe 's/[ \t]*<!--.*?-->\n?//gs' > $(KEXT)/Contents/Info.plist

clean:
	rm -rf build

.PHONY: all intel clean
