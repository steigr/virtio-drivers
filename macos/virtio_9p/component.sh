# What macos/ci builds for this driver: the kext, its installer package (PKG) and what the check looks for.
# mount_9p is built beside the kext for every ABI (EXTRAS, build/<abi>/mount_9p) and installed as /sbin/mount_9p.
KEXT=Virtio9P
PKG=Virtio9P
PACKAGE=macos_virtio_9p
VARIANTS="gcc2-10.0 gcc2-10.1 gcc3-10.2 gcc3-10.3"
EXTRAS="mount_9p"
EXTRAS_TOP=""
CHECK_PATHS="./System/Library/Extensions/Virtio9P.kext/Contents/MacOS/Virtio9P ./sbin/mount_9p"
# The Intel build, in the Snow Leopard guest with Xcode 3.2.6: the KPI kext and mount_9p for 10.4 to 10.13,
# with a ppc slice for PowerPC 10.4 and 10.5.
INTEL_MAKE="ABI=kpi"
INTEL_ARTIFACT="build/kpi/Virtio9P.kext build/kpi/mount_9p"
