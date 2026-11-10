# What macos/ci builds for this driver: the kext, its installer package (PKG) and what the check looks for. vsockcat is built once for
# every ABI (`make tools`, build/vsockcat).
KEXT=VirtioVsock
PKG=VirtioVsock
PACKAGE=macos_virtio_vsock
VARIANTS="gcc2-10.0 gcc2-10.1 gcc3-10.3"
EXTRAS=""
EXTRAS_TOP="vsockcat"
CHECK_PATHS="./System/Library/Extensions/VirtioVsock.kext/Contents/MacOS/VirtioVsock ./usr/local/bin/vsockcat"
