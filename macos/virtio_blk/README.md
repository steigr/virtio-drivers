# virtio_blk for Mac OS X

`VirtioBlock.kext` 0.2.7, for `virtio-blk-pci (1af4:1001)`. A virtio block device as a disk: an `IOBlockStorageDevice`, so `/dev/disk<n>` and its partitions appear as for any disk. Can be the boot disk.

**Maturity:** **Stable** on Intel (10.4 to 10.13 install onto and boot from it; 10.14 and 10.15 use Apple's driver). **Beta** on PowerPC: proven on 10.2.8 and in the gauntlet's add-ons; the `gcc2` build on 10.0 and 10.1 and the 10.3 build are not checked live.

## Build

The kext is built inside a Mac OS X guest with that guest's own Developer Tools: no cross compiler
understands these kernels' C++ ABIs. Each build leaves `build/<ABI>/VirtioBlock.kext`.

| ABI | Runs on | Built on | Command |
| --- | --- | --- | --- |
| `gcc2` | 10.0, 10.1 | a 10.1 guest, December 2001 Developer Tools, 10.0's Kernel.framework headers and kmod libraries | `make ABI=gcc2 KERNEL_HEADERS=<10.0 Headers> KMOD_LIB_DIR=<10.0 lib>` |
| `gcc3` | 10.2, 10.3 | a 10.2 guest, December 2002 Developer Tools | `make ABI=gcc3` |
| `intel` | 10.4 to 10.15 | a 10.6 guest, Xcode 3.2.6 with the 10.4u and 10.6 SDKs (i386 and x86_64 slices joined by lipo) | `make ABI=intel` |

`make clean` removes `build/`. `NOTES.md` says where each toolchain comes from and why each flag is
there.

## Install

Copy the kext to `/System/Library/Extensions`, owned `root:wheel`, then `touch
/System/Library/Extensions`; it loads at the next boot, or now with `kextload`. The packages from
this organisation's
`macos_virtio_blk` generic package, or from the update publisher's `macos_virtio_blk/`, do the same.

## Host side

    -drive file=disk.img,if=none,id=vd0,format=raw -device virtio-blk-pci,disable-modern=on,drive=vd0

## Testing

`test.sh <disk> <MB>` in the guest, as root, with a scratch disk: it overwrites the disk's first megabytes.

## CI

[`.forgejo/workflows/macos-virtio_blk.yml`](../../.forgejo/workflows/macos-virtio_blk.yml) builds the variants in
[`component.sh`](component.sh) in the Panther guest of the `macosx-ppc` runner, packages and checks
them, and publishes them on each push to `main` that touches this driver.
