#!/bin/sh
# Runs in the Panther guest: guest-build.sh <variant> <share job dir>
#
# The component (the driver's component.sh) says what is built; the share job dir holds src.tgz, side-toolkit.sh and
# component.sh.
#
# variant   gcc2-10.0   GCC 2.95.2 with the 10.0 toolkit's headers    (/opt/toolkit-10.0, side-toolkit.sh)
#           gcc2-10.1   GCC 2.95.2 with the 10.1 toolkit's headers    (/opt/toolkit-10.1)
#           gcc3-10.3   Panther's own GCC 3.3 and headers             (covers 10.2 and 10.3 in the ladder)
#           intel       Xcode 3.2.6's GCC 4.0/4.2 and 10.4u-10.6 SDKs  (the Snow Leopard guest, for Intel 10.4 on):
#                       component.sh's INTEL_MAKE and INTEL_ARTIFACT, all to <share>/out/intel.tgz with a BUILD-INFO
#
# The finished kext and the component's extras are tarred to <share>/out/<variant>.tgz.
# Built on the guest's own disk: the 9p share refuses the linker's chmod.
set -e
variant=$1
share=$2
. "$share/component.sh"
w=/tmp/build-$variant-$$
rm -rf "$w"; mkdir -p "$w/src"
cd "$w/src"
gzip -dc "$share/src.tgz" | tar xf -
case "$variant" in
gcc2-*)
  rel=${variant#gcc2-}
  eval "`sh $share/side-toolkit.sh env $rel $w`"
  abi=gcc2
  make ABI=gcc2 KERNEL_HEADERS=$KERNEL_HEADERS KMOD_LIB_DIR=$KMOD_LIB_DIR VERSION=$VERSION
  ;;
gcc3-*)
  abi=gcc3
  make ABI=gcc3 VERSION=$VERSION
  ;;
intel)
  # What component.sh names: INTEL_MAKE, what make is run with, and INTEL_ARTIFACT, the files the build
  # leaves -- a path ending in / for a directory's contents -- as the Intel tarballs have always held them
  # (steigr/qemu-vm-container-macos-guest's guest/ladder/intel-build.sh took the same two).
  : "${INTEL_MAKE:?component.sh names no INTEL_MAKE: this driver has no Intel build}"
  make $INTEL_MAKE VERSION=$VERSION
  o=$w/intel-out
  rm -rf "$o"; mkdir -p "$o"
  for a in $INTEL_ARTIFACT; do
    case "$a" in
    */) (cd "$a" && tar cf - .) | (cd "$o" && tar xf -) ;;
    *) cp -R "$a" "$o/" ;;
    esac
  done
  {
    echo "$KEXT $VERSION, make $INTEL_MAKE"
    echo "source: ${SOURCE:-unknown}"
    echo "built on: `sw_vers -productVersion` `sw_vers -buildVersion`"
    gcc-4.0 --version | head -1
    gcc-4.2 --version | head -1
    for k in `cd "$o" && find . -name "$KEXT" -path "*/MacOS/*"`; do
      echo "kext $k: `lipo -info "$o/$k" | sed 's/.*: //'`"
    done
  } > "$o/BUILD-INFO"
  tar -C "$o" -czf "$share/out/intel.tgz" .
  cd /
  rm -rf "$w"
  echo "built intel"
  exit 0
  ;;
*) echo "unknown variant $variant" >&2; exit 2 ;;
esac
# One file onto the share: the share lets the guest's account create files in a directory the runner made
# for everyone, but not directories in one the guest made ("cp: Permission denied", macos/virtio9p run 1).
files="$KEXT.kext"
for e in $EXTRAS; do files="$files $e"; done
tar -C "build/$abi" -czf "$share/out/$variant.tgz" $files
# Extras the Makefile builds once for every ABI, outside build/<abi>/
if [ -n "$EXTRAS_TOP" ]; then
  tar -C build -czf "$share/out/$variant-top.tgz" $EXTRAS_TOP
fi
cd /
rm -rf "$w"
echo "built $variant"
