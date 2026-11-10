# virtio_input for Mac OS X

`VirtioTablet.kext` 0.1.0, for `virtio-tablet-pci, virtio-mouse-pci (1af4:1052)`. A virtio input device as a pointer: the tablet as an absolute pointer, so the guest's cursor follows the host's over VNC, the mouse as a relative one. Keyboards are left alone.

**Maturity:** **Beta** on PowerPC 10.0 to 10.3: moves, clicks, drags and the wheel pass on every release, loaded by hand and installed. No Intel build.

## Build

The kext is built inside a Mac OS X guest with that guest's own Developer Tools: no cross compiler
understands these kernels' C++ ABIs. Each build leaves `build/<ABI>/VirtioTablet.kext`.

| ABI | Runs on | Built on | Command |
| --- | --- | --- | --- |
| `gcc2` | 10.0, 10.1 | a 10.0 or 10.1 guest, its Developer Tools (GCC 2.95), 10.0's Kernel.framework headers and kmod libraries | `make ABI=gcc2 KERNEL_HEADERS=<10.0 Headers> KMOD_LIB_DIR=<10.0 lib>` |
| `gcc3` | 10.2, 10.3 | a 10.2 or 10.3 guest, its Developer Tools (GCC 3) | `make ABI=gcc3` |

`make clean` removes `build/`. `NOTES.md` says where each toolchain comes from and why each flag is
there.

## Install

Copy the kext to `/System/Library/Extensions`, owned `root:wheel`, then `touch
/System/Library/Extensions`; it loads at the next boot, or now with `kextload`. The packages from
this organisation's
`macos_virtio_input` generic package, or from the update publisher's `macos_virtio_input/`, do the same.

## Host side

    -device virtio-tablet-pci

## Testing

`NOTES.md`, "Testing": the pointer is driven over QMP and checked on screen.

## CI

[`.forgejo/workflows/macos-virtio_input.yml`](../../.forgejo/workflows/macos-virtio_input.yml) builds the variants in
[`component.sh`](component.sh) in the Panther guest of the `macosx-ppc` runner, packages and checks
them, and publishes them on each push to `main` that touches this driver.
