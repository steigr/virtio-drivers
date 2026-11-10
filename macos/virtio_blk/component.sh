# What macos/ci builds for this driver: the kext, its installer package (PKG) and what the check looks for.
KEXT=VirtioBlock
PKG=VirtioBlock
PACKAGE=macos_virtio_blk
VARIANTS="gcc2-10.0 gcc2-10.1 gcc3-10.3"
EXTRAS=""
EXTRAS_TOP=""
CHECK_PATHS="./System/Library/Extensions/VirtioBlock.kext/Contents/MacOS/VirtioBlock"
