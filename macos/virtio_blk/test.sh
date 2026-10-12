#!/bin/sh
# Exercises VirtioBlock from the guest. Run as root, with the kext loaded, in a guest whose QEMU has
#
#   -drive file=vdisk.img,if=none,id=vd0,format=raw -device virtio-blk-pci,disable-modern=on,drive=vd0
#
# and vdisk.img a scratch image of a few MB or more: THE TEST OVERWRITES THE FIRST MEGABYTES OF THE
# DISK, so name it:
#
#   sh test.sh disk1 [megabytes]    # the device node without /dev/, default 4 MB
#
# `diskutil list` and the "VirtioBlock: N sectors" line of dmesg say which disk it is. Prints one
# line per check and "FAIL" for each that fails. Plain sh: 10.1's /bin/sh is zsh, 10.2's bash.

disk=${1:?usage: sh test.sh disk<n> [megabytes]}
mb=${2:-4}
fails=0
tmp=/tmp/vbtest.$$

check() {
    if eval "$2" > $tmp.out 2>&1; then
        echo "ok   $1"
    else
        echo "FAIL $1: `head -3 $tmp.out`"
        fails=`expr $fails + 1`
    fi
}

raw=/dev/r$disk
dmesg | grep "VirtioBlock: " | tail -1

check "block node present"       '[ -b /dev/$disk ]'
check "raw node present"         '[ -c $raw ]'
check "driver sized the disk"    'dmesg | grep "VirtioBlock: [0-9]* sectors" | tail -1'

# A pseudo-random file with a known checksum, from perl (10.0 has 5.6) so that the data is not
# a pattern the transport could get right by accident.
perl -e 'srand(42); $n = $ARGV[0] * 1048576; while ($n > 0) { $c = $n < 65536 ? $n : 65536; print pack("C*", map { int(rand(256)) } 1..$c); $n -= $c }' $mb > $tmp.src
want=`cksum < $tmp.src`

check "write $mb MB (raw, 64 KB)" 'dd if=$tmp.src of=$raw bs=64k conv=sync'
check "read it back (raw, 64 KB)" 'dd if=$raw of=$tmp.dst bs=64k count=`expr $mb \* 16` && [ "`cksum < $tmp.dst`" = "$want" ]'
check "read it back (raw, 512 B)" 'dd if=$raw of=$tmp.dst bs=512 count=`expr $mb \* 2048` && [ "`cksum < $tmp.dst`" = "$want" ]'
check "read it back (raw, 120 KB)" 'dd if=$raw of=$tmp.dst bs=120k count=`expr $mb \* 1024 / 120` && dd if=$tmp.src of=$tmp.exp bs=120k count=`expr $mb \* 1024 / 120` && [ "`cksum < $tmp.dst`" = "`cksum < $tmp.exp`" ]'
check "read it back (block)"      'dd if=/dev/$disk of=$tmp.dst bs=64k count=`expr $mb \* 16` && [ "`cksum < $tmp.dst`" = "$want" ]'
check "unaligned read (raw)"      'dd if=$raw of=$tmp.dst bs=512 skip=3 count=7 && dd if=$tmp.src of=$tmp.exp bs=512 skip=3 count=7 && cmp $tmp.exp $tmp.dst'
check "overwrite and read (raw)"  'dd if=/dev/zero of=$raw bs=4k count=2 conv=sync && dd if=$raw of=$tmp.dst bs=4k count=2 && [ "`cksum < $tmp.dst`" = "`dd if=/dev/zero bs=4k count=2 2>/dev/null | cksum`" ]'
check "read past the end is EOF"  'dd if=$raw of=$tmp.dst bs=512 skip=2000000000 count=1 && [ ! -s $tmp.dst ]'

# -w (an HFS wrapper around the HFS+ volume) is what 10.0 to 10.2 mount at boot; 10.6 dropped the
# option, and plain HFS+ is what everything from 10.3 on mounts.
check "newfs_hfs"                 'newfs_hfs -w -v VBTEST /dev/$disk 2>/dev/null || newfs_hfs -v VBTEST /dev/$disk'
check "mount"                     'mkdir -p $tmp.mnt && mount -t hfs /dev/$disk $tmp.mnt && mount | grep $tmp.mnt'
check "copy 3 MB in, compare"     'cp $tmp.src $tmp.mnt/f && sync && cmp $tmp.src $tmp.mnt/f'
# Forced when a plain unmount is refused: from 10.15 Spotlight takes a volume as it is mounted, and
# umount says "Resource busy" for a while.
check "unmount, remount, compare" '(umount $tmp.mnt || umount -f $tmp.mnt) && mount -t hfs /dev/$disk $tmp.mnt && cmp $tmp.src $tmp.mnt/f'
umount -f $tmp.mnt >/dev/null 2>&1; rmdir $tmp.mnt 2>/dev/null

rm -f $tmp.src $tmp.dst $tmp.exp $tmp.out
echo "$fails failed"
[ $fails = 0 ]
