#!/bin/sh
# Install a package with inst in the IRIX guest, check what landed, and remove it
# again. Nothing is loaded: a build node should not run a driver under test, and
# may not have the device.
#
# virtio_9p is not removed: the CI steps run on the 9p share (/share), and without the package the
# guest cannot mount it again ("unknown filesystem type: 9p"), so the next job would fail. It
# stays installed as the newer version; the module that is loaded now keeps running.
#
#   test-package.sh <virtio_serial|virtio_9p> <tardist>
product=$1
tardist=$2
log=/var/tmp/$product-inst-$$.log
fail() { echo "PACKAGE TEST FAILED: $*"; exit 1; }
remove() { /usr/sbin/inst -a -R $product > $log 2>&1; }

# The module, a symbol ml needs from it, and everything else the package installs.
case "$product" in
virtio_serial)
	obj=/usr/lib/virtio_serial/virtio_serial.o; sym=virtio_serial_init
	files="/etc/init.d/virtio_serial"
	links="/etc/rc2.d/S98virtio_serial /etc/rc0.d/K02virtio_serial"
	;;
virtio_9p)
	obj=/usr/lib/virtio_9p/virtio_9p.o; sym=virtio_9p_vfsops
	files="/etc/init.d/virtio_9p /usr/etc/mount_9p"
	links="/etc/rc2.d/S98virtio_9p /etc/rc0.d/K02virtio_9p"
	keep=1
	;;
*)
	echo "usage: $0 <virtio_serial|virtio_9p> <tardist>"; exit 2
	;;
esac
[ -f "$tardist" ] || { echo "usage: $0 <virtio_serial|virtio_9p> <tardist>"; exit 2; }
libdir=`dirname $obj`

if [ -n "${keep:-}" ]; then trap 'rm -f $log' 0; else trap 'cd /; remove; rm -f $log' 0; fi
cd / || exit 1

/usr/sbin/inst -a -f $tardist -I $product > $log 2>&1
tail -3 $log
grep 'Installations and removals were successful' $log > /dev/null || { cat $log; fail "inst"; }
/usr/sbin/versions -n $product | cat

for f in $obj $files; do
	[ -f $f ] || fail "missing $f"
done
for f in $links; do
	[ -f $f ] || fail "$f does not lead to the init script"
done
# ml needs the symbol table and relocations, and this board's ABI.
/usr/bin/nm $obj 2>/dev/null | grep $sym > /dev/null || fail "$obj has no symbols"
case `uname -m` in
IP22) abi=N32 ;;
*)    abi=64-bit ;;
esac
/usr/bin/file $obj | cat
/usr/bin/file $obj | grep "$abi" > /dev/null || fail "$obj is not $abi"

if [ -n "${keep:-}" ]; then
	echo "PACKAGE TEST OK: $product (kept installed)"
	exit 0
fi
remove
grep 'Installations and removals were successful' $log > /dev/null || { cat $log; fail "inst -R"; }
for f in $obj $files $links; do
	[ ! -f $f ] || fail "$f left behind after removal"
done
[ ! -d $libdir ] || fail "$libdir left behind after removal"
trap 'rm -f $log' 0
echo "PACKAGE TEST OK: $product"
