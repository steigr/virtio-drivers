# virtio_9p for IRIX

`virtio_9p.o`, a loadable filesystem for virtio-9p: it mounts a host directory as type `9p`, with no
network or NFS involved. Programs run from it and `mmap` works. `mount_9p` is the helper
`mount -t 9p` runs. Formerly `p9fs`.

**Maturity:** **Beta.** Runs end to end on the IP28 and the IP35 under IRIS, and carries the IRIX
runners' `/share`; compiles for IP22, IP24 and IP30, which have never run it.

## Build

Inside IRIX 6.5 with MIPSpro, `smake` and the kernel build environment
(`/var/sysgen/Makefile.kernloadio`):

```sh
smake            # /tmp/virtio_9p.<board>/mod/virtio_9p.o and mount_9p; CPUBOARD=IP30 for another board
smake load       # ml ld -j ... -p virtio_9p_ -a 9p; smake unload undoes it
smake install    # load it at every boot, and mount_9p; smake uninstall undoes it
```

Or install the package: `inst -f virtio_9p-latest.tardist`, then `/etc/init.d/virtio_9p start`.

## Use

```sh
mount -t 9p share /mnt/host                       # the first word is the device's tag
mount -t 9p -o aname=some/dir,ro share /mnt/host  # a directory of it, read-only
mount -t 9p -o owners share /mnt/host             # the guest's user and group go to the host
```

or `share /mnt/host 9p rw 0 0` in `/etc/fstab`, which the init script mounts at boot.

- Without `owners` every file has one owner, the device's `uid` and `gid`, and a `chown` has no
  effect. With it, and the device's `owners = "passthrough"`, creates and `chown` reach the host.
- Data is cached close to open, as over NFS. Locks are the guest's own.
- FIFOs and device files on the host do not open, and the guest cannot create any.
- One 9p device per machine. A guest cannot boot from one, and a mount cannot be exported over NFS.

## Host side (IRIS)

```toml
[virtio_9p]
enabled = true
slot    = "exp0"                  # on an Indigo2, see ../README.md
path    = "/path/on/the/host"
tag     = "share"
uid     = 0
gid     = 0
```

## Testing

`make -C tests` runs the 9P client's unit tests on any host. `tests/run_tests.py` drives a guest
booted by `../tests/boot-ip28.sh` with `P9_SHARE` set ([../CLAUDE.md](../CLAUDE.md)).

## CI

[`.forgejo/workflows/irix-virtio_9p.yml`](../../.forgejo/workflows/irix-virtio_9p.yml) builds the package
for IP22, IP28, IP30 and IP35 in the `irix` runner's guest, installs it with inst (and keeps it: the
runner's `/share` is a 9p mount), and publishes it on each push to `main` that touches this driver or
`../virtio`.
