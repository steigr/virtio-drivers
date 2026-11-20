#!/bin/sh
# Exercises a mounted 9p share from the guest. Run as root, in a guest whose QEMU has
# -device virtio-9p-pci,mount_tag=<tag>, with the kext loaded:
#
#   sh test.sh <mount_9p> <tag> <mountpoint>
#
# It mounts, works in <mountpoint>/p9test (made afresh), prints one line per check and "FAIL"
# for each that fails, and unmounts. Plain sh: 10.1's /bin/sh is zsh, 10.2's bash.

mount9p=${1:?usage: test.sh <mount_9p> <tag> <mountpoint>}
tag=${2:?}
mnt=${3:?}
mkdir -p "$mnt"
mnt=`cd "$mnt" && /bin/pwd`     # as the kernel has it: /tmp is /private/tmp
t="$mnt/p9test"
fails=0

# check <name> <shell code>: runs the code here, reports
check() {
    if eval "$2" > /tmp/p9check.out 2>&1; then
        echo "ok   $1"
    else
        echo "FAIL $1: `head -3 /tmp/p9check.out`"
        fails=`expr $fails + 1`
    fi
}

# absent <path>: there is nothing at path (not "! ls": 10.1's zsh-as-sh ignores ! under eval)
absent() {
    if [ -f "$1" -o -d "$1" -o -h "$1" ]; then echo "$1 is there"; return 1; fi
}

# same <a> <b>: the two strings are equal
same() {
    [ "$1" = "$2" ] || { echo "\"$1\" != \"$2\""; return 1; }
}

size() {
    ls -l "$1" | awk '{print $5}'
}

dd if=/dev/random of=/tmp/p9big bs=65536 count=40 2>/dev/null

check "mount"                   '$mount9p $tag $mnt'
check "mount listed"            'mount | grep " on $mnt\$\| on $mnt "'
check "statfs"                  'df $mnt'
check "mkdir"                   'rm -rf $t; mkdir $t'
check "pwd inside"              'same "`cd $t && /bin/pwd`" "$t"'
check "write"                   'echo hello 9p > $t/a.txt'
check "read back"               'same "`cat $t/a.txt`" "hello 9p"'
check "append"                  'echo more >> $t/a.txt && same "`wc -l < $t/a.txt | tr -d \" \"`" 2'
check "size"                    'same "`size $t/a.txt`" 14'
check "copy 2.5 MB in"          'cp /tmp/p9big $t/big'
check "same data"               'cmp /tmp/p9big $t/big'
check "read through a pipe"     'cat $t/big | cmp - /tmp/p9big'
check "truncate"                'cp /dev/null $t/a.txt && same "`size $t/a.txt`" 0'
check "rename"                  'mv $t/big $t/big2'
check "old name gone"           'absent $t/big'
check "new name there"          'cmp /tmp/p9big $t/big2'
check "symlink"                 'ln -s big2 $t/link'
check "readlink"                'same "`ls -l $t/link | sed \"s/.*-> //\"`" big2'
check "through the link"        'cmp /tmp/p9big $t/link'
check "hard link"               'ln $t/big2 $t/hard'
check "link count"              'same "`ls -l $t/hard | awk \"{print \\$2}\"`" 2'
check "chmod"                   'chmod 600 $t/hard && ls -l $t/hard | grep "^-rw-------"'
check "touch -t"                'touch -t 200101020304 $t/hard && ls -l $t/hard | grep "Jan  2  2001"'
check "300 files"               'mkdir $t/many && i=0 && while [ $i -lt 300 ]; do echo $i > $t/many/f$i || break; i=`expr $i + 1`; done; [ -f $t/many/f299 ]'
check "ls counts them"          'same "`ls $t/many | wc -l | tr -d \" \"`" 300'
check "find counts them"        'same "`find $t/many -type f | wc -l | tr -d \" \"`" 300'
check "exec from the share"     'cp /bin/echo $t/echo && same "`$t/echo run from 9p`" "run from 9p"'
check "exec a bigger one"       'cp /bin/ls $t/ls && $t/ls $t/many | grep f299'
check "remove a file"           'rm $t/hard'
check "remove a tree"           'rm -rf $t/many && absent $t/many'
check "rmdir"                   'rm -rf $t && absent $t'
check "unmount"                 'umount $mnt'

rm -f /tmp/p9big /tmp/p9check.out
echo "$fails failed"
[ $fails = 0 ]
