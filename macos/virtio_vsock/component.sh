# What ci/ builds for this repository (see virtio9p's component.sh for the words). vsockcat is built once for
# every ABI (`make tools`, build/vsockcat).
KEXT=VirtioVsock
PKG=VirtioVsock
PACKAGE=virtiovsock
VARIANTS="gcc2-10.0 gcc2-10.1 gcc3-10.3"
EXTRAS=""
EXTRAS_TOP="vsockcat"
CHECK_PATHS="./System/Library/Extensions/VirtioVsock.kext/Contents/MacOS/VirtioVsock ./usr/local/bin/vsockcat"
