# virtio_rng for Mac OS X

`VirtioRand.kext` 0.1.0, for `virtio-rng-pci (1af4:1005)`. A virtio entropy device feeding the kernel's `/dev/random`: 256 bytes at load, then 64 bytes every 10 seconds.

**Maturity:** **Beta** on PowerPC: proven on 10.2.8, loaded by hand and at boot; the `gcc2` build loads on 10.1 but has not run with a device on 10.0 or 10.1. Not needed on 10.4 and later.

## Build

The kext is built inside a Mac OS X guest with that guest's own Developer Tools: no cross compiler
understands these kernels' C++ ABIs. Each build leaves `build/<ABI>/VirtioRand.kext`.

| ABI | Runs on | Built on | Command |
| --- | --- | --- | --- |
| `gcc2` | 10.0, 10.1 | a 10.1 guest, December 2001 Developer Tools, 10.0's Kernel.framework headers and kmod libraries | `make ABI=gcc2 KERNEL_HEADERS=<10.0 Headers> KMOD_LIB_DIR=<10.0 lib>` |
| `gcc3` | 10.2, 10.3 | a 10.2 guest, December 2002 Developer Tools | `make ABI=gcc3` |

`make clean` removes `build/`. `NOTES.md` says where each toolchain comes from and why each flag is
there.

## Install

Copy the kext to `/System/Library/Extensions`, owned `root:wheel`, then `touch
/System/Library/Extensions`; it loads at the next boot, or now with `kextload`. The packages from
this organisation's
`macos_virtio_rng` generic package, or from the update publisher's `macos_virtio_rng/`, do the same.

## Host side

    -object rng-random,id=rng0,filename=/dev/urandom -device virtio-rng-pci,disable-modern=on,rng=rng0

## Testing

`test.sh` in the guest, as root, with the kext loaded.

## CI

[`.forgejo/workflows/macos-virtio_rng.yml`](../../.forgejo/workflows/macos-virtio_rng.yml) builds the variants in
[`component.sh`](component.sh) in the Panther guest of the `macosx-ppc` runner, packages and checks
them, and publishes them on each push to `main` that touches this driver.
