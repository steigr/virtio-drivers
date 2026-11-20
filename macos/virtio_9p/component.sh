# What macos/ci builds for this driver: the kext, its installer package (PKG) and what the check looks for.
# mount_9p is built beside the kext for every ABI (EXTRAS, build/<abi>/mount_9p) and installed as /sbin/mount_9p.
KEXT=Virtio9P
PKG=Virtio9P
PACKAGE=macos_virtio_9p
VARIANTS="gcc2-10.0 gcc2-10.1 gcc3-10.3"
EXTRAS="mount_9p"
EXTRAS_TOP=""
CHECK_PATHS="./System/Library/Extensions/Virtio9P.kext/Contents/MacOS/Virtio9P ./sbin/mount_9p"
