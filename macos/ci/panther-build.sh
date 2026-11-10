#!/bin/sh
# On the Forgejo runner beside the Panther guest: panther-build.sh <variant> <source dir> <out dir>
#
# Puts the source on the 9p share (the runner's /share is the guest's /Volumes/share), has the guest build it
# over ssh (127.0.0.1:22, the pod's key from the ssh-agent), and takes the result back. The guest's login
# shell is tcsh, so the build is a script run with sh.
set -eu
variant=$1
src=$2
out=$3
version=${VERSION:-0.0.0}
job=build-$variant-$$-$(date +%s)
share=/share/$job
mkdir -p "$share"
chmod 777 "$share"
ssh_guest() {
	ssh -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o BatchMode=yes -o LogLevel=ERROR \
		-o ConnectTimeout=15 macos@127.0.0.1 "$@"
}
# What the guest wrote onto the share belongs to the guest's account, and the runner's rm cannot take it:
# the guest removes the directory (ssh_guest is defined above).
cleanup() {
	ssh_guest "rm -rf /Volumes/share/$job" 2>/dev/null || true
	rm -rf "$share" 2>/dev/null || true
}
trap cleanup EXIT
tar -C "$src" --exclude=.git --exclude=build -czf "$share/src.tgz" .
cp "$src/ci/side-toolkit.sh" "$src/ci/guest-build.sh" "$src/ci/component.sh" "$share/"
mkdir -p "$share/out"
chmod 777 "$share/out"

# The first connection after a pod restart is reset until Dropbear listens.
n=0
until ssh_guest true; do
	n=$((n + 1))
	[ "$n" -lt 40 ] || { echo "the guest did not answer ssh" >&2; exit 1; }
	sleep 5
done
ssh_guest "sh -c 'VERSION=$version sh /Volumes/share/$job/guest-build.sh $variant /Volumes/share/$job 2>&1'"
mkdir -p "$out/$variant"
tar -C "$out/$variant" -xzf "$share/out/$variant.tgz"
[ ! -f "$share/out/$variant-top.tgz" ] || tar -C "$out/$variant" -xzf "$share/out/$variant-top.tgz"
