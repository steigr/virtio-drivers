#!/bin/sh
# Build the inst packages, one for each driver, in the IRIX guest: gendist, the
# MIPSpro compiler and the kernel headers exist only there.
#
#   build-package.sh <version> <out-dir>
#
# <version> is an integer, as inst needs. Writes vioser-<version>.tardist and
# p9fs-<version>.tardist to <out-dir>, with a module for each board; inst installs
# the one that matches the machine.
ver=$1; out=$2
case "$ver" in
''|*[!0-9]*) echo "usage: $0 <integer version> <out-dir>"; exit 2 ;;
esac
[ -n "$out" ] || { echo "usage: $0 <integer version> <out-dir>"; exit 2; }
[ -f /var/sysgen/Makefile.kernloadio ] || { echo "no kernel build environment in this system"; exit 1; }

repo=`cd \`dirname $0\`/.. && pwd`
boards="IP22 IP28 IP30 IP35"
tmp=/var/tmp/vioser-package-$$
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

vroot=$tmp/vioser/root
proot=$tmp/p9fs/root
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
	mkdir -p $vroot/usr/lib/vioser/$board $proot/usr/lib/p9fs/$board || exit 1
	cp /tmp/vioser.$board/mod/vioser.o $vroot/usr/lib/vioser/$board/vioser.o || exit 1
	cp /tmp/vioser.$board/mod/p9fs.o $proot/usr/lib/p9fs/$board/p9fs.o || exit 1
	# A user program, and all boards' copies are the same.
	cp /tmp/vioser.$board/mount_9p $proot/usr/etc/mount_9p || exit 1
	smake CPUBOARD=$board clean
done
cp vioser.init $vroot/etc/init.d/vioser || exit 1
cp vioser.h $vroot/usr/include/sys/vioser.h || exit 1
ln -s ../init.d/vioser $vroot/etc/rc2.d/S98vioser || exit 1
ln -s ../init.d/vioser $vroot/etc/rc0.d/K02vioser || exit 1
cp p9fs.init $proot/etc/init.d/p9fs || exit 1
ln -s ../init.d/p9fs $proot/etc/rc2.d/S98p9fs || exit 1
ln -s ../init.d/p9fs $proot/etc/rc0.d/K02p9fs || exit 1

{
	echo "f 0755 root sys etc/init.d/vioser etc/init.d/vioser vioser.sw.driver $mach removeop(\"rm -f \$rbase/dev/virtio-ports\")"
	echo "l 0755 root sys etc/rc2.d/S98vioser etc/rc2.d/S98vioser vioser.sw.driver $mach symval(../init.d/vioser)"
	echo "l 0755 root sys etc/rc0.d/K02vioser etc/rc0.d/K02vioser vioser.sw.driver $mach symval(../init.d/vioser)"
	echo "f 0444 root sys usr/include/sys/vioser.h usr/include/sys/vioser.h vioser.dev.hdr"
	echo "d 0755 root sys usr/lib/vioser usr/lib/vioser vioser.sw.driver $mach"
	for board in $boards; do
		echo "f 0644 root sys usr/lib/vioser/vioser.o usr/lib/vioser/$board/vioser.o vioser.sw.driver mach(CPUBOARD=$board) nostrip norqs"
	done
} > $tmp/vioser/vioser.idb.in

cat > $tmp/vioser/vioser.spec <<SPEC
product vioser
    id "vioser, a virtio-serial driver for IRIX 6.5 under IRIS"
    image sw
        id "vioser kernel module"
        version $ver
        order 9999
        subsys driver default
            id "vioser module, loaded at boot"
            replaces vioser.sw.driver 0 $ver
            exp vioser.sw.driver
        endsubsys
    endimage
    image dev
        id "vioser header"
        version $ver
        order 9999
        subsys hdr
            id "VIOSER_GETINFO, in sys/vioser.h"
            replaces vioser.dev.hdr 0 $ver
            exp vioser.dev.hdr
        endsubsys
    endimage
endproduct
SPEC

{
	echo "f 0755 root sys etc/init.d/p9fs etc/init.d/p9fs p9fs.sw.driver $mach"
	echo "l 0755 root sys etc/rc2.d/S98p9fs etc/rc2.d/S98p9fs p9fs.sw.driver $mach symval(../init.d/p9fs)"
	echo "l 0755 root sys etc/rc0.d/K02p9fs etc/rc0.d/K02p9fs p9fs.sw.driver $mach symval(../init.d/p9fs)"
	echo "f 0755 root sys usr/etc/mount_9p usr/etc/mount_9p p9fs.sw.driver $mach"
	echo "d 0755 root sys usr/lib/p9fs usr/lib/p9fs p9fs.sw.driver $mach"
	for board in $boards; do
		echo "f 0644 root sys usr/lib/p9fs/p9fs.o usr/lib/p9fs/$board/p9fs.o p9fs.sw.driver mach(CPUBOARD=$board) nostrip norqs"
	done
} > $tmp/p9fs/p9fs.idb.in

cat > $tmp/p9fs/p9fs.spec <<SPEC
product p9fs
    id "p9fs, a 9P filesystem over virtio-9p for IRIX 6.5 under IRIS"
    image sw
        id "p9fs kernel module and mount_9p"
        version $ver
        order 9999
        subsys driver default
            id "p9fs module, loaded at boot, and mount_9p"
            replaces p9fs.sw.driver 0 $ver
            exp p9fs.sw.driver
        endsubsys
    endimage
endproduct
SPEC

gen_tardist vioser
gen_tardist p9fs
cd /
