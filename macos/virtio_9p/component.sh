# What ci/ builds for this repository. KEXT is the bundle's name, PKG the package's (the manifest's "name"),
# PACKAGE the registry name. VARIANTS: gcc2-10.0 and gcc2-10.1 are GCC 2.95.2 with that release's headers (the
# side toolkits), gcc3-10.3 is Panther's own. EXTRAS are built beside the kext in build/<abi>/, EXTRAS_TOP in build/.
KEXT=Virtio9P
PKG=Virtio9P
PACKAGE=virtio9p
VARIANTS="gcc2-10.0 gcc2-10.1 gcc3-10.3"
EXTRAS="mount_9p"
EXTRAS_TOP=""
CHECK_PATHS="./System/Library/Extensions/Virtio9P.kext/Contents/MacOS/Virtio9P ./sbin/mount_9p"
