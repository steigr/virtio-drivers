#!/bin/bash
# build-intel.sh: build VirtioBlock.kext for the Intel releases (`make ABI=intel`) inside a Snow
# Leopard guest in the cluster, and leave guest/dist/virtioblock-intel.tar.gz -- the kext and a
# BUILD-INFO -- for `make guest-addons-embed`; the legacyintel installer puts it on every installed
# system (internal/installer/releases/legacyintel, addonKexts). guest/drivers/virtio9p's script of
# the same name, with this driver's names.
#
# usage: build-intel.sh [ISO]
#
# ISO is the Snow Leopard retail DVD (default 10.6.8, 10K549, under a local directory),
# whose "Optional Installs" carries Xcode 3.2.6: its packages -- the tools, GCC 4.0 and 4.2, the
# 10.4u, 10.5 and 10.6 SDKs -- are installed in the guest one by one unless it has them. Attached
# read-only with hdiutil, so this runs on a Mac.
#
# The guest is RELEASE's pod (default snowleopard-guest) in NAMESPACE, running with its agent connected,
# and claimed for this (kubectl annotate sts/<release> claimed-by=<you>): other sessions
# build and test in the same releases. Files go in over HTTP, which the pod serves on its loopback
# and passt gives the guest at its gateway; the result comes back as base64 through the agent and
# is checked against the guest's own SHA-256 of it.
set -euo pipefail

ISO=${1:-}
RELEASE=${RELEASE:-snowleopard-guest}
KUBE_CONTEXT=${KUBE_CONTEXT:-$KUBE_CONTEXT}
NAMESPACE=${NAMESPACE:-macos}
PORT=${PORT:-8093}
PACKAGES="DeveloperToolsSystemSupport VersionedDeveloperToolsSystemSupport DeveloperToolsCLI DevSDK gcc4.0 gcc4.2 MacOSX10.4.Universal MacOSX10.5 MacOSX10.6"

here=$(cd "$(dirname "$0")" && pwd)
out=$here/../../dist
pod() { kubectl --context "$KUBE_CONTEXT" -n "$NAMESPACE" exec -i "$RELEASE-0" -c macos -- "$@"; }
guest() { pod macos exec "$@"; }

work=$(mktemp -d)
mnt=""
cleanup() {
	[ -n "$mnt" ] && hdiutil detach -quiet "$mnt" || true
	rm -rf "$work"
}
trap cleanup EXIT

pod mkdir -p /share/virtioblock-build
# Asked until there is one: a guest just past its login window has no route yet.
for _ in $(seq 60); do
	gateway=$(guest /usr/sbin/netstat -rn -f inet 2>/dev/null | awk '$1 == "default" { print $2; exit }')
	[ -n "$gateway" ] && break
	sleep 5
done
[ -n "$gateway" ] || { echo "the guest has no default route after 5 minutes" >&2; exit 1; }

