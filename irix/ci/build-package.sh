#!/bin/sh
# Build the inst packages, one for each driver, in the IRIX guest: gendist, the
# MIPSpro compiler and the kernel headers exist only there.
#
#   build-package.sh <version> <out-dir>
#
# <version> is an integer, as inst needs. Writes virtio_serial-<version>.tardist and
# virtio_9p-<version>.tardist to <out-dir>, with a module for each board; inst installs
# the one that matches the machine.
ver=$1; out=$2
case "$ver" in
''|*[!0-9]*) echo "usage: $0 <integer version> <out-dir>"; exit 2 ;;
esac
[ -n "$out" ] || { echo "usage: $0 <integer version> <out-dir>"; exit 2; }
[ -f /var/sysgen/Makefile.kernloadio ] || { echo "no kernel build environment in this system"; exit 1; }

repo=`cd \`dirname $0\`/.. && pwd`
boards="IP22 IP28 IP30 IP35"
tmp=/var/tmp/virtio_serial-package-$$
trap 'cd /; rm -rf $tmp' 0
# Every board's file installs at one path. A kernel module must not be stripped.
mach="mach(CPUBOARD=IP22 CPUBOARD=IP28 CPUBOARD=IP30 CPUBOARD=IP35)"

# The tree under $tmp/<product>/root, with the <product>.idb.in and <product>.spec
# written beside it, becomes a tardist in <out-dir>.
gen_tardist() {
	product=$1
	sort -k 5,6 $tmp/$product/$product.idb.in > $tmp/$product/$product.idb || exit 1
	mkdir -p $tmp/$product/dist || exit 1
	(cd $tmp/$product &&
	    /usr/sbin/gendist -sbase root -idb $product.idb -spec $product.spec -dist dist &&
	    cd dist && tar cf ../$product-$ver.tardist *) || exit 1
	cp $tmp/$product/$product-$ver.tardist $out/ || exit 1
	ls -l $out/$product-$ver.tardist
}

vroot=$tmp/virtio_serial/root
proot=$tmp/virtio_9p/root
mkdir -p $out || exit 1
out=`cd $out && pwd`
mkdir -p $vroot/etc/init.d $vroot/etc/rc2.d $vroot/etc/rc0.d $vroot/usr/include/sys \
    $proot/etc/init.d $proot/etc/rc2.d $proot/etc/rc0.d $proot/usr/etc || exit 1

# One compile per CPU (hinv's count): the Tezro has four, the Indigo2 one.
ncpu=`hinv -c processor 2> /dev/null | sed -n 's/^\([0-9][0-9]*\) .* Processors*$/\1/p'`
case "$ncpu" in ''|*[!0-9]*|0) ncpu=1 ;; esac
cd $repo/driver || exit 1
for board in $boards; do
	smake CPUBOARD=$board clean || exit 1
	smake -J $ncpu CPUBOARD=$board || exit 1
	mkdir -p $vroot/usr/lib/virtio_serial/$board $proot/usr/lib/virtio_9p/$board || exit 1
	cp /tmp/virtio_serial.$board/mod/virtio_serial.o $vroot/usr/lib/virtio_serial/$board/virtio_serial.o || exit 1
	cp /tmp/virtio_serial.$board/mod/virtio_9p.o $proot/usr/lib/virtio_9p/$board/virtio_9p.o || exit 1
	# A user program, and all boards' copies are the same.
	cp /tmp/virtio_serial.$board/mount_9p $proot/usr/etc/mount_9p || exit 1
	smake CPUBOARD=$board clean
done
cp virtio_serial.init $vroot/etc/init.d/virtio_serial || exit 1
cp virtio_serial.h $vroot/usr/include/sys/virtio_serial.h || exit 1
ln -s ../init.d/virtio_serial $vroot/etc/rc2.d/S98virtio_serial || exit 1
ln -s ../init.d/virtio_serial $vroot/etc/rc0.d/K02virtio_serial || exit 1
cp virtio_9p.init $proot/etc/init.d/virtio_9p || exit 1
ln -s ../init.d/virtio_9p $proot/etc/rc2.d/S98virtio_9p || exit 1
ln -s ../init.d/virtio_9p $proot/etc/rc0.d/K02virtio_9p || exit 1

