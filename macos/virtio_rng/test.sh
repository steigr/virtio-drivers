#!/bin/sh
# Exercises VirtioRand from the guest. Run as root, with the kext loaded, in a guest whose QEMU has
#
#   -object rng-random,id=rng0,filename=/dev/urandom -device virtio-rng-pci,disable-modern=on,rng=rng0
#
#   sh test.sh
#
# Prints one line per check and "FAIL" for each that fails. Plain sh: 10.1's /bin/sh is zsh, 10.2's bash.

fails=0

check() {
    if eval "$2" > /tmp/vrcheck.out 2>&1; then
        echo "ok   $1"
    else
        echo "FAIL $1: `head -3 /tmp/vrcheck.out`"
        fails=`expr $fails + 1`
    fi
}

check "attached to the device"      'dmesg | grep "VirtioRand: I/O at"'
check "primed the generator"        'dmesg | grep "VirtioRand: primed the generator with"'
check "no refusal or stall logged"  '! dmesg | grep -E "VirtioRand: (the kernel|the device has not|could not)"'
check "/dev/random reads"           '[ "`dd if=/dev/random bs=64 count=4 2>/dev/null | wc -c | tr -d " "`" = 256 ]'
check "two reads differ"            'a=`dd if=/dev/random bs=32 count=1 2>/dev/null | cksum`; b=`dd if=/dev/random bs=32 count=1 2>/dev/null | cksum`; [ "$a" != "$b" ]'
check "no stall past an interval"   'sleep 12; ! dmesg | grep -E "VirtioRand: (the kernel|the device has not)"'

rm -f /tmp/vrcheck.out
[ $fails = 0 ] && echo "all passed"
exit $fails
