# Builds VirtioBlock.kext inside a Mac OS X guest, with that guest's own Developer Tools.
#
#   make ABI=gcc2    on 10.1 with the December 2001 tools (GCC 2.95): for PowerPC 10.0 and 10.1
#   make ABI=gcc3    on 10.2 with the December 2002 tools (GCC 3):    for PowerPC 10.2 on
#   make ABI=intel   on 10.6 with Xcode 3.2.6 and its 10.4u and 10.6 SDKs: for Intel 10.4 to 10.15
#
# The PowerPC builds are ../ppc/virtioconsole's (see its Makefile for where every compiler flag
# comes from and the failure behind each). Leaves build/<ABI>/VirtioBlock.kext.

VERSION ?= 0.2.5
ABI     ?= gcc2

# As for VirtioConsole, the GCC 2.95 build must see 10.0's Kernel.framework and kmod libraries,
# not 10.1's, or the kext imports what 10.0's kernel lacks:
#   make ABI=gcc2 KERNEL_HEADERS=<its Kernel.framework/Headers> KMOD_LIB_DIR=<its libkmod*.a, libcc_kext.a>
KERNEL_HEADERS ?= /System/Library/Frameworks/Kernel.framework/Headers
KMOD_LIB_DIR   ?=

CFLAGS = -arch ppc -fno-common -nostdinc -O2 -Wall -I$(KERNEL_HEADERS) -I$(KERNEL_HEADERS)/bsd \
         -fno-builtin -finline -fno-keep-inline-functions -force_cpusubtype_ALL -msoft-float -mlong-branch \
         -static -DKERNEL -DKERNEL_PRIVATE -DDRIVER_PRIVATE -DAPPLE -DNeXT \
         -DVIRTIOBLOCK_VERSION='"$(VERSION)"'
# No -fpermissive, unlike VirtioNet: that was for netinet/in.h, which only IOEthernetController.h
# pulls in.
CXXFLAGS_gcc2 = -fvtable-thunks -findirect-virtual-calls
CXXFLAGS_gcc3 = -fapple-kext
CXXFLAGS = $(CFLAGS) -fno-rtti -fno-exceptions -fcheck-new $(CXXFLAGS_$(ABI))

# OSBundleLibraries come from libraries-$(ABI).xml, as VirtioConsole's, with IOStorageFamily in
# place of IOSerialFamily; 1.0.0b1 is what VirtioNet's IONetworkingFamily is compatible back to,
# and is assumed here until read off the guests (README, "Not yet"). No com.apple.kernel.bsd: the
# driver uses no BSD symbol, and no one version of it satisfies both 10.0 and 10.1 (../virtio9p).

BUILD = build/$(ABI)
KEXT  = $(BUILD)/VirtioBlock.kext

all: $(KEXT)

# Intel: one kext of two slices, joined with lipo. i386 against the 10.4u SDK with GCC 4.0, which
# every 32-bit kernel loads (10.4 to 10.7, linked relocatable as 10.4's kextload wants it); x86_64
# against the 10.6 SDK with GCC 4.2 (-mkernel: no red zone, no SSE, no builtins), which every
# 64-bit kernel loads (10.6 to 10.15) -- the 64-bit kernels have their own IOBlockStorageDevice
# (VirtioBlock.h). Both declare the KPI libraries (libraries-intel.xml), which go back to 10.4.
INTEL_SDKS       ?= $(firstword $(wildcard /Developer/SDKs /SDKs))
INTEL_DEFS        = -DKERNEL -DKERNEL_PRIVATE -DDRIVER_PRIVATE -DAPPLE -DNeXT -DVIRTIOBLOCK_VERSION='"$(VERSION)"'
INTEL_CXXFLAGS    = -fno-common -nostdinc -O2 -Wall -Wno-deprecated-declarations -fno-builtin -static \
                    -fapple-kext -fno-rtti -fno-exceptions -fcheck-new $(INTEL_DEFS)
I386_SDK         ?= $(INTEL_SDKS)/MacOSX10.4u.sdk
X86_64_SDK       ?= $(INTEL_SDKS)/MacOSX10.6.sdk
I386_INC          = -I$(I386_SDK)/System/Library/Frameworks/Kernel.framework/Headers
X86_64_INC        = -I$(X86_64_SDK)/System/Library/Frameworks/Kernel.framework/Headers