{
	echo "f 0755 root sys etc/init.d/virtio_serial etc/init.d/virtio_serial virtio_serial.sw.driver $mach removeop(\"rm -f \$rbase/dev/virtio-ports\")"
	echo "l 0755 root sys etc/rc2.d/S98virtio_serial etc/rc2.d/S98virtio_serial virtio_serial.sw.driver $mach symval(../init.d/virtio_serial)"
	echo "l 0755 root sys etc/rc0.d/K02virtio_serial etc/rc0.d/K02virtio_serial virtio_serial.sw.driver $mach symval(../init.d/virtio_serial)"
	echo "f 0444 root sys usr/include/sys/virtio_serial.h usr/include/sys/virtio_serial.h virtio_serial.dev.hdr"
	echo "d 0755 root sys usr/lib/virtio_serial usr/lib/virtio_serial virtio_serial.sw.driver $mach"
	for board in $boards; do
		echo "f 0644 root sys usr/lib/virtio_serial/virtio_serial.o usr/lib/virtio_serial/$board/virtio_serial.o virtio_serial.sw.driver mach(CPUBOARD=$board) nostrip norqs"
	done
} > $tmp/virtio_serial/virtio_serial.idb.in

cat > $tmp/virtio_serial/virtio_serial.spec <<SPEC
product virtio_serial
    id "virtio_serial, a virtio-serial driver for IRIX 6.5 under IRIS"
    image sw
        id "virtio_serial kernel module"
        version $ver
        order 9999
        subsys driver default
            id "virtio_serial module, loaded at boot"
            replaces virtio_serial.sw.driver 0 $ver
            exp virtio_serial.sw.driver
        endsubsys
    endimage
    image dev
        id "virtio_serial header"
        version $ver
        order 9999
        subsys hdr
            id "VIRTIO_SERIAL_GETINFO, in sys/virtio_serial.h"
            replaces virtio_serial.dev.hdr 0 $ver
            exp virtio_serial.dev.hdr
        endsubsys
    endimage
endproduct
SPEC

{
	echo "f 0755 root sys etc/init.d/virtio_9p etc/init.d/virtio_9p virtio_9p.sw.driver $mach"
	echo "l 0755 root sys etc/rc2.d/S98virtio_9p etc/rc2.d/S98virtio_9p virtio_9p.sw.driver $mach symval(../init.d/virtio_9p)"
	echo "l 0755 root sys etc/rc0.d/K02virtio_9p etc/rc0.d/K02virtio_9p virtio_9p.sw.driver $mach symval(../init.d/virtio_9p)"
	echo "f 0755 root sys usr/etc/mount_9p usr/etc/mount_9p virtio_9p.sw.driver $mach"
	echo "d 0755 root sys usr/lib/virtio_9p usr/lib/virtio_9p virtio_9p.sw.driver $mach"
	for board in $boards; do
		echo "f 0644 root sys usr/lib/virtio_9p/virtio_9p.o usr/lib/virtio_9p/$board/virtio_9p.o virtio_9p.sw.driver mach(CPUBOARD=$board) nostrip norqs"
	done
} > $tmp/virtio_9p/virtio_9p.idb.in

cat > $tmp/virtio_9p/virtio_9p.spec <<SPEC
product virtio_9p
    id "virtio_9p, a 9P filesystem over virtio-9p for IRIX 6.5 under IRIS"
    image sw
        id "virtio_9p kernel module and mount_9p"
        version $ver
        order 9999
        subsys driver default
            id "virtio_9p module, loaded at boot, and mount_9p"
            replaces virtio_9p.sw.driver 0 $ver
            exp virtio_9p.sw.driver
        endsubsys
    endimage
endproduct
SPEC

gen_tardist virtio_serial
gen_tardist virtio_9p
cd /
