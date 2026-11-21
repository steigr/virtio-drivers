# What macos/ci builds for this driver: the kext, its installer package (PKG) and what the check looks for.
KEXT=VirtioRand
PKG=VirtioRand
PACKAGE=macos_virtio_rng
VARIANTS="gcc2-10.0 gcc2-10.1 gcc3-10.2 gcc3-10.3"
# 10.0 has no /dev/random and no random_write: there the driver is the device (its Makefile's RANDOM_DEVICE).
MAKE_gcc2_10_0="RANDOM_DEVICE=1"
EXTRAS=""
EXTRAS_TOP=""
CHECK_PATHS="./System/Library/Extensions/VirtioRand.kext/Contents/MacOS/VirtioRand"
