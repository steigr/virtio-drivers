# virtio_net for Mac OS X

`VirtioNet.kext` 0.2.2, for `virtio-net-pci (1af4:1000)`. A virtio network device as an Ethernet port: an `IOEthernetController`, so an `en<n>` appears and is configured like any other.

**Maturity:** **Stable** on PowerPC 10.0 to 10.3 (the guests' only NIC) and on Intel 10.5 to 10.13 (8 MB echoed each). **Beta** on 10.4 and 10.14: attaches, traffic not yet tested. 10.15 uses Apple's driver.

## Build

The kext is built inside a Mac OS X guest with that guest's own Developer Tools: no cross compiler
understands these kernels' C++ ABIs. Each build leaves `build/<ABI>/VirtioNet.kext`.

| ABI | Runs on | Built on | Command |
| --- | --- | --- | --- |
| `gcc2` | 10.0, 10.1 | a 10.1 guest, December 2001 Developer Tools, 10.0's Kernel.framework headers and kmod libraries | `make ABI=gcc2 KERNEL_HEADERS=<10.0 Headers> KMOD_LIB_DIR=<10.0 lib>` |
| `gcc3` | 10.2, 10.3 | a 10.2 guest, December 2002 Developer Tools | `make ABI=gcc3` |
| `kpi` | 10.4 to 10.14 | a 10.6 guest, Xcode 3.2.6 with the 10.4u and 10.6 SDKs | `make ABI=kpi` |

`make clean` removes `build/`. `NOTES.md` says where each toolchain comes from and why each flag is
there.

## Install

Copy the kext to `/System/Library/Extensions`, owned `root:wheel`, then `touch
/System/Library/Extensions`; it loads at the next boot, or now with `kextload`. The packages from
this organisation's
`macos_virtio_net` generic package, or from the update publisher's `macos_virtio_net/`, do the same.

## Host side

    -netdev user,id=vn0 -device virtio-net-pci,disable-modern=on,netdev=vn0

## Testing

`echo-server.py 9000` beside QEMU, then `test.sh <address> <peer> 9000 <MB>` in the guest, as root.

## CI

[`.forgejo/workflows/macos-virtio_net.yml`](../../.forgejo/workflows/macos-virtio_net.yml) builds the variants in
[`component.sh`](component.sh) in the Panther guest of the `macosx-ppc` runner, packages and checks
them, and publishes them on each push to `main` that touches this driver.
