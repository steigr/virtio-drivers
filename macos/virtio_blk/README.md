# VirtioBlock

A Mac OS X kernel extension that drives QEMU's virtio block device, `virtio-blk-pci` (1af4:1001),
as a disk, for PowerPC Mac OS X 10.0 to 10.3 and Intel Mac OS X 10.4 to macOS 10.15, from one
source. It is an `IOBlockStorageDevice` that matches the PCI
device itself, in the place of an ATA or SCSI driver's nub: IOStorageFamily's
`IOBlockStorageDriver` attaches to it, the partition schemes read the partitions, and the BSD layer
makes `/dev/disk<n>`, `/dev/rdisk<n>` of them (HFS, HFS+, UFS volumes mount as from any disk).
See `VirtioBlock.h` for the design.

**Status:** Intel proven on every release from 10.4 to 10.13 as the installer puts it there --
injected by OpenCore from 10.6, in `/System/Library/Extensions` on 10.4 and 10.5 -- and by hand on
10.15 (below, "Intel"); 10.14 and 10.15 keep Apple's own driver. PowerPC `gcc3` proven on 10.2.8
(2026-10-02); the one-source `gcc2` and `gcc3` builds compile, and the extended gauntlet's add-ons
carry it on 10.0 to 10.3. Until 2026-10-10 it lived in `drivers/ppc/virtioblock` and was PowerPC
only.

## Host side

    -drive file=disk.img,if=none,id=vd0,format=raw -device virtio-blk-pci,disable-modern=on,drive=vd0

`disable-modern=on` as for VirtioConsole; a transitional device (without it) has the same legacy
I/O BAR and works too. A read-only image (`readonly=on` on the drive) is mounted read-only.

## In the guest

The IOKit personality matches the device, so the kext loads at boot when it is in
`/System/Library/Extensions` (owned root:wheel; `touch /System/Library/Extensions` after copying it
there). A disk that is new to the guest has no partition map: `diskutil` (10.0 and 10.1:
`diskutil partitionDisk disk1 1 HFS+ Name 100%`; later `partitionDisk` with a scheme) or Disk
Utility initialises it.

## What it does and does not do

- Negotiates RO and FLUSH only. Reads and writes are 512-byte sectors, whatever BLK_SIZE says, a
  chain per request: header, one descriptor for each physically contiguous piece of the buffer
  (zero-copy; the `IOMemoryDescriptor` is wired and walked by physical segment), status byte.
  `doSynchronizeCache` is a `VIRTIO_BLK_T_FLUSH`.
- At most 30 pages per request (`reportMaxRead/WriteTransfer`, 120 KiB), so a request never needs
  more than the 32 data descriptors of its slot. Up to 16 requests are in flight (the queue's size
  divided by 34 descriptors a slot: 7 with QEMU's 256 entries); more wait in a list.
- Interrupt-driven where the platform routes the device's PCI interrupt; otherwise, or when the
  watchdog (every 50 ms while requests are out) finds completions no interrupt announced, polled
  every 2 ms while busy, and every 100 ms while idle to read the ISR (which lowers a stray INTx).
  `Polled` set to `true` in the personality forces polling.
- `doSyncReadWrite` and `doSynchronizeCache` wait for their completion a millisecond at a time;
  called on the driver's own work loop, which alone delivers completions, they drive the device
  themselves instead (0.2.3). Requests out five seconds with no completion are logged once, with
  the ring indices and the ISR: lion-guest (10.7.5 on iMac19,1, polled because its ACPI could not
  enable the PCI links) froze after a security update with every disk request stalled and nothing
  in its log to say where (2026-10-14). Whether that was the wait on the work loop is not known.
- Block numbers are 32 bits in `doAsyncReadWrite`: 2 TB is the most that is reachable.
- Fixed media: not removable, not ejectable, not lockable; no media polling.
- Not tested for unloading with a volume mounted; the storage stack refuses that anyway.

## Why it is built three times, and in the guests

As VirtioConsole and VirtioNet (`../ppc/virtioconsole/README.md`, `../virtionet/README.md`):
the kernel's C++ ABI differs between GCC 2.95 (10.0, 10.1) and GCC 3 (10.2 on); and Intel is
another architecture again, with two kernels of its own.

| Build | For | Built on | Headers and kmod libraries |
| --- | --- | --- | --- |
| `gcc2` | 10.0, 10.1 | a 10.1 guest, December 2001 tools | 10.0's, from the March 2001 tools |
| `gcc3` | 10.2, 10.3 | a 10.2 guest, December 2002 tools | the December 2002 tools' own |
| `intel` | 10.4 to 10.15 | a 10.6 guest, Xcode 3.2.6 from its DVD | i386: the 10.4u SDK's; x86_64: the 10.6 SDK's |

    make ABI=gcc2 KERNEL_HEADERS=/tmp/k100/Headers KMOD_LIB_DIR=/tmp/k100/lib   # on 10.1
    make ABI=gcc3                                                                # on 10.2
    make ABI=intel                                                               # on 10.6

Each leaves `build/<ABI>/VirtioBlock.kext`. The build and copy steps are
`../virtio9p/README.md`'s "Rebuilding, step by step" with `virtioblock` for `virtio9p`. Unlike
VirtioNet's, the `gcc2` build needs no `-fpermissive` (that was `netinet/in.h` through
IOEthernetController.h), and neither build declares `com.apple.kernel.bsd`: no BSD symbol is used.
The library `com.apple.iokit.IOStorageFamily` is asked for at 1.0.0b1 (an assumption, see below).

