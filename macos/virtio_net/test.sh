#!/bin/sh
# Exercises VirtioNet from the guest. Run as root, with the kext loaded, in a guest whose QEMU has
#
#   -netdev user,id=vn0 -device virtio-net-pci,disable-modern=on,netdev=vn0
#
# and echo-server.py running beside QEMU (slirp makes its 127.0.0.1:9000 the guest's 10.0.2.2:9000):
#
#   sh test.sh [address] [peer] [port] [megabytes]    # defaults 10.0.2.15 10.0.2.2 9000 4
#
# It finds the driver's interface by the address the driver logged, configures it, prints one line
# per check and "FAIL" for each that fails. Plain sh: 10.1's /bin/sh is zsh, 10.2's bash.

addr=${1:-10.0.2.15}
peer=${2:-10.0.2.2}
port=${3:-9000}
mb=${4:-4}
fails=0
here=`dirname $0`

check() {
    if eval "$2" > /tmp/vncheck.out 2>&1; then
        echo "ok   $1"
    else
        echo "FAIL $1: `head -3 /tmp/vncheck.out`"
        fails=`expr $fails + 1`
    fi
}

# An address as ifconfig prints it: without leading zeros (10.0's and 10.1's do, later ones don't).
norm() {
    echo "$1" | tr 'A-F' 'a-f' | sed 's/^0\([0-9a-f]\)/\1/; s/:0\([0-9a-f]\)/:\1/g'
}

# From dmesg, or from the system log where the kernel's message buffer no longer has it (10.4).
mac=`(dmesg; cat /var/log/system.log) 2>/dev/null | grep 'VirtioNet: [0-9a-f][0-9a-f]:' | tail -1 | sed 's/.*VirtioNet: \([0-9a-f:]*\),.*/\1/'`
if [ -z "$mac" ]; then
    echo "FAIL the driver logged no address: is the kext loaded and the device there?"
    exit 1
fi
want=`norm $mac`
if=
for i in `ifconfig -l`; do
    e=`ifconfig $i | awk '/ether/ {print $2}'`
    [ -n "$e" ] && [ "`norm $e`" = "$want" ] && if=$i
done
echo "     device $mac is ${if:-nowhere}"

check "interface attached"      '[ -n "$if" ]'
check "configure"               'ifconfig $if inet $addr netmask 255.255.255.0 up'
check "link active"             'ifconfig $if | grep "status: active"'
check "ping the peer"           'ping -c 5 $peer | grep " 0% packet loss"'
check "peer in the ARP table"   'arp -n $peer | grep -v incomplete | grep ":"'
check "large pings"             'ping -c 3 -s 1472 $peer | grep " 0% packet loss"'
check "TCP echo, $mb MB"        'perl $here/vnecho.pl $peer $port $mb'
check "down"                    'ifconfig $if down'
check "up again"                'ifconfig $if up && sleep 2 && ping -c 3 $peer | grep " 0% packet loss"'
check "TCP echo after up"       'perl $here/vnecho.pl $peer $port 1'
check "counters"                'netstat -in | grep "^$if " | head -1 | awk "{ if (\$5 > 0 && \$7 > 0) exit 0; exit 1 }"'

netstat -in | grep "^$if "
rm -f /tmp/vncheck.out
echo "$fails failed"
[ $fails = 0 ]
