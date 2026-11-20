# virtio_9p for Mac OS X

`Virtio9P.kext` 0.3.0 and `mount_9p`, for `virtio-9p-pci (1af4:1009)`. A QEMU virtio 9P share (VirtFS) as a filesystem, `9p`: a host directory mounted in the guest with `mount_9p <tag> <mountpoint>`, read and written both ways. A 9P2000.L client with a frontend for each VFS: the BSD one of 10.0 to 10.3 and the KPI of 10.4 on.

**Maturity:** **Stable** on PowerPC 10.0 to 10.3 and Intel 10.4 to 10.13: the gauntlet's `9pfs` check ("read and written both ways") passes on every one of them installed from nothing, and the share is in daily use by the test guests. Interrupt-driven since 0.2.0; on a plain MSI vector since 0.3.0 where the device offers one, live on 10.5 only. `test.sh` has run on 10.1 and 10.2, not yet on the Intel releases. 10.14 on use Apple's AppleVirtIO9P. [irix/virtio_9p](../../irix/virtio_9p) is the IRIX counterpart; the two share no code.

## Build

The kext is built inside a Mac OS X guest with that guest's own Developer Tools: no cross compiler
understands these kernels' C++ ABIs. Each build leaves `build/<ABI>/Virtio9P.kext` and `build/<ABI>/mount_9p`.

| ABI | Runs on | Built on | Command |
| --- | --- | --- | --- |
| `gcc2` | 10.0, 10.1 | a 10.1 guest, December 2001 Developer Tools, 10.0's Kernel.framework headers and kmod libraries | `make ABI=gcc2 KERNEL_HEADERS=<10.0 Headers> KMOD_LIB_DIR=<10.0 lib>` |
| `gcc3` | 10.2, 10.3 | a 10.2 guest, December 2002 Developer Tools | `make ABI=gcc3` |
| `kpi` | 10.4 to 10.13 | a 10.6 guest, Xcode 3.2.6 with the 10.4u and 10.6 SDKs (i386 and x86_64 slices joined by lipo) | `make ABI=kpi` |

`make clean` removes `build/`. `NOTES.md` says where each toolchain comes from and why each flag is
there; `build-intel.sh` runs the `kpi` build in a Snow Leopard guest.

## Install

Copy the kext to `/System/Library/Extensions`, owned `root:wheel`, then `touch
/System/Library/Extensions`; it loads at the next boot, or now with `kextload`; copy `mount_9p` to
`/sbin`. The packages from this organisation's
`macos_virtio_9p` generic package, or from the update publisher's `macos_virtio_9p/`, do the same.
On 10.10 to 10.13 no unsigned kext loads from disk: `NOTES.md`'s "Intel" says how those guests get it.

## Host side

    -fsdev local,id=p9,path=/path/to/share,security_model=none
    -device virtio-9p-pci,disable-modern=on,fsdev=p9,mount_tag=host

and in the guest `mount_9p host /Volumes/host`.

## Testing

`test.sh <mount_9p> <tag> <mountpoint>` in the guest, as root: it mounts the share, runs 32 checks in a fresh directory on it, and unmounts.

## CI

[`.forgejo/workflows/macos-virtio_9p.yml`](../../.forgejo/workflows/macos-virtio_9p.yml) builds the variants in
[`component.sh`](component.sh) in the Panther guest of the `macosx-ppc` runner, packages and checks
them, and publishes them on each push to `main` that touches this driver.
