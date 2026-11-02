#!/bin/sh
# Build the vioser inst package in the IRIX guest: gendist, the MIPSpro compiler
# and the kernel headers exist only there.
#
#   build-package.sh <version> <out-dir>
#
# <version> is an integer, as inst needs. Writes <out-dir>/vioser-<version>.tardist
# with a module for each board; inst installs the one that matches the machine.
ver=$1; out=$2
case "$ver" in
''|*[!0-9]*) echo "usage: $0 <integer version> <out-dir>"; exit 2 ;;
esac
[ -n "$out" ] || { echo "usage: $0 <integer version> <out-dir>"; exit 2; }
[ -f /var/sysgen/Makefile.kernloadio ] || { echo "no kernel build environment in this system"; exit 1; }

repo=`cd \`dirname $0\`/.. && pwd`
boards="IP22 IP28 IP30"
tmp=/var/tmp/vioser-package-$$
trap 'cd /; rm -rf $tmp' 0
root=$tmp/root
mkdir -p $root/etc/init.d $root/etc/rc2.d $root/etc/rc0.d $root/usr/include/sys $tmp/dist || exit 1

cd $repo/driver || exit 1
for board in $boards; do
	smake CPUBOARD=$board clean || exit 1
	smake CPUBOARD=$board || exit 1
	mkdir -p $root/usr/lib/vioser/$board || exit 1
	cp /tmp/vioser.$board/mod/vioser.o $root/usr/lib/vioser/$board/vioser.o || exit 1
	smake CPUBOARD=$board clean
done
cp vioser.init $root/etc/init.d/vioser || exit 1
cp vioser.h $root/usr/include/sys/vioser.h || exit 1
ln -s ../init.d/vioser $root/etc/rc2.d/S98vioser || exit 1
ln -s ../init.d/vioser $root/etc/rc0.d/K02vioser || exit 1

# Every board's file installs at one path. A kernel module must not be stripped.
mach="mach(CPUBOARD=IP22 CPUBOARD=IP28 CPUBOARD=IP30)"
{
	echo "f 0755 root sys etc/init.d/vioser etc/init.d/vioser vioser.sw.driver $mach removeop(\"rm -f \$rbase/dev/virtio-ports\")"
	echo "l 0755 root sys etc/rc2.d/S98vioser etc/rc2.d/S98vioser vioser.sw.driver $mach symval(../init.d/vioser)"
	echo "l 0755 root sys etc/rc0.d/K02vioser etc/rc0.d/K02vioser vioser.sw.driver $mach symval(../init.d/vioser)"
	echo "f 0444 root sys usr/include/sys/vioser.h usr/include/sys/vioser.h vioser.dev.hdr"
	echo "d 0755 root sys usr/lib/vioser usr/lib/vioser vioser.sw.driver $mach"
	for board in $boards; do
		echo "f 0644 root sys usr/lib/vioser/vioser.o usr/lib/vioser/$board/vioser.o vioser.sw.driver mach(CPUBOARD=$board) nostrip norqs"
	done
} | sort -k 5,6 > $tmp/vioser.idb

cat > $tmp/vioser.spec <<SPEC
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

cd $tmp || exit 1
/usr/sbin/gendist -sbase root -idb vioser.idb -spec vioser.spec -dist dist || exit 1
(cd dist && tar cf ../vioser-$ver.tardist *) || exit 1
cd / || exit 1
mkdir -p $out || exit 1
cp $tmp/vioser-$ver.tardist $out/ || exit 1
ls -l $out/vioser-$ver.tardist
