# virtio drivers for IRIX

Loadable kernel modules that let IRIX 6.5 use virtio devices under the
[IRIS](irix/iris) emulator. IRIS's `docs/virtio-serial.md` and
`docs/virtio-9p.md` describe the devices they talk to.

| Folder | What |
| --- | --- |
| [`virtio`](virtio) | The transport layer every driver links in: split rings, virtio-mmio on GIO, virtio-pci. Not a module by itself. |
| [`virtio_serial`](virtio_serial) | virtio-serial: named, 8-bit-clean byte channels to the host. |
| [`virtio_9p`](virtio_9p) | virtio-9p: a filesystem that mounts a host directory, as type `9p`. |
| [`ci`](ci) | Package build, install test and changelog, for the workflows. |
| [`tools`](tools) | The MIPSpro wrapper. |
| [`tests`](tests) | `boot-ip28.sh`, which boots an IP28 under IRIS for the end-to-end tests. |

## Machines

| Machine | Transport | Status |
| --- | --- | --- |
| Indigo2 IMPACT (IP28) | virtio-mmio on GIO | Runs: tested end to end under IRIS |
| Tezro (IP35, IRIX 6.5.30) | virtio-pci on bus 1 of the IO9 PIC | Runs under IRIS, modules built on the IP28 |
| Indigo2 (IP22), Indy (IP24) | virtio-mmio on GIO | Compiles; never run |
| Octane (IP30) | virtio-pci on the Bridge | Compiles; never run |
| O2 (IP32) | | Not supported: IRIS does not wire the device there |

## Building

Inside IRIX, with MIPSpro, `smake` and the kernel build environment
(`/var/sysgen/Makefile.kernloadio`, from `eoe.sw.kdev` / the kernel headers). Each driver's
folder has its own Makefile; `smake CPUBOARD=IP30` compiles for another board where the headers
allow. Objects go to `/tmp/<driver>.<board>`, since `/share` may be NFS and `noexec`.

[`tools/mipspro-suppress`](tools/mipspro-suppress) drops MIPSpro's "requires a license password"
banner from compiler output and keeps everything else. `sh tools/install-wrapper` installs it
under `/usr/local/mipspro/bin`, where the Makefiles look for it. It is a Bourne shell port of
[LaurentChardon/mipspro](https://github.com/LaurentChardon/mipspro), whose script needs bash.

## Packages

Each driver is an inst package, with a module for every board in it:
`inst -f <driver>-latest.tardist`, then `/etc/init.d/<driver> start` or a reboot. They are the
generic packages `irix_virtio_serial` and `irix_virtio_9p` of the
[`virtualization`](virtualization/-/packages) organisation, and on
[irix.the update publisher](https://irix.$UPDATES_URL). They replace the packages of the drivers'
old names, `vioser` and `p9fs`. Removing a package leaves a loaded module in the kernel until the
next reboot.

## Tests

Host unit tests, no IRIX needed: `make -C virtio/tests` and `make -C virtio_9p/tests` (ASan and
UBSan). End to end under IRIS: [CLAUDE.md](CLAUDE.md).

## Limits

- A restored IRIS snapshot leaves the devices idle; reload the drivers.
- The modules load with `ml`. Neither has been linked statically into a kernel.
- On an Indigo2, use `slot = "exp0"` in `iris.toml`: an Indigo2 kernel dispatches no interrupts for
  `exp1`, IRIS's default. The drivers detect that and poll, so `exp1` works, but slower. A slot
  holds one device, so with both drivers on, give `exp0` to the one that needs the speed.
