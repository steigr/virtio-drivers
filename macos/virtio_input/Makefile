# Builds VirtioTablet.kext inside a PowerPC Mac OS X guest, with that guest's own Developer Tools,
# the way ../virtioconsole is built (see its Makefile for where every compiler flag comes from and
# the failure behind each):
#
#   make ABI=gcc2   on 10.0 or 10.1 (GCC 2.95): for 10.0 and 10.1
#   make ABI=gcc3   on 10.2 or 10.3 (GCC 3):    for 10.2 on
#
# Leaves build/<ABI>/VirtioTablet.kext.

VERSION ?= 0.1.0
ABI     ?= gcc2

# As for VirtioConsole, a GCC 2.95 build for both 10.0 and 10.1 must see 10.0's Kernel.framework
# and kmod libraries, not 10.1's, or the kext imports what 10.0's kernel lacks:
#   make ABI=gcc2 KERNEL_HEADERS=<its Kernel.framework/Headers> KMOD_LIB_DIR=<its libkmod*.a, libcc_kext.a>
KERNEL_HEADERS ?= /System/Library/Frameworks/Kernel.framework/Headers
KMOD_LIB_DIR   ?=

CFLAGS = -arch ppc -fno-common -nostdinc -O2 -Wall -I$(KERNEL_HEADERS) -I$(KERNEL_HEADERS)/bsd \
         -fno-builtin -finline -fno-keep-inline-functions -force_cpusubtype_ALL -msoft-float -mlong-branch \
         -static -DKERNEL -DKERNEL_PRIVATE -DDRIVER_PRIVATE -DAPPLE -DNeXT \
         -DVIRTIOTABLET_VERSION='"$(VERSION)"'
CXXFLAGS_gcc2 = -fvtable-thunks -findirect-virtual-calls
CXXFLAGS_gcc3 = -fapple-kext
CXXFLAGS = $(CFLAGS) -fno-rtti -fno-exceptions -fcheck-new $(CXXFLAGS_$(ABI))

# OSBundleLibraries come from libraries-$(ABI).xml, as VirtioConsole's, with IOHIDFamily for
# IOHIPointing, the class the driver derives from. It is not in the same place in any two releases
# (nm of each one's binaries): 10.0's kernel itself, 10.1's com.apple.iokit.IOHIDSystem kext,
# 10.2's and 10.3's com.apple.iokit.IOHIDFamily. IOHIDFamily is the one bundle all four have, and
# where it is not IOHIPointing's home it leads there: 10.1's depends on IOHIDSystem, and 10.0's on
# the kernel. com.apple.iokit.IOHIDSystem itself cannot be named: 10.0 has no such bundle.

BUILD = build/$(ABI)
KEXT  = $(BUILD)/VirtioTablet.kext

all: $(KEXT)

$(BUILD)/VirtioTablet.o: VirtioTablet.cpp VirtioTablet.h
	mkdir -p $(BUILD)
	cc $(CXXFLAGS) -x c++ -c VirtioTablet.cpp -o $@

$(BUILD)/VirtioTablet_info.o: VirtioTablet_info.c
	mkdir -p $(BUILD)
	cc $(CFLAGS) -c VirtioTablet_info.c -o $@

# Objects before the libraries, as for VirtioConsole: otherwise libkmod's __start is taken and no
# constructor registers the driver's class.
$(BUILD)/VirtioTablet: $(BUILD)/VirtioTablet.o $(BUILD)/VirtioTablet_info.o
	cc -arch ppc -static -nostdlib -r -o $@ $(BUILD)/VirtioTablet_info.o $(BUILD)/VirtioTablet.o \
	   $(addprefix -L,$(KMOD_LIB_DIR)) -lkmodc++ -lkmod -lcc_kext

# perl rather than sed: 10.1's sed cannot put a file's lines into a replacement.
# The comments of Info.plist.in are dropped from the built plist: BootX ends a <dict> at an XML
# comment (see ../virtiorand/Makefile).
$(KEXT): $(BUILD)/VirtioTablet Info.plist.in libraries-$(ABI).xml
	rm -rf $(KEXT)
	mkdir -p $(KEXT)/Contents/MacOS
	cp $(BUILD)/VirtioTablet $(KEXT)/Contents/MacOS/VirtioTablet
	perl -pe 's/\@VERSION\@/$(VERSION)/g; if (/\@LIBRARIES\@/) { open(L, "libraries-$(ABI).xml") or die; local $$/; $$_ = <L>; }' Info.plist.in | perl -0pe 's/[ \t]*<!--.*?-->\n?//gs' > $(KEXT)/Contents/Info.plist

clean:
	rm -rf build

.PHONY: all clean
