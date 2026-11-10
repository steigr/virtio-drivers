# virtio-drivers

Guest drivers for virtio devices on systems that never had them: IRIX 6.5 under the
[IRIS](irix/iris) emulator, and Mac OS X 10.0 to 10.15 under QEMU. Each driver
is named after the virtio device it drives, and has its own folder, build file, README and
release workflow.

## Drivers

| Driver | Platform | Device | Version | Maturity |
| --- | --- | --- | --- | --- |
| [virtio_blk](macos/virtio_blk) | Mac OS X | block | 0.2.7 | Stable on Intel 10.4 to 10.13 (boot disk); Beta on PowerPC 10.0 to 10.3 |
| [virtio_console](macos/virtio_console) | Mac OS X | console | 0.2.0 | Stable on PowerPC 10.0 to 10.3 |
| [virtio_input](macos/virtio_input) | Mac OS X | input (tablet, mouse) | 0.1.0 | Beta on PowerPC 10.0 to 10.3 |
| [virtio_net](macos/virtio_net) | Mac OS X | network | 0.2.2 | Stable on PowerPC 10.0 to 10.3 and Intel 10.5 to 10.13; Beta on 10.4 and 10.14 |
| [virtio_rng](macos/virtio_rng) | Mac OS X | entropy | 0.1.0 | Beta on PowerPC 10.0 to 10.3 (proven on 10.2) |
| [virtio_vsock](macos/virtio_vsock) | Mac OS X | socket | 0.2.3 | Beta on PowerPC 10.0 to 10.3 and Intel 10.6 |
| [virtio_serial](irix/virtio_serial) | IRIX | console (multiport) | date | Beta: runs on IP28 and IP35 |
| [virtio_9p](irix/virtio_9p) | IRIX | 9P transport | date | Beta: runs on IP28 and IP35 |

[irix/virtio](irix/virtio) is the IRIX transport layer (split rings, virtio-mmio on GIO,
virtio-pci) that both IRIX drivers link in.

**Stable**: runs end to end on the releases or machines named, in daily use by the test guests and
installers. **Beta**: runs end to end on some of them; the rest compile, or are not yet checked
live. Each driver's README says which.

## Layout

```
macos/<driver>/      sources, Makefile, component.sh (what CI builds), README.md, NOTES.md (design, evidence)
macos/ci/            builds and packages a kext in the Panther guest of the macosx-ppc runner
irix/<driver>/       sources, Makefile (smake), init script, tests, README.md
irix/virtio/         the shared transport layer and virtio.mk, which each IRIX Makefile includes
irix/ci/             builds, installs and describes an inst package in the IRIX guest
ci/                  runner side, both platforms: package registry and update publisher uploads
.forgejo/workflows/  one workflow per driver: <platform>-<driver>.yml
```

## Releases

Each driver's workflow runs on a push to `main` that touches it, and on demand. It builds the
driver in its platform's guest, checks the package there, and publishes it twice:

- to this organisation's generic packages, as `<platform>_<driver>`
  ([list](virtualization/-/packages)), under its version and `latest`;
- to the update publisher in the home cluster (namespace `update publisher`), which serves it at
  `https://updates.$UPDATES_URL/<platform>_<driver>/`; the IRIX packages are listed on
  [irix.the update publisher](https://irix.$UPDATES_URL).

Both uploads use the job's ID token, so the workflows need no secret: the registry accepts it
through an Authorized Integration whose audience is the organisation variable `AUD`, the publisher
through its policy for this repository and each workflow.

## History

The drivers were separate repositories until 2026-11-10, imported here with their history:
`macos/virtioblock`, `virtioconsole`, `virtiotablet`, `virtionet`, `virtiorand`, `virtiovsock` and
`irix/virtio` (whose `vioser` and `p9fs` are now `virtio_serial` and `virtio_9p`).
