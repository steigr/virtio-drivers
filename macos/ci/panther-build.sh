#!/bin/sh
# On the Forgejo runner beside the Panther guest: panther-build.sh <variant> <driver dir> <out dir>
#
# <driver dir> is one of macos/<driver>, with its Makefile and component.sh; the guest-side scripts come
# from this directory.
#
# Puts the source on the share (SHARE_DIR on the runner, GUEST_SHARE in the guest), has the guest build it
# over ssh (GUEST_SSH, with the key from the runner's ssh-agent), and takes the result back. The guest's login
# shell is tcsh, so the build is a script run with sh.
set -eu
variant=$1
src=$2
out=$3
version=${VERSION:-0.0.0}
: "${GUEST_SSH:?ssh destination of the guest, user@host}"
: "${SHARE_DIR:?the share, as the runner sees it}"
: "${GUEST_SHARE:?the share, as the guest sees it}"
job=build-$variant-$$-$(date +%s)
share=$SHARE_DIR/$job
gshare=$GUEST_SHARE/$job
mkdir -p "$share"
chmod 777 "$share"
ssh_guest() {
	ssh -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o BatchMode=yes -o LogLevel=ERROR \
		-o ConnectTimeout=15 "$GUEST_SSH" "$@"
}
# What the guest wrote onto the share belongs to the guest's account, and the runner's rm cannot take it:
# the guest removes the directory (ssh_guest is defined above).
cleanup() {
	ssh_guest "rm -rf $gshare" 2>/dev/null || true
	rm -rf "$share" 2>/dev/null || true
}
trap cleanup EXIT
tar -C "$src" --exclude=.git --exclude=build -czf "$share/src.tgz" .
ci=$(cd "$(dirname "$0")" && pwd)
cp "$ci/side-toolkit.sh" "$ci/guest-build.sh" "$src/component.sh" "$share/"
mkdir -p "$share/out"
chmod 777 "$share/out"

# The first connection after a pod restart is reset until Dropbear listens.
n=0
until ssh_guest true; do
	n=$((n + 1))
	[ "$n" -lt 40 ] || { echo "the guest did not answer ssh" >&2; exit 1; }
	sleep 5
done
ssh_guest "sh -c 'VERSION=$version sh $gshare/guest-build.sh $variant $gshare 2>&1'"
mkdir -p "$out/$variant"
tar -C "$out/$variant" -xzf "$share/out/$variant.tgz"
[ ! -f "$share/out/$variant-top.tgz" ] || tar -C "$out/$variant" -xzf "$share/out/$variant-top.tgz"
