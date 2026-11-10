# Builds VirtioVsock.kext inside a Mac OS X guest, with that guest's own Developer Tools. On PowerPC
# the way ../ppc/virtioconsole is built (see its Makefile for where every compiler flag comes from and
# the failure behind each); on Intel against the kernel programming interfaces of 10.4 on, as
# ../virtionet is:
#
#   make ABI=gcc2   on 10.1 with the December 2001 tools (GCC 2.95): for 10.0 and 10.1, PowerPC
#   make ABI=gcc3   on 10.2 with the December 2002 tools (GCC 3):    for 10.2 and 10.3, PowerPC
#   make ABI=kpi    on 10.6 with Xcode 3.2.6:                       for 10.4 to 10.15, Intel
#
# Leaves build/<ABI>/VirtioVsock.kext; `make ABI=kpi intel` also the two Intel bundles and an Intel
# vsockcat, build/intel.

VERSION ?= 0.2.3
ABI     ?= gcc2

# As for VirtioConsole, the GCC 2.95 build must see 10.0's Kernel.framework and kmod libraries,
# not 10.1's, or the kext imports what 10.0's kernel lacks:
#   make ABI=gcc2 KERNEL_HEADERS=<its Kernel.framework/Headers> KMOD_LIB_DIR=<its libkmod*.a, libcc_kext.a>
KERNEL_HEADERS ?= /System/Library/Frameworks/Kernel.framework/Headers
KMOD_LIB_DIR   ?=

CFLAGS = -arch ppc -fno-common -nostdinc -O2 -Wall -I$(KERNEL_HEADERS) -I$(KERNEL_HEADERS)/bsd \
         -fno-builtin -finline -fno-keep-inline-functions -force_cpusubtype_ALL -msoft-float -mlong-branch \
         -static -DKERNEL -DKERNEL_PRIVATE -DDRIVER_PRIVATE -DAPPLE -DNeXT \
         -DVIRTIOVSOCK_VERSION='"$(VERSION)"'
CXXFLAGS_gcc2 = -fvtable-thunks -findirect-virtual-calls
CXXFLAGS_gcc3 = -fapple-kext
CXXFLAGS = $(CFLAGS) -fno-rtti -fno-exceptions -fcheck-new $(CXXFLAGS_$(ABI))

# OSBundleLibraries come from libraries-$(ABI).xml, as VirtioConsole's, without a family of its own:
# the driver is an IOService on IOPCIFamily with an IOUserClient, both in IOKit proper. No
# com.apple.kernel.bsd: the driver uses no BSD symbol, and no one version of it satisfies both 10.0
# and 10.1 (../virtio9p).

BUILD = build/$(ABI)
KEXT  = $(BUILD)/VirtioVsock.kext

all: $(KEXT) tools

# The Intel build, as VirtioNet's (../virtionet/Makefile says where each choice comes from): one
# binary of two slices against the KPIs 10.4 introduced (libraries-kpi.xml, 8.0.0: Darwin 8).
#   i386, GCC 4.0 and the 10.4u SDK: the 32-bit kernels of 10.4 to 10.7, a relocatable object (-r).
#   x86_64, GCC 4.2 and the 10.6 SDK: the 64-bit kernels of 10.6 on, a kext bundle (-Xlinker -kext).
# The source is the PowerPC one's; what differs on Intel -- registers in the processor's own order,
# mfence, a DMA address's high half on a 64-bit kernel -- is chosen in it by architecture.
KPI_SDKS      ?= $(firstword $(wildcard /Developer/SDKs /SDKs))
KPI_CC_I386   ?= gcc-4.0
KPI_CC_X86_64 ?= gcc-4.2
KPI_SDK_I386  ?= $(KPI_SDKS)/MacOSX10.4u.sdk
KPI_SDK_X86_64 ?= $(KPI_SDKS)/MacOSX10.6.sdk
KPI_CFLAGS   = -fno-common -nostdinc -fno-builtin -finline -fno-keep-inline-functions -O2 -Wall -static \
               -DKERNEL -DAPPLE -DNeXT -DVIRTIOVSOCK_VERSION='"$(VERSION)"'
KPI_CXXFLAGS = -x c++ $(KPI_CFLAGS) -fno-rtti -fno-exceptions -fcheck-new -fapple-kext
KPI_SRCS = VirtioVsock.cpp VirtioVsock_info.c

build/kpi/VirtioVsock.i386: $(KPI_SRCS) VirtioVsock.h VirtioVsockUser.h
	mkdir -p build/kpi
	$(KPI_CC_I386) -arch i386 -mmacosx-version-min=10.4 $(KPI_CXXFLAGS) -I$(KPI_SDK_I386)/System/Library/Frameworks/Kernel.framework/Headers \
	   -c VirtioVsock.cpp -o build/kpi/VirtioVsock.i386.o
	$(KPI_CC_I386) -arch i386 -mmacosx-version-min=10.4 $(KPI_CFLAGS) -I$(KPI_SDK_I386)/System/Library/Frameworks/Kernel.framework/Headers \
	   -c VirtioVsock_info.c -o build/kpi/VirtioVsock_info.i386.o
	$(KPI_CC_I386) -arch i386 -isysroot $(KPI_SDK_I386) -static -nostdlib -r -o $@ \
	   build/kpi/VirtioVsock_info.i386.o build/kpi/VirtioVsock.i386.o -lkmodc++ -lkmod -lcc_kext

