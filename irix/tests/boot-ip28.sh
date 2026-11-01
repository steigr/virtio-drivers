#!/bin/sh
# Boot an installed IP28 IRIX under iris with a virtio-serial device, this
# repository as the guest's /share, and copy-on-write disk writes.
#
#   IRIS=path/to/iris tests/iris/boot-ip28.sh [workdir]
#
# IRIS            iris built from the virtio-serial branch (required)
# IRIX_INSTANCE   the installed system (default $HOME/Developer/irix-ip28):
#                 irix65.raw, ip28-nvram.bin, ip28-nveeprom.bin
# VIRTIO_SLOT     GIO slot, exp0 (interrupts) or exp1 (polled: an Indigo2 kernel
#                 dispatches no slot 1 interrupts).  Default exp0.
#
# Disk writes go to irix65.raw.overlay beside the image, so the base image
# stays clean and deleting the overlay undoes everything.  The ports are
# chosen not to collide with the instance's own ip28-*.toml configurations.
set -eu
: "${IRIS:?set IRIS to an iris binary built from the virtio-serial branch}"
inst=${IRIX_INSTANCE:-$HOME/Developer/irix-ip28}
slot=${VIRTIO_SLOT:-exp0}
root=$(cd "$(dirname "$0")/../.." && pwd)
work=${1:-$(mktemp -d "${TMPDIR:-/tmp}/virtio-iris.XXXXXX")}

# Two emulators on one overlay ruin the filesystem in it, and only an iris with
# overlay locking refuses by itself.
if [ -e "$inst/irix65.raw.overlay" ] && lsof -t "$inst/irix65.raw.overlay" >/dev/null 2>&1; then
	echo "$inst/irix65.raw.overlay is in use by another emulator" >&2
	exit 1
fi
mkdir -p "$work"

cat > "$work/iris.toml" <<EOF
headless   = true
no_audio   = true
banks      = [512, 512, 0, 0]
prom       = ""
nvram      = "$inst/ip28-nvram.bin"
nveeprom   = "$inst/ip28-nveeprom.bin"
serial_log = "$work/console.log"
ci_socket  = "tcp:127.0.0.1:19861"
monitor_port = 18888
[machine]
profile = "indigo2_ip28"
cpu     = "r10000"
[graphics]
board = "solidimpact"
heads = 1
resolution = "guest"
[scsi.1]
path    = "$inst/irix65.raw"
overlay = true
cdrom   = false
[[port_forward]]
proto = "tcp"
host_port = 12323
guest_port = 23
bind = "localhost"
[nfs]
shared_dir = "$root"
[virtio_serial]
enabled = true
slot    = "$slot"
[[virtio_serial.port]]
console  = true
tcp_port = 18890
[[virtio_serial.port]]
name   = "org.iris.agent.0"
socket = "agent.sock"
[[virtio_serial.port]]
name   = "org.iris.unwired.0"
EOF

echo "workdir: $work"
cd "$work"
exec "$IRIS" --config "$work/iris.toml"
