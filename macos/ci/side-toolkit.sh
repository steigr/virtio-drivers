# Older Developer Tools beside a newer system's own, to build for 10.0 and 10.1 on Panther (10.3.9).
# Run in the guest as root (sh), with the discs' package archives somewhere the guest can read, e.g.
# the 9p share:
#
#   side-toolkit.sh unpack 10.0 /Volumes/share/tk/dev10.0.pax.gz       # 691-2963-A: Developer.pkg's pax.gz
#   side-toolkit.sh unpack 10.1 /Volumes/share/tk/devtools10.1.pax.gz /Volumes/share/tk/devsdk10.1.pax.gz
#                                                                       # 691-3451-A: DevTools.pkg and DevSDK.pkg
#   side-toolkit.sh env 10.0 /tmp/work                                   # a cc for 10.0's compiler; prints the make words
#
# The packages are unpacked into /opt/toolkit-<release> and nothing else of the system is touched: over
# "/" they would replace Panther's GCC 3.3, its headers and its Kernel.framework. Used by path:
#
#   eval "`side-toolkit.sh env 10.0 /tmp/work`"      # sets PATH, KERNEL_HEADERS, KMOD_LIB_DIR
#   make ABI=gcc2 KERNEL_HEADERS=$KERNEL_HEADERS KMOD_LIB_DIR=$KMOD_LIB_DIR
#
# Proven 2026-10-19 on a Panther guest: drivers/ppc/virtioconsole at b838142 built this way came out
# byte for byte as the bootstrap ladder's, built in 10.0.4 and 10.1.5 guests (cksum 427663139, 51304
# bytes for 10.0; 1978860130, 95616 for 10.1). What it took:
#   - the packages' postflight makes Kernel.framework/Versions/Current; it did not run, so the headers
#     are named by Versions/A;
#   - 2.95.2's cc finds cc1, cc1plus and as under /usr/libexec/gcc, which is Panther's: -B says where
#     the toolkit's are;
#   - its cpp-precomp reads /usr/libexec/gcc/darwin/ppc/default/specs, which Panther has no copy of:
#     "malformed spec file" on the first C file. -no-cpp-precomp preprocesses with cc1 itself.
# Not tried: userland software (it links the toolkit's SDK libraries, not the running system's).

set -e
case "$1" in
unpack)
  rel=$2; shift 2
  prefix=/opt/toolkit-$rel
  mkdir -p "$prefix"
  for a in "$@"; do
    echo "side-toolkit: $a into $prefix"
    gzip -dc "$a" | (cd "$prefix" && /bin/pax -r -p e)
  done
  ;;
env)
  rel=$2; work=$3
  t=/opt/toolkit-$rel
  [ -x "$t/usr/bin/cc" ] || { echo "side-toolkit: no toolkit at $t" >&2; exit 1; }
  mkdir -p "$work/bin"
  cat > "$work/bin/cc" <<WRAP
#!/bin/sh
exec $t/usr/bin/cc -B$t/usr/libexec/gcc/darwin/ppc/2.95.2/ -B$t/usr/libexec/gcc/darwin/ppc/ -no-cpp-precomp "\$@"
WRAP
  chmod +x "$work/bin/cc"
  echo "PATH=$work/bin:\$PATH; KERNEL_HEADERS=$t/System/Library/Frameworks/Kernel.framework/Versions/A/Headers; KMOD_LIB_DIR=$t/usr/lib; export PATH KERNEL_HEADERS KMOD_LIB_DIR"
  ;;
*)
  echo "usage: side-toolkit.sh unpack <release> <archive.pax.gz>... | env <release> <workdir>" >&2
  exit 2
  ;;
esac
