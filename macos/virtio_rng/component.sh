# What macos/ci builds for this driver: the kext, its installer package (PKG) and what the check looks for.
KEXT=VirtioRand
PKG=VirtioRand
PACKAGE=macos_virtio_rng
VARIANTS="gcc2-10.1 gcc3-10.3"
EXTRAS=""
EXTRAS_TOP=""
CHECK_PATHS="./System/Library/Extensions/VirtioRand.kext/Contents/MacOS/VirtioRand"
