# What macos/ci builds for this driver: the kext, its installer package (PKG) and what the check looks for.
KEXT=VirtioConsole
PKG=VirtioConsole
PACKAGE=macos_virtio_console
VARIANTS="gcc2-10.0 gcc2-10.1 gcc3-10.3"
EXTRAS=""
EXTRAS_TOP=""
CHECK_PATHS="./System/Library/Extensions/VirtioConsole.kext/Contents/MacOS/VirtioConsole"
# The Intel build (macos/ci/guest-build.sh's variant intel, on the Snow Leopard runner): make's arguments, and what it
# leaves for the tarball -- a trailing / for a directory's contents.
INTEL_MAKE="ABI=kpi intel"
INTEL_ARTIFACT="build/intel/"