if ! guest /bin/sh -c 'test -x /usr/bin/gcc-4.0 -a -x /usr/bin/gcc-4.2 -a -x /usr/bin/lipo && ls -d /Developer/SDKs/MacOSX10.4u.sdk /SDKs/MacOSX10.4u.sdk 2>/dev/null | grep -q . && ls -d /Developer/SDKs/MacOSX10.6.sdk /SDKs/MacOSX10.6.sdk 2>/dev/null | grep -q .'; then
	mnt=$(hdiutil attach -readonly -nobrowse -mountrandom "$work" "$ISO" | awk -F'\t' '/Apple_HFS/ { print $NF }')
	for p in $PACKAGES; do cp "$mnt/Optional Installs.localized/Packages/$p.pkg" "$work/"; done
	(cd "$work" && shasum -a 256 ./*.pkg | sed 's| \./| |' > SHA256SUMS)
	for f in "$work"/*.pkg "$work/SHA256SUMS"; do pod sh -c "cat > /share/virtioblock-build/$(basename "$f")" < "$f"; done
	toolchain="Xcode 3.2.6 packages from $(basename "$ISO")"
else
	toolchain="already installed in the guest"
fi

COPYFILE_DISABLE=1 tar -C "$here/.." --format ustar --no-mac-metadata --no-xattrs --exclude build \
	-czf "$work/src.tgz" virtioblock
pod sh -c 'cat > /share/virtioblock-build/src.tgz' < "$work/src.tgz"
# Served from /share/virtioblock-build by a server this script started, which a nonce proves.
nonce=$(uuidgen)
pod sh -c "echo $nonce > /share/virtioblock-build/nonce"
pod sh -c "curl -s -o /dev/null http://127.0.0.1:$PORT/nonce || { nohup python3 -m http.server $PORT --bind 127.0.0.1 -d /share/virtioblock-build >/tmp/virtioblock-build-http.log 2>&1 </dev/null & sleep 1; }"
[ "$(guest /usr/bin/curl -sfS "http://$gateway:$PORT/nonce")" = "$nonce" ] ||
	{ echo "port $PORT in $RELEASE-0 is served by something else than /share/virtioblock-build" >&2; exit 1; }

guest -i /bin/sh <<EOF
set -e
url=http://$gateway:$PORT
mkdir -p /tmp/virtioblock-build && cd /tmp/virtioblock-build
if curl -sfS -o SHA256SUMS \$url/SHA256SUMS 2>/dev/null; then
	for p in \$(awk '{ print \$2 }' SHA256SUMS); do curl -sfS -O \$url/\$p; done
	shasum -a 256 -c SHA256SUMS
	for p in $PACKAGES; do installer -pkg \$p.pkg -target / | tail -1; done
fi
rm -rf virtioblock && curl -sfS \$url/src.tgz | tar -xzf -
cd virtioblock && make ABI=intel
cd build/intel && rm -rf dist && mkdir dist && cp -R VirtioBlock.kext dist/
EOF

{
	echo "VirtioBlock $(sed -n 's/^VERSION *?= *//p' "$here/Makefile"), make ABI=intel"
	echo "source: $(git -C "$here" rev-parse HEAD)$(git -C "$here" diff --quiet HEAD -- . || echo ' (modified)')"
	echo "built on: $RELEASE, $(guest /usr/bin/sw_vers -productVersion) $(guest /usr/bin/sw_vers -buildVersion)"
	echo "toolchain: $toolchain"
	guest /bin/sh -c 'gcc-4.0 --version | head -1; gcc-4.2 --version | head -1'
	echo "kext: $(guest /usr/bin/lipo -info /tmp/virtioblock-build/virtioblock/build/intel/dist/VirtioBlock.kext/Contents/MacOS/VirtioBlock | sed 's/.*: //')"
} > "$work/BUILD-INFO"
pod sh -c 'cat > /share/virtioblock-build/BUILD-INFO' < "$work/BUILD-INFO"
guest /bin/sh -c "cd /tmp/virtioblock-build/virtioblock/build/intel/dist && curl -sfS -o BUILD-INFO http://$gateway:$PORT/BUILD-INFO && tar -czf ../virtioblock-intel.tar.gz VirtioBlock.kext BUILD-INFO"

mkdir -p "$out"
guest /usr/bin/openssl base64 -in /tmp/virtioblock-build/virtioblock/build/intel/virtioblock-intel.tar.gz | base64 -d > "$out/virtioblock-intel.tar.gz.new"
want=$(guest /usr/bin/shasum -a 256 /tmp/virtioblock-build/virtioblock/build/intel/virtioblock-intel.tar.gz | awk '{ print $1 }')
got=$(shasum -a 256 "$out/virtioblock-intel.tar.gz.new" | awk '{ print $1 }')
[ "$want" = "$got" ] || { echo "the tarball came back as $got, the guest has $want" >&2; exit 1; }
mv "$out/virtioblock-intel.tar.gz.new" "$out/virtioblock-intel.tar.gz"
cat "$work/BUILD-INFO"
echo "sha256: $got"
