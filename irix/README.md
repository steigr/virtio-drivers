# virtio drivers for IRIX

Drivers that let IRIX 6.5.22 use virtio devices under the
[IRIS](irix/iris) emulator:

- **vioser**, for virtio-serial: named, 8-bit-clean byte channels between the guest
  and the host, with no UART, network or disk involved.
- **p9fs**, for virtio-9p: a filesystem that mounts a directory on the host, with
  no network or NFS involved. Programs run from it and `mmap` works, which the NFS
  `/share` does not manage.

The device side is IRIS's `feature/virtio-9p` branch, on top of
`feature/virtio-serial-guest-device-412469`; its `docs/virtio-serial.md` and
`docs/virtio-9p.md` describe what the drivers talk to.

## Status

| Machine | Transport | Status |
|---|---|---|
| Indigo2 IMPACT (IP28) | virtio-mmio on GIO | Runs: tested end to end under IRIS |
| Indigo2 (IP22), Indy (IP24) | virtio-mmio on GIO | Compiles; never run |
| Octane (IP30) | virtio-pci on the Bridge | Compiles; never run |
| Tezro (IP35, IRIX 6.5.30) | virtio-pci on bus 1 of the IO9 PIC | Built on the IP28 (6.5.22 headers) |
| O2 (IP32) | | Not supported: IRIS does not wire the device there |

## Using it

The easy way is the inst packages, one for each driver, from the `irix`
organisation's packages:
[`vioser`](irix/-/packages/generic/vioser/latest) and
[`p9fs`](irix/-/packages/generic/p9fs/latest).

```sh
inst -f vioser-latest.tardist     # installs the module for this board
/etc/init.d/vioser start          # or reboot: it loads at every boot
inst -f p9fs-latest.tardist       # the same, and mount_9p
/etc/init.d/p9fs start
```

The `dev` image of `vioser`, not selected by default, adds `sys/vioser.h`. Removing
a package leaves a loaded module in the kernel until the next reboot. With no
device present a module loads and stays idle.

To build it yourself instead, inside IRIX, with MIPSpro and the kernel headers
installed:

```sh
cd driver
smake            # builds /tmp/vioser.<board>/mod/{vioser,p9fs}.o and mount_9p
smake load       # ml ld of vioser; smake load9p of the filesystem
smake unload     # ml unld; smake unload9p
smake install    # load both at every boot; smake uninstall undoes it
```

### virtio-serial

Each port is a character device:

```
/hw/virtio-serial/port0            every port, by number
/hw/virtio-ports/org.iris.agent.0  named ports, by name
/dev/virtio-ports                  a link to the above, after smake install
```

`read`, `write`, `poll`, `FIONREAD` and `VIOSER_GETINFO` (see
[driver/vioser.h](driver/vioser.h)) work as you would expect of a pipe. One
program can have a port open at a time. `read` returns end of file while no
program is connected to the host end, and what the guest writes then is dropped
by the device. A port is not a tty, so `getty` cannot run on one.

In `iris.toml`:

```toml
[virtio_serial]
enabled = true
slot    = "exp0"                  # see below, on Indigo2

[[virtio_serial.port]]
console  = true
tcp_port = 8890

[[virtio_serial.port]]
name   = "org.iris.agent.0"
socket = "/tmp/iris-agent.sock"
```

### virtio-9p

```sh
ml ld -j /usr/lib/p9fs/p9fs.o -p p9fs_ -a 9p     # what /etc/init.d/p9fs does
mount -t 9p share /mnt/host                      # the first word is the device's tag
mount -t 9p -o aname=some/dir,ro share /mnt/host # a directory of it, read-only
mount -t 9p -o owners share /mnt/host            # the guest's user and group go to the host
umount /mnt/host
```

or in `/etc/fstab`, which `/etc/init.d/p9fs` mounts at boot:

```
share /mnt/host 9p rw 0 0
```

`mount_9p` in `/usr/etc` is the helper `mount -t 9p` runs. In `iris.toml`:

```toml
[virtio_9p]
enabled = true
slot    = "exp0"                  # see below, on Indigo2
path    = "/path/on/the/host"
tag     = "share"
uid     = 0                       # who every file appears to belong to
gid     = 0
```

What to expect of it:

- **`owners` passes the guest's ids to the host.** Without it (and with the device's default
  `owners = "squash"`) there is one owner, as in the next point. With `mount -o owners` the
  driver follows every create, mkdir and symlink with a `Tsetattr` of the creator's user and
  group, and sends a guest `chown`/`chgrp` (CAP_CHOWN needed, or the owner changing the group
  to one it is in). The device, with `owners = "passthrough"` and optionally `uid_map` /
  `gid_map`, makes them on the host (see its `docs/virtio-9p.md`) and reports the host's
  owners back; an id its maps lack is refused (`EPERM`). The device trusts what the guest says.

- **Ownership is one owner for everything**, the configured `uid` and `gid`: the host
  files have one, the user running IRIS. A guest `chown` is accepted and has no
  effect. Permission checks happen in the guest, from those and the host's modes.
- **Data is cached close to open.** A close pushes what was written, and an open or
  read notices that the host has changed the file. Two sides writing one file at
  once get no more than that, as over NFS.
- **Names can change under it**: a file the host renames or removes behaves as the
  guest had looked it up, until it next looks.
- Locks are the guest's own, and do not reach the host.
- FIFOs and device files on the host appear in listings but do not open, and the
  guest cannot create any.

### Use `slot = "exp0"` on an Indigo2

IRIS defaults to `exp1`, but an Indigo2 kernel dispatches no interrupts for
that slot: IP28's `ip22_gio1_intr` only tests the graphics slot and slot 0. The
drivers detect this and poll, so `exp1` works, but `exp0` gets real interrupts.
A slot holds one device, so with both on, give `exp0` to whichever needs the
speed: for a filesystem, that is the one.

## The MIPSpro wrapper

[tools/mipspro-suppress](tools/mipspro-suppress) removes the "requires a license
password" banner from compiler output, and keeps everything else, including real
errors and the exit status. `sh tools/install-wrapper` installs it under
`/usr/local/mipspro/bin`, and `driver/Makefile` uses it from there.

It is a Bourne shell port of
[LaurentChardon/mipspro](https://github.com/LaurentChardon/mipspro), whose
script needs `/usr/freeware/bin/bash`. A stock IRIX install has no bash.

## Tests

See [CLAUDE.md](CLAUDE.md) for how to run them.

## Package builds

[.forgejo/workflows/package.yml](.forgejo/workflows/package.yml) runs on a push to
`main` that touches the driver or the packaging, and on demand. On the `irix`
runner, the IRIX guest compiles both modules for each of IP22, IP28, IP30 and IP35 and runs
`gendist`, then installs and removes each package with `inst`. The runner uploads
them as the generic packages `vioser` and `p9fs`, each under its version and
`latest`, with the organisation secret `PACKAGES_TOKEN`.

The version is an integer, as inst needs: the commit's UTC date and its number
that day, such as `2026110202`.

## Limits

- Ports are found once, when the driver loads. A port added or removed later is
  ignored with a warning.
- A restored IRIS snapshot leaves the devices idle; reload the drivers.
- Both modules load with `ml`. Neither has been linked statically into a kernel.
- One 9p device per machine. A guest cannot boot from one, and a 9p mount cannot be
  exported over NFS.
