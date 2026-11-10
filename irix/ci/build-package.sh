#!/bin/sh
# Build one driver's inst package in the IRIX guest: gendist, the MIPSpro
# compiler and the kernel headers exist only there.
#
#   build-package.sh <virtio_serial|virtio_9p> <version> <out-dir>
#
# <version> is an integer, as inst needs. Writes <driver>-<version>.tardist to
# <out-dir>, with a module for each board; inst installs the one that matches the
# machine. Each package replaces its predecessor of the old name (vioser, p9fs).
drv=$1; ver=$2; out=$3
usage() { echo "usage: $0 <virtio_serial|virtio_9p> <integer version> <out-dir>"; exit 2; }
case "$drv" in virtio_serial|virtio_9p) ;; *) usage ;; esac
case "$ver" in ''|*[!0-9]*) usage ;; esac
[ -n "$out" ] || usage
[ -f /var/sysgen/Makefile.kernloadio ] || { echo "no kernel build environment in this system"; exit 1; }

irix=`cd \`dirname $0\`/.. && pwd`
boards="IP22 IP28 IP30 IP35"
tmp=/var/tmp/$drv-package-$$
trap 'cd /; rm -rf $tmp' 0
# Every board's file installs at one path. A kernel module must not be stripped.
mach="mach(CPUBOARD=IP22 CPUBOARD=IP28 CPUBOARD=IP30 CPUBOARD=IP35)"
# inst versions are integers; the old packages' are dates such as 2026110202.
any=2147483647
root=$tmp/root
mkdir -p $out $root/etc/init.d $root/etc/rc2.d $root/etc/rc0.d $root/usr/lib/$drv || exit 1
out=`cd $out && pwd`

# One compile per CPU (hinv's count): the Tezro has four, the Indigo2 one.
ncpu=`hinv -c processor 2> /dev/null | sed -n 's/^\([0-9][0-9]*\) .* Processors*$/\1/p'`
case "$ncpu" in ''|*[!0-9]*|0) ncpu=1 ;; esac
cd $irix/$drv || exit 1
for board in $boards; do
	smake CPUBOARD=$board clean || exit 1
	smake -J $ncpu CPUBOARD=$board || exit 1
	mkdir -p $root/usr/lib/$drv/$board || exit 1
	cp /tmp/$drv.$board/mod/$drv.o $root/usr/lib/$drv/$board/$drv.o || exit 1
	if [ $drv = virtio_9p ]; then
		# A user program, and all boards' copies are the same.
		mkdir -p $root/usr/etc || exit 1
		cp /tmp/$drv.$board/mount_9p $root/usr/etc/mount_9p || exit 1
	fi
	smake CPUBOARD=$board clean
done
cp $drv.init $root/etc/init.d/$drv || exit 1
ln -s ../init.d/$drv $root/etc/rc2.d/S98$drv || exit 1
ln -s ../init.d/$drv $root/etc/rc0.d/K02$drv || exit 1

{
	case $drv in
	virtio_serial)
		mkdir -p $root/usr/include/sys || exit 1
		cp virtio_serial.h $root/usr/include/sys/virtio_serial.h || exit 1
		echo "f 0755 root sys etc/init.d/$drv etc/init.d/$drv $drv.sw.driver $mach removeop(\"rm -f \$rbase/dev/virtio-ports\")"
		echo "f 0444 root sys usr/include/sys/virtio_serial.h usr/include/sys/virtio_serial.h $drv.dev.hdr"
		;;
	virtio_9p)
		echo "f 0755 root sys etc/init.d/$drv etc/init.d/$drv $drv.sw.driver $mach"
		echo "f 0755 root sys usr/etc/mount_9p usr/etc/mount_9p $drv.sw.driver $mach"
		;;
	esac
	echo "l 0755 root sys etc/rc2.d/S98$drv etc/rc2.d/S98$drv $drv.sw.driver $mach symval(../init.d/$drv)"
	echo "l 0755 root sys etc/rc0.d/K02$drv etc/rc0.d/K02$drv $drv.sw.driver $mach symval(../init.d/$drv)"
	echo "d 0755 root sys usr/lib/$drv usr/lib/$drv $drv.sw.driver $mach"
	for board in $boards; do
		echo "f 0644 root sys usr/lib/$drv/$drv.o usr/lib/$drv/$board/$drv.o $drv.sw.driver mach(CPUBOARD=$board) nostrip norqs"
	done
} > $tmp/$drv.idb.in || exit 1

case $drv in
virtio_serial)
	cat > $tmp/$drv.spec <<SPEC
product virtio_serial
    id "virtio_serial, a virtio-serial driver for IRIX 6.5 under IRIS"
    image sw
        id "virtio_serial kernel module"
        version $ver
        order 9999
        subsys driver default
            id "virtio_serial module, loaded at boot"
            replaces virtio_serial.sw.driver 0 $ver
            replaces vioser.sw.driver 0 $any
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
            replaces vioser.dev.hdr 0 $any
            exp virtio_serial.dev.hdr
        endsubsys
    endimage
endproduct
SPEC
	;;
virtio_9p)
	cat > $tmp/$drv.spec <<SPEC
product virtio_9p
    id "virtio_9p, a 9P filesystem over virtio-9p for IRIX 6.5 under IRIS"
    image sw
        id "virtio_9p kernel module and mount_9p"
        version $ver
        order 9999
        subsys driver default
            id "virtio_9p module, loaded at boot, and mount_9p"
            replaces virtio_9p.sw.driver 0 $ver
            replaces p9fs.sw.driver 0 $any
            exp virtio_9p.sw.driver
        endsubsys
    endimage
endproduct
SPEC
	;;
esac

sort -k 5,6 $tmp/$drv.idb.in > $tmp/$drv.idb || exit 1
mkdir -p $tmp/dist || exit 1
(cd $tmp &&
    /usr/sbin/gendist -sbase root -idb $drv.idb -spec $drv.spec -dist dist &&
    cd dist && tar cf ../$drv-$ver.tardist *) || exit 1
cp $tmp/$drv-$ver.tardist $out/ || exit 1
ls -l $out/$drv-$ver.tardist
cd /
