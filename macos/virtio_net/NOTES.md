# VirtioNet

A Mac OS X kernel extension that drives QEMU's virtio network device, `virtio-net-pci`
(1af4:1000), as an Ethernet port: for PowerPC Mac OS X 10.0 to 10.3 and for Intel Mac OS X 10.4 to
macOS 10.14, from one source (10.15 and later have Apple's own driver, AppleVirtIONetwork). It is an
`IOEthernetController`: IONetworkingFamily makes an `en<n>` of it, and the BSD stack, configd and
the Network preferences treat it as any other Ethernet interface. See `VirtioNet.h` for the design.

## Host side

    -netdev user,id=vn0 -device virtio-net-pci,disable-modern=on,netdev=vn0

Any netdev backend works (`user`, `tap`, `stream`, `passt`, ...). `disable-modern=on` as for
VirtioConsole; a transitional device (without it) has the same legacy I/O BAR and works too.

## In the guest

The IOKit personality matches the device, so the kext loads at boot when it is in
`/System/Library/Extensions` (owned root:wheel; `touch /System/Library/Extensions` after copying
it there so the cache is rebuilt). The interface then appears as the next free `en<n>` and is
configured like any other: in the Network preferences, or by hand:

    ifconfig en2 inet 10.0.2.15 netmask 255.255.255.0 up
    ipconfig set en2 DHCP

## What it does and does not do

- Negotiates MAC (the address QEMU gives the device) and STATUS (link up/down, from QEMU's
  `set_link`). No checksum offload, no TSO, no mergeable receive buffers, no control queue.
- Without the control queue the device does not filter: every frame on the link reaches the
  guest, as on a hub, and the stack drops what is not for it. Promiscuous and multicast requests
  are accepted and change nothing.
- The interrupt is registered only while the interface is up (0.2.2). Registered and left
  disabled, a source on a shared INTx gets the vector masked when an interrupt arrives, and every
  device on the line with it: on q35 an attached-but-never-up second NIC (slot 6) took the guest
  agent's pci-serial (slot 2) down with it, extended gauntlet, 2026-10-11. With 0.2.2 the same
  machine -- 10.6.8 installed from nothing, the NIC at slot 6 beside the pci-serial, VirtioNet
  injected by OpenCore from the installed ESP -- passed: "interrupt-driven while up", "interface
  attached", `en1`, and the agent answered 87 requests in two minutes.
- Interrupt-driven where the platform routes the device's PCI interrupt, which OpenBIOS on QEMU's
  mac99 does: 10.0.4, 10.2.8 and 10.3.9 all took it. Where it cannot be had, the driver polls every
  10 ms; and if, after it had one, received packets wait two watchdog seconds with no interrupt,
  it switches to polling then. `Polled` set to `true` in the personality forces polling.
- 128 receive and 64 transmit slots of 2 KiB (fewer if QEMU's queues are smaller), frames copied
  between the slots and mbufs. MTU 1500.
- Cannot be unloaded once it has attached: 10.0 to 10.3 cannot take an interface out of the BSD
  stack. `kextunload` is refused with result `0xe00002c7` and "en<n> cannot be detached from the
  network stack; not unloading" in the system log, and the interface keeps working. (Before this
  refusal, `kextunload` took the driver away and left `en<n>` listed, pointing at unloaded code.)
- No kernel debugger (KDP) support.

## A trap on the test guests

The test guests have no saved network configuration: their GuestTuning startup item brings each
`en*` up with `ipconfig set <if> DHCP` at boot, and that lasts only until configd next re-reads
its configuration. Loading the kext by hand makes a new interface appear, configd re-reads its
configuration, and the sungem's interface goes down: SSH through it drops. The guest agent still
answers; `ipconfig set en1 DHCP` (or running GuestTuning again) brings it back. Kicker also restarts
lookupd then, and for some seconds `ping` says "unknown protocol icmp". A kext installed in
`/System/Library/Extensions` is loaded before GuestTuning runs and has none of this.

## Intel: one more build, against the kernel programming interfaces

From 10.4 a kext no longer sees `struct mbuf`: it walks packets through the mbuf KPI
(`mbuf_data`, `mbuf_len`, `mbuf_next`, `mbuf_setdata`), and IONetworkController declares
`outputPacket` with the opaque `mbuf_t` -- a driver that kept `struct mbuf *` there would compile and
never be called. `VIRTIONET_KPI` selects that, with the x86 barrier (`mfence`) and a 64-bit
descriptor address; everything else is the PowerPC driver's. Legacy virtio keeps its rings in the
guest's own byte order, so nothing is swapped on either side.

    make ABI=kpi        # on 10.6 with Xcode 3.2.6; leaves build/kpi/VirtioNet.kext

One binary, two slices:

| Slice | Compiler, SDK | For | Mach-O |
| --- | --- | --- | --- |
| i386 | GCC 4.0, 10.4u SDK | the 32-bit kernels of 10.4 to 10.7 | relocatable object (`-r`), as 10.4 and 10.5 load |
| x86_64 | GCC 4.2, 10.6 SDK | the 64-bit kernels of 10.6 to 10.14 (the only kind from 10.8) | kext bundle (`-Xlinker -kext`) |

The libraries are the KPIs at 8.0.0 (Darwin 8, 10.4) and IOPCIFamily and IONetworkingFamily at
1.0.0b1 (`libraries-kpi.xml`): what `kextlibs` asks for, at the oldest version every release
from 10.4 still declares itself compatible with (10.6.8: bsd 10.8.0 compatible back to 8.0.0b1,
iokit to 7.0, libkern to 8.0.0d0; IOPCIFamily 2.6.5 and IONetworkingFamily 1.10 to 1.0.0b1).
`kextlibs` also names two reserved IOService vtable slots the i386 slice refers to, which 10.6's
32-bit kernel calls by other names; the kernel's linker patches vtables by slot, and the guests
here boot 10.6 and later 64-bit, so it has not mattered.

`../../ladder/intel-build.sh drivers/virtionet ABI=kpi build/kpi/VirtioNet.kext dist/virtionet-intel`
does the build in the cluster's Snow Leopard guest: the Xcode packages from the Snow Leopard DVD,
the source, `make`, and the kext back with a `BUILD-INFO` in `guest/dist/virtionet-intel`.

On the Intel releases the orchestrator gives the guest an e1000 (`internal/machine/presets.go`
refuses virtio-net before Big Sur), so a test adds the device beside it, with the chart's
`qemu.extraArgs` and `qemu.policy: loose` (`permissive` drops the forbidden device with a warning):

    qemu:
      extraArgs: [-netdev, "user,id=vn0", -device, "virtio-net-pci,disable-modern=on,netdev=vn0"]
      policy: loose

The kext loads by hand (`kextload`) on every release: OpenCore sets `csr-active-config` to 0x7F
on each boot, which lets SIP load an unsigned kext on 10.11 and later.

## PowerPC: why it is built twice, and in the guests

As VirtioConsole (`../virtio_console/README.md`, which also says where the toolchains come from):
the kernel's C++ ABI differs between GCC 2.95 (10.0, 10.1) and GCC 3 (10.2 on).

| Build | For | Built on | Headers and kmod libraries |
| --- | --- | --- | --- |
| `gcc2` | 10.0, 10.1 | a 10.1 guest, December 2001 tools | 10.0's, from the March 2001 tools |
| `gcc3` | 10.2, 10.3 | a 10.2 guest, December 2002 tools | the December 2002 tools' own |

    make ABI=gcc2 KERNEL_HEADERS=/tmp/k100/Headers KMOD_LIB_DIR=/tmp/k100/lib   # on 10.1
    make ABI=gcc3                                                                # on 10.2

Each leaves `build/<ABI>/VirtioNet.kext`. What differs from VirtioConsole's build, and why:

- The `gcc2` build adds `-fpermissive`. `IOEthernetController.h` includes `netinet/in.h`, whose
  `struct ip_opts` has a member `ip_opts`, and GCC 2.95's C++ makes that an error by default ("ANSI
  C++ forbids data member `ip_opts' with same name as enclosing class"), with 10.0's headers and
  10.1's own alike. With `-fpermissive` it is that one warning, the only one the build prints.
  `-isystem`, which would silence it, makes GCC 2.95 wrap the kernel headers in an implicit
  `extern "C"` and breaks every C++ header.
- Both builds ask for `com.apple.iokit.IONetworkingFamily` 1.0.0b1. It is 1.0.7 inside 10.0's
  System.kext, 1.1 on 10.1, 1.2 on 10.2 and 1.4.0 on 10.3, each compatible back to 1.0.0b1.
- Neither declares `com.apple.kernel.bsd` (no one version satisfies both 10.0 and 10.1, see
  `macos/virtio9p`): the driver uses no BSD symbol. Mbufs are allocated and freed through
  IONetworkController and walked by their fields.

The build and copy steps are `macos/virtio9p's README.md`'s "Rebuilding, step by step", with
`virtionet` for `virtio9p` and `VirtioNet.kext` for `Virtio9P.kext` (there is no `mount_9p`).

## Testing

`test.sh` runs in the guest, as root, with the kext loaded; `echo-server.py` runs beside QEMU,
where a `user` netdev makes its `127.0.0.1:9000` the guest's `10.0.2.2:9000`:

    python3 echo-server.py 9000 &                    # on the QEMU side
    sh test.sh 10.0.2.15 10.0.2.2 9000 4             # in the guest: address, peer, port, MB

It finds the driver's interface by the address the driver logged, configures it statically (no
default route, so it does not take over the guest's other interface), and checks: link active,
ping (small and 1472-byte), ARP, a TCP echo of 4 MB checked byte for byte (`vnecho.pl`, plain perl
5.6), down and up again, another echo, and the interface counters.

## Evidence

QEMU's mac99 in the `cheetah-guest`, `jaguar-guest` and `panther-guest` pods, `-netdev user,id=vn0,...
-device virtio-net-pci,disable-modern=on,netdev=vn0` beside the sungem, 2026-10-01:

- Built with both toolchains (10.1.5, GCC 2.95.2; 10.2.8, GCC 3.1): `gcc3` without warnings,
  `gcc2` with only the `ip_opts` one above.
- Without the device, both builds load and unload: `gcc2` on 10.0.4 (4Q12) and 10.1.5 (5S60b),
  `gcc3` on 10.2.8 (6R73). The `gcc2` build linking on 10.0.4 means every symbol it imports is
  in 10.0's kernel.
- With it, loaded by hand, interrupt-driven, `test.sh` 11 of 11: `gcc2` on 10.0.4 ("VirtioNet:
  52:54:00:12:34:56, I/O at 0x1080, 128 receive and 64 transmit slots, interrupt-driven", as
  `en1`), `gcc3` on 10.2.8 (`en2`) and 10.3.9 (7W98, `en2`). Pings to the slirp gateway around
  1 ms; the TCP echo moved 8 to 16 MB intact at 5.4 to 8 MB/s each way under TCG.
- On 10.2.8, SSH into the guest through the virtio interface (slirp `hostfwd` to dropbear on
  10.0.2.15): an 8 MB random file copied in and out with `scp` had the same MD5 on both sides.
- `kextunload` refused, the module kept and the interface still working: 10.0.4, 10.2.8 and
  10.3.9.
- Installed in `/System/Library/Extensions` and the guests restarted: the kext loaded at boot on
  all three, GuestTuning's `ipconfig set ... DHCP` got 10.0.2.15 from slirp's DHCP server on the
  virtio interface, the default route stayed on the sungem, and `test.sh` passed 11 of 11 again,
  with 16 MB echoed at 8 MB/s (10.0.4) and 5.4 MB/s (10.3.9) interrupt-driven.
- Polled (`Polled` set to `true`, 10.2.8, loaded at boot): 11 of 11, pings 1 to 11 ms (6 average),
  16 MB echoed at 3.2 MB/s each way.

Version 0.2.0, the one source for both platforms, 2026-10-09/10 in the cluster:

- PowerPC, each in place of the add-ons' 0.1 as the guest's only NIC (`virtio-net-pci` on passt),
  installed in `/System/Library/Extensions` and the guest restarted: `gcc2` (built on `puma-guest`,
  10.1.5, with 10.0's headers and kmod libraries) on 10.0.4 (`cheetah-guest`, an 8 MB TCP echo intact
  at 4 MB/s each way) and 10.1.5 (`puma-guest`, the first live 10.1 run: an 8 MB file over HTTP with its
  MD5); `gcc3` (built on `jaguar-guest`, 10.2.8) on 10.2.8 (8 MB at 7.2 MB/s, MD5 intact) and 10.3.9
  (`panther-guest`, 8 MB echo intact at 2 MB/s). The `gcc3` build without warnings, `gcc2` with only
  the `ip_opts` one.
- Intel, the device beside the e1000 (above), the kext loaded by hand, `test.sh` 11 of 11 (8 MB
  echoed): 10.5.8 (the 10.4 bundle), 10.6.8 (64-bit kernel; the release `snowleopard-guest`),
  10.7.5, 10.8.5, 10.9.5, 10.10.5, 10.11.6, 10.12.6 and 10.13.6 (the 10.6 bundle; 10.13's approval
  of third-party kexts did not stop it under OpenCore's `csr-active-config`). On 10.4.11 the kext
  loaded, attached and named the interface (`en1`, interrupt-driven), and Tiger then panicked in
  its pmap, as it does without it (TODO.md's "Tiger and Leopard lose pages in their pmap"), so
  no traffic test there yet.
- Once on 10.2.8, the first restart after swapping the kext in ended at the panic screen with no
  log to read (OpenBIOS keeps no panic NVRAM); the guest booted with the new kext on the next
  restart and on every one after, and 10.0 to 10.3 were then swapped with `Extensions.mkext`
  deleted first, which none of them minded.

- The extended gauntlet, 2026-10-12 (the guest image, every release installed from nothing, an
  attach-only second virtio NIC beside a second virtio disk in the default slots): 0.2.2 named
  `en1` on every Intel release from 10.4 to 10.14 -- from /System/Library/Extensions on 10.4 and
  10.5, injected by OpenCore on 10.6 to 10.14 (10.9 and 10.10 once IONetworkingFamily was forced
  into their prelinked kernel) -- with the guest agent's pci-serial unharmed; 10.15 used Apple's
  AppleVirtIONetwork; PowerPC 10.0 to 10.3 passed their network item with the 0.2.0 add-ons. The
  grid is docs/gauntlet/2026-10-12-extended in the extended-gauntlet branch.
- Before 0.2.1, 10.14 with OpenCore's injection: VirtioNet started on one boot and on the next did
  not, AppleVirtIO's generic AppleVirtIOPCITransport holding the device with nothing on it. 0.2.1
  asks for it with IOProbeScore 50000.

- PowerPC with 0.2.2, 2026-10-12: the add-ons rebuilt by the local ladder (source 5a5184e) passed
  `virtio-test` on 10.0 to 10.3, and installed from nothing in the cluster (the guest image) each
  release passed the gauntlet with 0.2.2 as its only NIC, first attempt, no item failed.

## Not yet

- Traffic on 10.4 and 10.14: the gauntlet's check is attach-only, and Tiger's pmap panics stopped
  the hand test; 10.5 to 10.13 moved 8 MB each by hand.
- 10.14: its SIP is fully on in the test guest ("Untrusted kexts are not allowed", the kext
  staged and refused), so it is loaded by OpenCore's injection or not at all; not yet run that way.
  10.15 needs none of this: Apple's AppleVirtIONetwork (com.apple.driver.AppleVirtIO) drives the
  device -- on 10.15.8, 2026-10-10, it took 10.0.2.15 by DHCP and answered pings at 0.3 ms -- so
  VirtioNet is not for 10.15, and is not to be put where it would compete with that driver.
- 10.4 traffic, past Tiger's pmap panics.
- Done since 2026-10-14: virtio-net is the only NIC of 10.4 to 10.15, this kext driving it to 10.14
  (CHANGELOG.md's "2026-10-14: Intel 10.4 to 10.15 boot from virtio-blk with a virtio-net NIC alone, and PowerPC's checks get a second disk and NIC").
- QEMU's `set_link` (the STATUS feature and its configuration interrupt): the test pods' QMP socket
  has one client, the orchestrator.
- Checksum offload, mergeable buffers, the control queue (receive filtering, MAC changes),
  multiqueue (pmj/virtio-net-osx has some of it for 10.5 and later).

## References

- The virtio 0.9.5 specification (legacy PCI) and the virtio 1.x "Network Device" chapter, for
  the device's configuration, features, header and legacy layout rules.
- Apple's xnu-123.5 (`iokit/Drivers/network/drvIntel82557`, `iokit/Families/IONetworking`): the
  10.0 generation's PCI Ethernet driver, and how IONetworkController, IOGatedOutputQueue and
  allocatePacket behave there.
- elliotnunn/classicvirtio (MIT): virtio on QEMU's mac99 with a big-endian guest, for Mac OS 9
  (block, 9P, input, GPU, sound; it has no network driver).
- pmj/virtio-net-osx: virtio-net for 10.5 and later on x86, for the IONetworking side of a
  virtio driver.
