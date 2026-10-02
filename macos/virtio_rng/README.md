# VirtioRand

A Mac OS X kernel extension that feeds the kernel's `/dev/random` generator from QEMU's virtio
entropy device, `virtio-rng-pci` (1af4:1005), for PowerPC Mac OS X 10.0 to 10.3. See `VirtioRand.h`
for the design.

QEMU's mac99 has no hardware random number generator, so Yarrow in these releases is seeded from
timing jitter and the seed file alone. With the device, the host's randomness (the rng backend:
`/dev/urandom`, a hardware generator, whatever the host chose) goes into the generator as well.

## Host side

    -object rng-random,id=rng0,filename=/dev/urandom -device virtio-rng-pci,disable-modern=on,rng=rng0

`disable-modern=on` as for VirtioConsole; a transitional device (without it) has the same legacy
I/O BAR and works too. `rng-builtin` and `rng-egd` backends work as well, and `max-bytes`/`period`
on the device rate-limit what the guest can take; the driver takes 64 bytes at a time.

## In the guest

The IOKit personality matches the device, so the kext loads at boot when it is in
`/System/Library/Extensions` (owned root:wheel; `touch /System/Library/Extensions` after copying it
there so the cache is rebuilt), or by hand with `kextload`. Once up it takes 256 bytes in four
requests straight away, then 64 bytes every 10 seconds, and logs

    VirtioRand: I/O at 0x1180, request queue 8 entries, 64 bytes at a time every 10000 ms
    VirtioRand: primed the generator with 256 bytes from the device

The interval is the personality's `IntervalMS` (at least 100).

## What it does and does not do

- Mixes the bytes in through the kernel's own `random_write` (what a write to `/dev/random` calls),
  given a `struct uio` over kernel memory: Yarrow takes them under its lock, credited at eight bits
  a byte as for any write to `/dev/random`, and reseeds. The generator's state and mutex are static
  in the kernel and cannot be linked to; `random_write` is global and the same in 10.1's and
  10.2's kernels (compared by disassembly, 2026-10-02), bar the name of the mutex routine. The
  bytes are wiped from the driver's buffer once mixed in.
- Does not replace the generator or `/dev/random` itself, and does not feed `/dev/urandom`
  separately: in these releases both are the one Yarrow.
- Polled from a work-loop timer (10 ms while a request is out); the device is told not to
  interrupt. Entropy is slow and a poll is a memory read.
- Unloads cleanly (`kextunload`): it has no interface, port or other client to keep it.
- No feature bits exist for this device, so none are negotiated.

## Why it is built twice, and in the guests

As VirtioConsole (`../virtioconsole/README.md`, which also says where the toolchains come from):
the kernel's C++ ABI differs between GCC 2.95 (10.0, 10.1) and GCC 3 (10.2 on).

| Build | For | Built on | Headers and kmod libraries |
| --- | --- | --- | --- |
| `gcc2` | 10.0, 10.1 | a 10.1 guest, December 2001 tools | 10.0's, from the March 2001 tools |
| `gcc3` | 10.2, 10.3 | a 10.2 guest, December 2002 tools | the December 2002 tools' own |

    make ABI=gcc2 KERNEL_HEADERS=/tmp/k100/Headers KMOD_LIB_DIR=/tmp/k100/lib   # on 10.1
    make ABI=gcc3                                                                # on 10.2

Each leaves `build/<ABI>/VirtioRand.kext`. Neither declares `com.apple.kernel.bsd` (no one version
satisfies both 10.0 and 10.1, see `../virtio9p`); the kernel's `random_write` resolves without it,
as virtio9p's BSD symbols do. Neither build needs `-fpermissive` (the headers it includes are not
`IOEthernetController`'s). The build and copy steps are `../virtio9p/README.md`'s "Rebuilding,
step by step", with `virtiorand` for `virtio9p` and `VirtioRand.kext` for `Virtio9P.kext` (there is
no `mount_9p`).

## Testing

`test.sh` runs in the guest, as root, with the kext loaded:

    sh test.sh

It checks the log (attached, primed, no refusal or stall), reads `/dev/random`, and that the
driver is still up after an interval.

## Evidence

QEMU's mac99 in the `cheetah-guest`, `jaguar-guest` and `puma-guest` pods, with
`-object rng-random,id=rng0,filename=/dev/urandom -device virtio-rng-pci,disable-modern=on,rng=rng0`
on `cheetah-guest` and `jaguar-guest` (puma-guest has no such device), 2026-10-02:

- Built with both toolchains (10.1.5, GCC 2.95.2; 10.2.8, GCC 3.1), both without a warning.

- Loaded by hand on 10.2.8 (6R73, `gcc3`, I/O at 0x1180, 8-entry queue): primed with 256 bytes, `test.sh` 6 of 6, `kextunload` and reload clean.
- Installed in `/System/Library/Extensions` and the guest restarted (10.2.8): loaded at boot, attached and primed.
- `gcc2` on 10.1.5: loads without the device (puma-guest has none). Not yet live with a device: 10.0 and 10.1 (cheetah-guest was reinstalling when this was written).

## Not yet

- 10.4 and later, which need no such driver from here (they have their own, and a different
  generator).
- A way to see the counts from userland: `ioreg` of 10.2.8 stops at the NVRAM node ("can't obtain
  properties"), before it gets to the driver's `BytesFed` and `Requests` properties.

## References

- The virtio 0.9.5 specification (legacy PCI) and the virtio 1.x "Entropy Device" chapter.
- Apple's xnu-123 (`bsd/dev/random/randomdev.c`): `random_write` and Yarrow's use of it.
- elliotnunn/classicvirtio (MIT): virtio on QEMU's mac99 with a big-endian guest, for Mac OS 9.
