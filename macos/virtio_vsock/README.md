# virtio_vsock for Mac OS X

`VirtioVsock.kext` 0.2.3, for `vhost-vsock-pci (1af4:1053)`. A virtio socket device: stream connections to the host by CID and port, with no network in between. Not a BSD socket family: `libvsock` (`vsock.h`, `vsock.c`) gives programs a socket-like API, and `vsockcat` is a netcat for it. The guest agent's fast channel.

**Maturity:** **Beta**: on PowerPC 10.0 to 10.3 and on Intel 10.6 the tests pass, interrupt-driven and polled. Loading at boot is proven on 10.2.8 only.

## Build

The kext is built inside a Mac OS X guest with that guest's own Developer Tools: no cross compiler
understands these kernels' C++ ABIs. Each build leaves `build/<ABI>/VirtioVsock.kext`.

| ABI | Runs on | Built on | Command |
| --- | --- | --- | --- |
| `gcc2` | 10.0, 10.1 | a 10.1 guest, December 2001 Developer Tools, 10.0's Kernel.framework headers and kmod libraries | `make ABI=gcc2 KERNEL_HEADERS=<10.0 Headers> KMOD_LIB_DIR=<10.0 lib>` |
| `gcc3` | 10.2, 10.3 | a 10.2 guest, December 2002 Developer Tools | `make ABI=gcc3` |
| `kpi` | 10.4 to 10.15 | a 10.6 guest, Xcode 3.2.6 with the 10.4u and 10.6 SDKs; `make ABI=kpi intel` adds both Intel bundles and an Intel vsockcat | `make ABI=kpi` |

`make clean` removes `build/`. `NOTES.md` says where each toolchain comes from and why each flag is
there.

## Install

Copy the kext to `/System/Library/Extensions`, owned `root:wheel`, then `touch
/System/Library/Extensions`; it loads at the next boot, or now with `kextload`. The packages from
the [`macos_virtio_vsock`](virtualization/-/packages/generic/macos_virtio_vsock/latest)
generic package, or from `https://updates.$UPDATES_URL/macos_virtio_vsock/`, do the same.

## Host side

    -device vhost-vsock-pci,guest-cid=302

## Testing

`host-peer.py` on the QEMU host, then `test.sh` in the guest; `make tools` builds `vsockcat`.

## CI

[`.forgejo/workflows/macos-virtio_vsock.yml`](../../.forgejo/workflows/macos-virtio_vsock.yml) builds the variants in
[`component.sh`](component.sh) in the Panther guest of the `macosx-ppc` runner, packages and checks
them, and publishes them on each push to `main` that touches this driver.