build/kpi/VirtioVsock.x86_64: $(KPI_SRCS) VirtioVsock.h VirtioVsockUser.h
	mkdir -p build/kpi
	$(KPI_CC_X86_64) -arch x86_64 -mmacosx-version-min=10.6 -mkernel $(KPI_CXXFLAGS) -I$(KPI_SDK_X86_64)/System/Library/Frameworks/Kernel.framework/Headers \
	   -c VirtioVsock.cpp -o build/kpi/VirtioVsock.x86_64.o
	$(KPI_CC_X86_64) -arch x86_64 -mmacosx-version-min=10.6 -mkernel $(KPI_CFLAGS) -I$(KPI_SDK_X86_64)/System/Library/Frameworks/Kernel.framework/Headers \
	   -c VirtioVsock_info.c -o build/kpi/VirtioVsock_info.x86_64.o
	$(KPI_CC_X86_64) -arch x86_64 -isysroot $(KPI_SDK_X86_64) -mmacosx-version-min=10.6 -Xlinker -kext -nostdlib -o $@ \
	   build/kpi/VirtioVsock_info.x86_64.o build/kpi/VirtioVsock.x86_64.o -lkmodc++ -lkmod -lcc_kext

build/kpi/VirtioVsock: build/kpi/VirtioVsock.i386 build/kpi/VirtioVsock.x86_64
	lipo -create -output $@ build/kpi/VirtioVsock.i386 build/kpi/VirtioVsock.x86_64

# vsockcat for every Intel release, as the guest agent is built (../../software/qemu-guest-agent's
# "intel"): i386 against 10.4u, x86_64 against 10.5 with IOKit and CoreFoundation weak, for Tiger's
# kernel picks that slice and has no 64-bit IOKit.
build/intel/vsockcat: vsock.c vsock.h vsockcat.c VirtioVsockUser.h
	mkdir -p build/intel
	$(KPI_CC_I386) -O2 -Wall -I. -arch i386 -isysroot $(KPI_SDK_I386) -mmacosx-version-min=10.4 \
	   -o build/intel/vsockcat-i386 vsock.c vsockcat.c -framework IOKit -framework CoreFoundation
	$(KPI_CC_X86_64) -O2 -Wall -I. -arch x86_64 -isysroot $(KPI_SDKS)/MacOSX10.5.sdk -mmacosx-version-min=10.4 \
	   -o build/intel/vsockcat-x86_64 vsock.c vsockcat.c -weak_framework IOKit -weak_framework CoreFoundation
	lipo -create -output $@ build/intel/vsockcat-i386 build/intel/vsockcat-x86_64
	rm -f build/intel/vsockcat-i386 build/intel/vsockcat-x86_64

# What the Intel releases are given, by the oldest release each serves, as VirtioNet's: 10.6/ the
# universal kext, 10.4/ its i386 slice alone (10.5 refuses a universal kext whose x86_64 slice is a
# kext bundle; ../virtionet/Makefile).
#   make ABI=kpi intel
intel: $(KEXT) build/intel/vsockcat
	rm -rf build/intel/10.4 build/intel/10.6
	mkdir -p build/intel/10.4 build/intel/10.6
	cp -R $(KEXT) build/intel/10.6/
	cp -R $(KEXT) build/intel/10.4/
	lipo -thin i386 -output build/intel/10.4/VirtioVsock.kext/Contents/MacOS/VirtioVsock $(KEXT)/Contents/MacOS/VirtioVsock

# libvsock and vsockcat, for the guest the kext is built on (the same on every release; plain C):
#   make tools
tools: build/vsockcat

build/vsockcat: vsock.c vsock.h vsockcat.c VirtioVsockUser.h
	mkdir -p build
	cc -O2 -Wall -I. vsock.c vsockcat.c -framework IOKit -framework CoreFoundation -o $@

ifneq ($(ABI),kpi)
$(BUILD)/VirtioVsock.o: VirtioVsock.cpp VirtioVsock.h VirtioVsockUser.h
	mkdir -p $(BUILD)
	cc $(CXXFLAGS) -x c++ -c VirtioVsock.cpp -o $@

$(BUILD)/VirtioVsock_info.o: VirtioVsock_info.c
	mkdir -p $(BUILD)
	cc $(CFLAGS) -c VirtioVsock_info.c -o $@

# Objects before the libraries, as for VirtioConsole: otherwise libkmod's __start is taken and no
# constructor registers the driver's class.
$(BUILD)/VirtioVsock: $(BUILD)/VirtioVsock.o $(BUILD)/VirtioVsock_info.o
	cc -arch ppc -static -nostdlib -r -o $@ $(BUILD)/VirtioVsock_info.o $(BUILD)/VirtioVsock.o \
	   $(addprefix -L,$(KMOD_LIB_DIR)) -lkmodc++ -lkmod -lcc_kext
endif

# perl rather than sed: 10.1's sed cannot put a file's lines into a replacement.
# The comments of Info.plist.in are dropped from the built plist: BootX, which loads the kexts a boot
# needs before the root is mounted, ends a <dict> at an XML comment, and then loads nothing of the kext
# (live, VirtioBlock on 10.0.4 booting from virtio-blk, 2026-10-03; BootX-36 drivers.c ParseTagList).
$(KEXT): $(BUILD)/VirtioVsock Info.plist.in libraries-$(ABI).xml
	rm -rf $(KEXT)
	mkdir -p $(KEXT)/Contents/MacOS
	cp $(BUILD)/VirtioVsock $(KEXT)/Contents/MacOS/VirtioVsock
	perl -pe 's/\@VERSION\@/$(VERSION)/g; if (/\@LIBRARIES\@/) { open(L, "libraries-$(ABI).xml") or die; local $$/; $$_ = <L>; }' Info.plist.in | perl -0pe 's/[ \t]*<!--.*?-->\n?//gs' > $(KEXT)/Contents/Info.plist

clean:
	rm -rf build

.PHONY: all tools intel clean
