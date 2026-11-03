#!/bin/sh
# Install the package with inst in the IRIX guest, check what landed, and remove
# it again. Nothing is loaded: a build node should not run a driver under test,
# and may not have the device.
#
#   test-package.sh <tardist>
tardist=$1
log=/var/tmp/vioser-inst-$$.log
obj=/usr/lib/vioser/vioser.o
obj9p=/usr/lib/vioser/p9fs.o
fail() { echo "PACKAGE TEST FAILED: $*"; exit 1; }
remove() { /usr/sbin/inst -a -R vioser > $log 2>&1; }
trap 'cd /; remove; rm -f $log' 0
cd / || exit 1

/usr/sbin/inst -a -f $tardist -I vioser > $log 2>&1
tail -3 $log
grep 'Installations and removals were successful' $log > /dev/null || { cat $log; fail "inst"; }
/usr/sbin/versions -n vioser | cat

for f in $obj $obj9p /etc/init.d/vioser /etc/init.d/p9fs /usr/etc/mount_9p; do
	[ -f $f ] || fail "missing $f"
done
for f in /etc/rc2.d/S98vioser /etc/rc0.d/K02vioser /etc/rc2.d/S98p9fs /etc/rc0.d/K02p9fs; do
	[ -f $f ] || fail "$f does not lead to the init script"
done
# ml needs the symbol table and relocations, and this board's ABI.
/usr/bin/nm $obj 2>/dev/null | grep vioser_init > /dev/null || fail "$obj has no symbols"
/usr/bin/nm $obj9p 2>/dev/null | grep p9fs_vfsops > /dev/null || fail "$obj9p has no symbols"
case `uname -m` in
IP22) abi=N32 ;;
*)    abi=64-bit ;;
esac
/usr/bin/file $obj $obj9p | cat
/usr/bin/file $obj | grep "$abi" > /dev/null || fail "$obj is not $abi"
/usr/bin/file $obj9p | grep "$abi" > /dev/null || fail "$obj9p is not $abi"

remove
grep 'Installations and removals were successful' $log > /dev/null || { cat $log; fail "inst -R"; }
for f in $obj $obj9p /etc/init.d/vioser /etc/init.d/p9fs /usr/etc/mount_9p \
    /etc/rc2.d/S98vioser /etc/rc0.d/K02vioser /etc/rc2.d/S98p9fs /etc/rc0.d/K02p9fs; do
	[ ! -f $f ] || fail "$f left behind after removal"
done
[ ! -d /usr/lib/vioser ] || fail "/usr/lib/vioser left behind after removal"
trap 'rm -f $log' 0
echo "PACKAGE TEST OK"
