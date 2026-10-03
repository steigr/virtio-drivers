# Builds VirtioConsole.kext inside a PowerPC Mac OS X guest, with that guest's own Developer Tools:
#
#   make ABI=gcc2   on 10.1 with the December 2001 tools (GCC 2.95): for 10.0 and 10.1
#   make ABI=gcc3   on 10.2 with the December 2002 tools (GCC 3):    for 10.2 on
#
# The two compilers' C++ ABIs differ and a kernel extension links against the kernel's, so one
# binary cannot serve both. The flags are what Project Builder's kernel-extension rules pass
# (/Developer/Makefiles/pbx_jamfiles/ProjectBuilderJambase, KERNEL_MODULE = YES); for GCC 2.95 that
# includes -fvtable-thunks and -findirect-virtual-calls, which lay out and call virtual functions the
# way the 10.0 and 10.1 kernels were built to (read off the December 2001 tools, 2026-09-29); for
# GCC 3 it is -fapple-kext (C++ only; GCC 3.1's C compiler refuses it), without which the kext's
# vtables are not laid out as the kernel's and 10.2 refuses it: "io_virtio_VirtioConsole is not
# compatible with its superclass, IOSerialDriverSync superclass changed?" (live, 10.2, 2026-09-29;
# read off the December 2002 tools).

VERSION ?= 0.1.0
ABI     ?= gcc2

# A GCC 2.95 build for 10.0 must see 10.0's headers and libraries, not 10.1's: built against
# 10.1's Kernel.framework the kext imports eight IOService and IORegistryEntry virtuals 10.1 added
# (copyProperty, requestTerminate, ...), and 10.0's kernel refuses to link it (live, 2026-09-29).
# 10.0's are in the Developer package of the March 2001 tools CD (691-2963-A); build with
#   make ABI=gcc2 KERNEL_HEADERS=<its Kernel.framework/Headers> KMOD_LIB_DIR=<its libkmod*.a, libcc_kext.a>
KERNEL_HEADERS ?= /System/Library/Frameworks/Kernel.framework/Headers
KMOD_LIB_DIR   ?=

CFLAGS = -arch ppc -fno-common -nostdinc -O2 -Wall -I$(KERNEL_HEADERS) -I$(KERNEL_HEADERS)/bsd \
         -fno-builtin -finline -fno-keep-inline-functions -force_cpusubtype_ALL -msoft-float -mlong-branch \
         -static -DKERNEL -DKERNEL_PRIVATE -DDRIVER_PRIVATE -DAPPLE -DNeXT \
         -DVIRTIOCONSOLE_VERSION='"$(VERSION)"'
CXXFLAGS_gcc2 = -fvtable-thunks -findirect-virtual-calls
CXXFLAGS_gcc3 = -fapple-kext
CXXFLAGS = $(CFLAGS) -fno-rtti -fno-exceptions -fcheck-new $(CXXFLAGS_$(ABI))

# OSBundleLibraries come from libraries-$(ABI).xml: what each generation's kernel and families call
# themselves. 10.0's System.kext lists com.apple.kernel.* and com.apple.iokit.IOPCIFamily at
# 1.0.0b1, and IOSerialFamily is 1.0.4, compatible back to 1.0.4 (read off the 10.0 and 10.1 CDs,
# 2026-09-29). 10.2's (6C115) kernel libraries are 6.0, compatible back to 1.0.0b1, and GCC 3 kexts
# ask for 6.0 so that 10.0 and 10.1, whose C++ ABI they cannot link against, refuse them outright;
# its IOPCIFamily 1.2 and IOSerialFamily 6.0.1d19 are compatible back to 1.0.0b1 and 1.0.4.

BUILD = build/$(ABI)
KEXT  = $(BUILD)/VirtioConsole.kext

all: $(KEXT)

$(BUILD)/VirtioConsole.o: VirtioConsole.cpp VirtioConsole.h
	mkdir -p $(BUILD)
	cc $(CXXFLAGS) -x c++ -c VirtioConsole.cpp -o $@

$(BUILD)/VirtioConsole_info.o: VirtioConsole_info.c
	mkdir -p $(BUILD)
	cc $(CFLAGS) -c VirtioConsole_info.c -o $@

# The objects before the libraries: libkmodc++'s __start runs the C++ constructors that register
# the driver's class, and libkmod's does not. With -lkmodc++ first, as Project Builder's rule has
# it, nothing referenced __start yet, the linker took libkmod's, and IOKit could match the device
# and not make a driver for it -- "Couldn't alloc class io_virtio_VirtioConsole" (live, 10.1,
# 2026-09-29).
$(BUILD)/VirtioConsole: $(BUILD)/VirtioConsole.o $(BUILD)/VirtioConsole_info.o
	cc -arch ppc -static -nostdlib -r -o $@ $(BUILD)/VirtioConsole_info.o $(BUILD)/VirtioConsole.o \
	   $(addprefix -L,$(KMOD_LIB_DIR)) -lkmodc++ -lkmod -lcc_kext

# The bundle. perl rather than sed: 10.1's sed cannot put a file's lines into a replacement.
# The comments of Info.plist.in are dropped from the built plist: BootX, which loads the kexts a boot
# needs before the root is mounted, ends a <dict> at an XML comment, and then loads nothing of the kext
# (live, VirtioBlock on 10.0.4 booting from virtio-blk, 2026-10-03; BootX-36 drivers.c ParseTagList).
$(KEXT): $(BUILD)/VirtioConsole Info.plist.in libraries-$(ABI).xml
	rm -rf $(KEXT)
	mkdir -p $(KEXT)/Contents/MacOS
	cp $(BUILD)/VirtioConsole $(KEXT)/Contents/MacOS/VirtioConsole
	perl -pe 's/\@VERSION\@/$(VERSION)/g; if (/\@LIBRARIES\@/) { open(L, "libraries-$(ABI).xml") or die; local $$/; $$_ = <L>; }' Info.plist.in | perl -0pe 's/[ \t]*<!--.*?-->\n?//gs' > $(KEXT)/Contents/Info.plist

clean:
	rm -rf build

.PHONY: all clean