## Intel

One kext, two slices joined with `lipo`: i386 (GCC 4.0, the 10.4u SDK, linked relocatable as
Tiger's kextload wants it) for every 32-bit kernel, 10.4 to 10.7; x86_64 (GCC 4.2 `-mkernel`, the
10.6 SDK, a kext bundle) for every 64-bit kernel, 10.6 to 10.15. Both declare the KPI libraries
(`libraries-intel.xml`: `com.apple.kpi.*` 8.0.0, IOPCIFamily and IOStorageFamily 1.0.0b1, which
10.4 to 10.15 all satisfy). What differs from the PowerPC builds is in `VirtioBlock.h`'s comment:
byte order (the structures use whole 64-bit fields, the capacity is read as bytes), the barrier,
the physical-segment call, and the 64-bit kernels' IOBlockStorageDevice (64-bit block numbers and
attributes in `doAsyncReadWrite`, no `doSyncReadWrite`, the maximum transfer as properties).

Xcode 3.2.6 is on the Snow Leopard retail DVD (`Optional Installs`); the packages a build needs --
DeveloperToolsCLI, DevSDK, gcc4.0, gcc4.2, MacOSX10.4.Universal, MacOSX10.6 and the two
system-support packages -- install one by one with `installer -pkg`, which puts the SDKs in
`/SDKs` rather than `/Developer/SDKs`; the Makefile looks in both.

The orchestrator's presets say these releases have no virtio-blk driver (they had none before this
one), so a test disk needs `qemu.policy: loose` beside the extra arguments:

    qemu:
      policy: loose
      extraArgs: [-drive, "file=/work/vblk.img,if=none,id=vd0,format=raw",
                  -device, "virtio-blk-pci,disable-modern=on,drive=vd0"]

From 10.11 the guests' OpenCore turns SIP off (`csr-active-config` 0x7f), which lets an unsigned
kext load; 10.10 needs `kext-dev-mode=1` instead.

## Testing

`test.sh` runs in the guest, as root, with the kext loaded and a scratch disk attached;
**it overwrites the first megabytes of the disk**:

    sh test.sh disk1 4        # the device node without /dev/, megabytes

It writes a pseudo-random file to the raw device, reads it back through the raw device with 64 KiB,
512-byte and 120 KiB transfers and through the block device, checks an unaligned read and an
overwrite, and that a read beyond the end fails.

## Evidence

`jaguar-guest` (10.2.8, 6R73), a 64 MB raw image as a second disk beside the boot disk, `-drive
file=/work/vdisk.img,if=none,id=vd0,format=raw -device virtio-blk-pci,disable-modern=on,drive=vd0`:

- `make ABI=gcc3` builds without warnings; `kextload` finds `com.apple.iokit.IOStorageFamily`
  1.0.0b1 satisfied; the driver logs "131072 sectors (64 MB) ... 7 request slots,
  interrupt-driven, flush" (so the capacity's byte order is right).
- IOBlockStorageDriver attaches and makes `disk1`/`rdisk1`; `diskutil list` shows 64.0 MB.
  `kextunload -b` and `kextload` again work with no volume mounted.
- `test.sh` passes on 3 and 8 MB: raw writes and reads at 64 KiB, 512 B and 120 KiB, block-device
  reads, unaligned reads, overwrite, EOF past the end, `newfs_hfs`, mount, a 3 MB file copied in and
  compared, unmount and remount and compared.

A trap found on the way: IOBlockStorageDriver acts only on a *reported change* of media state;
`reportMediaState` saying "present, unchanged" at once leaves it attached and without an IOMedia.

### Intel

**As installed (0.2.2, the extended gauntlet, 2026-10-11 and 12, `the guest image`).** Every Intel
release installed from nothing, then booted with a second virtio disk (`-blockdev null-co`, 64 MB,
`virtio-blk-pci,disable-modern=on`) and a second virtio NIC in their default slots: VirtioBlock
drives the second disk on 10.4 to 10.13, interrupt-driven. The installer's staging lines: "kext=
VirtioBlock.kext into=/Volumes/Macintosh HD/System/Library/Extensions" on 10.4 and 10.5 (thinned to
i386), "into=ESP inject=10.0.0..17.99.99" on 10.6 to 10.13; on 10.14 and 10.15 Apple's
AppleVirtIOBlockStorageDevice drives it. Evidence: the gauntlet's out/gauntlet/2026-10-11-493d061
and 2026-10-12-493d061, and its grid, docs/gauntlet/2026-10-12-extended.

