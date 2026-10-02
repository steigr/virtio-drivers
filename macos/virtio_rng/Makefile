# Builds VirtioRand.kext inside a PowerPC Mac OS X guest, with that guest's own Developer Tools,
# the way ../virtioconsole is built (see its Makefile for where every compiler flag comes from and
# the failure behind each):
#
#   make ABI=gcc2   on 10.1 with the December 2001 tools (GCC 2.95): for 10.0 and 10.1
#   make ABI=gcc3   on 10.2 with the December 2002 tools (GCC 3):    for 10.2 on
#
# Leaves build/<ABI>/VirtioRand.kext.

VERSION ?= 0.1.0
ABI     ?= gcc2

# As for VirtioConsole, the GCC 2.95 build must see 10.0's Kernel.framework and kmod libraries,
# not 10.1's, or the kext imports what 10.0's kernel lacks:
#   make ABI=gcc2 KERNEL_HEADERS=<its Kernel.framework/Headers> KMOD_LIB_DIR=<its libkmod*.a, libcc_kext.a>
KERNEL_HEADERS ?= /System/Library/Frameworks/Kernel.framework/Headers
KMOD_LIB_DIR   ?=

CFLAGS = -arch ppc -fno-common -nostdinc -O2 -Wall -I$(KERNEL_HEADERS) -I$(KERNEL_HEADERS)/bsd \
         -fno-builtin -finline -fno-keep-inline-functions -force_cpusubtype_ALL -msoft-float -mlong-branch \
         -static -DKERNEL -DKERNEL_PRIVATE -DDRIVER_PRIVATE -DAPPLE -DNeXT \
         -DVIRTIORAND_VERSION='"$(VERSION)"'
CXXFLAGS_gcc2 = -fvtable-thunks -findirect-virtual-calls
CXXFLAGS_gcc3 = -fapple-kext
CXXFLAGS = $(CFLAGS) -fno-rtti -fno-exceptions -fcheck-new $(CXXFLAGS_$(ABI))

# OSBundleLibraries come from libraries-$(ABI).xml, as VirtioConsole's, without IOSerialFamily: the driver
# is an IOService on IOPCIFamily and nothing more. No com.apple.kernel.bsd although it calls the BSD
# kernel's random_write: no one version of it satisfies both 10.0 and 10.1 (../virtio9p), and
# virtio9p shows the symbols resolve without the declaration.

BUILD = build/$(ABI)
KEXT  = $(BUILD)/VirtioRand.kext

all: $(KEXT)

$(BUILD)/VirtioRand.o: VirtioRand.cpp VirtioRand.h
	mkdir -p $(BUILD)
	cc $(CXXFLAGS) -x c++ -c VirtioRand.cpp -o $@

$(BUILD)/VirtioRand_info.o: VirtioRand_info.c
	mkdir -p $(BUILD)
	cc $(CFLAGS) -c VirtioRand_info.c -o $@

# Objects before the libraries, as for VirtioConsole: otherwise libkmod's __start is taken and no
# constructor registers the driver's class.
$(BUILD)/VirtioRand: $(BUILD)/VirtioRand.o $(BUILD)/VirtioRand_info.o
	cc -arch ppc -static -nostdlib -r -o $@ $(BUILD)/VirtioRand_info.o $(BUILD)/VirtioRand.o \
	   $(addprefix -L,$(KMOD_LIB_DIR)) -lkmodc++ -lkmod -lcc_kext

# perl rather than sed: 10.1's sed cannot put a file's lines into a replacement.
$(KEXT): $(BUILD)/VirtioRand Info.plist.in libraries-$(ABI).xml
	rm -rf $(KEXT)
	mkdir -p $(KEXT)/Contents/MacOS
	cp $(BUILD)/VirtioRand $(KEXT)/Contents/MacOS/VirtioRand
	perl -pe 's/\@VERSION\@/$(VERSION)/g; if (/\@LIBRARIES\@/) { open(L, "libraries-$(ABI).xml") or die; local $$/; $$_ = <L>; }' Info.plist.in > $(KEXT)/Contents/Info.plist

clean:
	rm -rf build

.PHONY: all clean