build/intel/VirtioBlock.i386: VirtioBlock.cpp VirtioBlock.h VirtioBlock_info.c
	mkdir -p build/intel
	gcc-4.0 -arch i386 -isysroot $(I386_SDK) -mmacosx-version-min=10.4 $(INTEL_CXXFLAGS) $(I386_INC) -x c++ -c VirtioBlock.cpp -o build/intel/VirtioBlock.i386.o
	gcc-4.0 -arch i386 -isysroot $(I386_SDK) -mmacosx-version-min=10.4 -fno-common -nostdinc -O2 -static $(INTEL_DEFS) $(I386_INC) -c VirtioBlock_info.c -o build/intel/VirtioBlock_info.i386.o
	gcc-4.0 -arch i386 -isysroot $(I386_SDK) -mmacosx-version-min=10.4 -static -nostdlib -r -o $@ \
	   build/intel/VirtioBlock_info.i386.o build/intel/VirtioBlock.i386.o -lkmodc++ -lkmod -lcc_kext

build/intel/VirtioBlock.x86_64: VirtioBlock.cpp VirtioBlock.h VirtioBlock_info.c
	mkdir -p build/intel
	gcc-4.2 -arch x86_64 -isysroot $(X86_64_SDK) -mmacosx-version-min=10.6 -mkernel $(INTEL_CXXFLAGS) $(X86_64_INC) -x c++ -c VirtioBlock.cpp -o build/intel/VirtioBlock.x86_64.o
	gcc-4.2 -arch x86_64 -isysroot $(X86_64_SDK) -mmacosx-version-min=10.6 -mkernel -fno-common -nostdinc -O2 $(INTEL_DEFS) $(X86_64_INC) -c VirtioBlock_info.c -o build/intel/VirtioBlock_info.x86_64.o
	gcc-4.2 -arch x86_64 -isysroot $(X86_64_SDK) -mmacosx-version-min=10.6 -nostdlib -Xlinker -kext -o $@ \
	   build/intel/VirtioBlock_info.x86_64.o build/intel/VirtioBlock.x86_64.o -lkmodc++ -lkmod -lcc_kext

build/intel/VirtioBlock: build/intel/VirtioBlock.i386 build/intel/VirtioBlock.x86_64
	lipo -create -output $@ build/intel/VirtioBlock.i386 build/intel/VirtioBlock.x86_64

$(BUILD)/VirtioBlock.o: VirtioBlock.cpp VirtioBlock.h
	mkdir -p $(BUILD)
	cc $(CXXFLAGS) -x c++ -c VirtioBlock.cpp -o $@

$(BUILD)/VirtioBlock_info.o: VirtioBlock_info.c
	mkdir -p $(BUILD)
	cc $(CFLAGS) -c VirtioBlock_info.c -o $@

# Objects before the libraries, as for VirtioConsole: otherwise libkmod's __start is taken and no
# constructor registers the driver's class. (The Intel slices keep the same order.)
ifneq ($(ABI),intel)
$(BUILD)/VirtioBlock: $(BUILD)/VirtioBlock.o $(BUILD)/VirtioBlock_info.o
	cc -arch ppc -static -nostdlib -r -o $@ $(BUILD)/VirtioBlock_info.o $(BUILD)/VirtioBlock.o \
	   $(addprefix -L,$(KMOD_LIB_DIR)) -lkmodc++ -lkmod -lcc_kext
endif

# perl rather than sed: 10.1's sed cannot put a file's lines into a replacement.
# The comments of Info.plist.in are dropped from the built plist: BootX, which loads the kexts a boot
# needs before the root is mounted, ends a <dict> at an XML comment, and then loads nothing of the kext
# (live, VirtioBlock on 10.0.4 booting from virtio-blk, 2026-10-03; BootX-36 drivers.c ParseTagList).
$(KEXT): $(BUILD)/VirtioBlock Info.plist.in libraries-$(ABI).xml
	rm -rf $(KEXT)
	mkdir -p $(KEXT)/Contents/MacOS
	cp $(BUILD)/VirtioBlock $(KEXT)/Contents/MacOS/VirtioBlock
	perl -pe 's/\@VERSION\@/$(VERSION)/g; if (/\@LIBRARIES\@/) { open(L, "libraries-$(ABI).xml") or die; local $$/; $$_ = <L>; }' Info.plist.in | perl -0pe 's/[ \t]*<!--.*?-->\n?//gs' > $(KEXT)/Contents/Info.plist

clean:
	rm -rf build

.PHONY: all clean
