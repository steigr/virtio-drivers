# What macos/ci builds for this driver: the kext, its installer package (PKG) and what the check looks for.
KEXT=VirtioTablet
PKG=VirtioTablet
PACKAGE=macos_virtio_input
VARIANTS="gcc2-10.0 gcc2-10.1 gcc3-10.3"
EXTRAS=""
EXTRAS_TOP=""
CHECK_PATHS="./System/Library/Extensions/VirtioTablet.kext/Contents/MacOS/VirtioTablet"