**By hand (0.2.0).** Each release the gauntlet had installed from nothing, a 64 MB raw image as a second disk
(`/work/vblk.img`, the arguments above), the kext loaded from `/tmp` with `kextload`, `test.sh disk1
8`, then `kextunload` (2026-10-10, one build, 0.2.0):

| Release | Kernel | Loads | test.sh | Unloads |
| --- | --- | --- | --- | --- |
| 10.4.10 (8R4088) | i386 | yes | 15 of 15 | yes |
| 10.5.4 (9E25) | i386 | yes | 15 of 15 | yes |
| 10.6.8 (10K549) | x86_64 | yes | 15 of 15 | yes |
| 10.7.5 (11G63) | x86_64 | yes | 15 of 15 | yes |
| 10.8.5 (12F45) | x86_64 | yes | 15 of 15 | yes |

Every one logs "131072 sectors (64 MB), I/O at 0x6000, 7 request slots, interrupt-driven, flush":
the capacity's byte order is right on a little-endian guest, and the PCI interrupt is routed. On
10.6 both slices pass `kextutil -n -t` (i386 against `/mach_kernel`'s i386 kernel). The PowerPC
`gcc3` build of the same source compiles without warnings on 10.2.8.

VirtioBlock was suspected, for a day, of starving the pci-serial 16550 the guest agent uses (the
extended gauntlet, 2026-10-10 and 11: with a second virtio disk and a second virtio NIC, the agent
read nothing on 10.4.11, 10.5.8 and 10.6.8). It was a bystander. QEMU gives the devices slots in
order and q35 shares its PCI interrupt lines by slot; only the NIC's slot changed between the boots
that failed and those that did not. On 10.6.8 the NIC alone, forced onto the 16550's line, killed
the agent; the disk and NIC with the NIC off that line kept it, VirtioBlock driving its disk. A
VirtioNet whose interface never came up held the shared line with its source disabled, and the
vector was masked (fixed in VirtioNet 0.2.2). Along the way the driver polled on Intel (0.2.0, 0.2.1);
0.2.2 is interrupt-driven again, and keeps 0.2.1's ISR read in polled mode. 0.2.2 on 10.6.8 from nothing
(the extended gauntlet, `the guest image`, 2026-10-11), disk and NIC in their default slots, VirtioNet
0.2.2 beside it: VirtioBlock injected and drives the second disk (ioreg: io_virtio_VirtioBlock
registered, matched, active; kextstat: 0.2.2), and the agent on the 16550 answered throughout.

## Not yet

- The `gcc2` build on 10.0 and 10.1, and 10.3 (`gcc3`): `IOStorageFamily` 1.0.0b1 is satisfied on
  10.2.8; the others are not checked. Booting from the disk (OpenBIOS on mac99 has no virtio-blk
  boot support to rely on), a read-only image, the polled mode, and a disk installed in
  `/System/Library/Extensions` loading at boot.
- Multiqueue, indirect descriptors, discard and write-zeroes, larger transfers, TRIM.
- On Intel: a system disk on virtio-blk (OpenCore would have to inject the kext, or the installer
  stage it before the first boot from that disk), and the orchestrator's presets, which still
  forbid virtio-blk before Big Sur; the kext installed in `/System/Library/Extensions` and loading
  at boot; 10.14 and 10.15 beside Apple's own AppleVirtIO.

## References

- The virtio 0.9.5 specification (legacy PCI) and the virtio 1.x "Block Device" chapter, for the
  configuration, request layout and status values.
- elliotnunn/classicvirtio (MIT), `device-block.c`: virtio-blk on QEMU's mac99 for Mac OS 9 — the
  request chain (header, physical extents, status), negotiating RO, and the 512-byte sector rule.
- Apple's xnu-123.5 and IOStorageFamily headers (`IOBlockStorageDevice.h`, `IOBlockStorageDriver.h`,
  `IOStorage.h`) as shipped in the 10.0 tools: the pure virtuals a device implements and what
  IOBlockStorageDriver calls.

## Booting from it

A system on a virtio-blk disk boots with no IDE device at all (10.0.4, 2026-10-03): the transitional
device in slot 0xf, the patched OpenBIOS of `firmware/openbios`, and

    -bios dist/firmware/openbios-ppc \
    -prom-env 'boot-device=/pci@f2000000/scsi@10/disk:2,\\:tbxi' \
    -prom-env 'boot-args=rd=*/pci@f2000000/scsi@10/disk@0:2' \
    -drive file=disk.img,if=none,id=vd0,format=raw -device virtio-blk-pci,drive=vd0,addr=0x10

with this kext in the system's `/System/Library/Extensions`, owned by root (the `VirtioBlock`
package does that). It is `OSBundleRequired` `Local-Root`, so BootX loads it with the kernel, and its
personalities set `IOUnit` 0, so the partitions are in the device tree where the root path finds
them. Why each of these: `firmware/openbios/README.md`. In the installer context the kext comes off
the PROVISION disc (`packaging/installer/virtio.sh`) and the disk is the install's target like any
other.
