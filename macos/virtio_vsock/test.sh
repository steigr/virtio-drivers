#!/bin/sh
# Exercises VirtioVsock from the host, through the guest's SSH, against a guest whose QEMU has
#
#   -device vhost-vsock-pci,guest-cid=<CID>
#
# with the kext loaded and `make tools` built in /tmp/virtiovsock in the guest (see README.md):
#
#   sh test.sh POD SSH_PORT CID [megabytes]     # e.g. sh test.sh jaguar-guest 2202 302 4
#
# The host's end (host-peer.py) runs in the pod, where /dev/vhost-vsock is; the guest's, vsockcat,
# over `ssh -p SSH_PORT root@127.0.0.1` (a `kubectl port-forward pod/POD SSH_PORT:22` the caller
# has running). Prints one line per check and "FAIL" for each that fails.

pod=${1:?pod}
port=${2:?ssh port}
cid=${3:?guest cid}
mb=${4:-4}
here=`dirname $0`
fails=0
vc=/tmp/virtiovsock/build/vsockcat

guest() {
    echo "$1" | ssh -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -p $port root@127.0.0.1 /bin/sh 2>&1 | grep -v '^Warning: Permanently'
}
peer() {
    kubectl -n macos exec -i $pod -- python3 - "$@" < $here/host-peer.py
}
check() {
    if [ "$2" = 0 ]; then
        echo "ok   $1"
    else
        echo "FAIL $1"
        fails=`expr $fails + 1`
    fi
}

got=`guest "$vc -i"`
check "the driver reports cid $cid (got $got)" `[ "$got" = "$cid" ]; echo $?`

# host -> guest: the guest listens and echoes, the host sends random bytes and compares
guest "rm -f /tmp/vc.log; ($vc -l 5000 -n 1 > /tmp/vc.log 2>&1 &); sleep 2" > /dev/null
out=`peer echo-client $cid 5000 $mb`; echo "     $out"
check "host connects to the guest, $mb MB echoed" `echo "$out" | grep -q '^ok'; echo $?`

# guest -> host: the guest sends a random file and reads back what the host made of it
(peer serve 5001 > /tmp/vsock-serve.out 2>&1 &)
sleep 5
want=`guest "dd if=/dev/urandom of=/tmp/rnd bs=1024 count=\`expr $mb \* 1024\` 2>/dev/null; openssl sha1 /tmp/rnd | sed 's/.*= //'"`
back=`guest "$vc -c 2 5001 < /tmp/rnd"`
echo "     guest sent $want, host says $back"
exp="`expr $mb \* 1048576` $want"
check "guest connects to the host, $mb MB, sha1 agrees" `[ "$back" = "$exp" ]; echo $?`

# a port nobody listens on
out=`guest "$vc -c 2 5999 < /dev/null; echo status \\$?"`
check "guest connect to a closed port is refused" `echo "$out" | grep -q 'refused\|Connection refused'; echo $?`
out=`peer refused $cid 5998`; echo "     $out"
check "host connect to a closed guest port fails" `echo "$out" | grep -q '^ok'; echo $?`

# the listener goes away cleanly: nothing left open
sleep 1
out=`guest "$vc -i; ps -ax | grep '[v]sockcat'"`
check "no vsockcat left running" `echo "$out" | grep -q vsockcat; [ $? = 1 ]; echo $?`

echo "$fails failed"
[ $fails = 0 ]
