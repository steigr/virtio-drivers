# virtio_console for Mac OS X

`VirtioConsole.kext` 0.2.0, for `virtio-serial-pci, legacy console port`. The legacy virtio console as a serial port, `/dev/cu.virtio-0`: the PowerPC guest agent's channel, needing neither network nor USB.

**Maturity:** **Stable** on PowerPC 10.0 to 10.3: interrupt-driven, the gauntlet's checks pass on releases installed from nothing. No Intel build.

## Build

The kext is built inside a Mac OS X guest with that guest's own Developer Tools: no cross compiler
understands these kernels' C++ ABIs. Each build leaves `build/<ABI>/VirtioConsole.kext`.

| ABI | Runs on | Built on | Command |
| --- | --- | --- | --- |
| `gcc2` | 10.0, 10.1 | a 10.1 guest, December 2001 Developer Tools, 10.0's Kernel.framework headers and kmod libraries | `make ABI=gcc2 KERNEL_HEADERS=<10.0 Headers> KMOD_LIB_DIR=<10.0 lib>` |
| `gcc3` | 10.2, 10.3 | a 10.2 guest, December 2002 Developer Tools | `make ABI=gcc3` |

`make clean` removes `build/`. `NOTES.md` says where each toolchain comes from and why each flag is
there.

## Install

Copy the kext to `/System/Library/Extensions`, owned `root:wheel`, then `touch
/System/Library/Extensions`; it loads at the next boot, or now with `kextload`. The packages from
the [`macos_virtio_console`](virtualization/-/packages/generic/macos_virtio_console/latest)
generic package, or from `https://updates.$UPDATES_URL/macos_virtio_console/`, do the same.

## Host side

    -device virtio-serial-pci,disable-modern=on,max_ports=1 -device virtconsole,chardev=qga0,nr=0

## Testing

The guest agent's startup item serves the port with `--tty auto`; `NOTES.md` has the checks.

## CI

[`.forgejo/workflows/macos-virtio_console.yml`](../../.forgejo/workflows/macos-virtio_console.yml) builds the variants in
[`component.sh`](component.sh) in the Panther guest of the `macosx-ppc` runner, packages and checks
them, and publishes them on each push to `main` that touches this driver.
