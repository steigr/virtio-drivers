# VirtioVsock

A Mac OS X kernel extension that drives QEMU's virtio socket device, `vhost-vsock-pci`
(1af4:1053), for PowerPC Mac OS X 10.0 to 10.3 and Intel Mac OS X 10.4 to macOS 10.15, from one
source: stream sockets between the guest and the host (or other guests) by CID and port, with no
network in between. It is the guest agent's fast channel (`../../software/qemu-guest-agent`) on
every release whose agent is ours. See `VirtioVsock.h` for the design.

It is not a BSD socket family: 10.0 to 10.3 give a kext no way to add one. The driver is an
`IOService` with an `IOUserClient`, and `libvsock` (`vsock.h`, `vsock.c`) wraps that in a
socket-like C API (`vsock_listen`, `vsock_accept`, `vsock_connect`, `vsock_read`, `vsock_write`,
...). Its handles are not file descriptors: they work with those functions only, not with
`read(2)` or `select(2)`. `vsockcat` is a netcat for them and the driver's test client.

## Host side

    -device vhost-vsock-pci,guest-cid=302

The CID must be unique on the QEMU host (the vhost-vsock kernel module hands them out), and QEMU
needs `/dev/vhost-vsock`. The host's own CID is 2. Host programs use `AF_VSOCK`: `host-peer.py`
here is a small one.

## In the guest

The IOKit personality matches the device, so the kext loads at boot when it is in
`/System/Library/Extensions` (owned root:wheel; `touch /System/Library/Extensions` after copying
it there). Loaded by hand, `kextload -v` on a root-owned copy logs `VirtioVsock: cid 302, ...`.
Any process can open the driver; there is no access control yet.

    vsockcat -i                  # our CID
    vsockcat -l 5000             # listen on port 5000, echo each connection back
    vsockcat -c 2 5001 < file    # connect to the host's port 5001, send a file, print the reply

Programs link `vsock.c` and use `vsock.h`; `-framework IOKit -framework CoreFoundation`.

## Modern-only, and big-endian

Every other driver here speaks legacy (0.9.5) virtio. This device has no legacy interface: QEMU
makes it modern-only whatever `disable-modern` says (`info pci` in QEMU 10.2: `1af4:1053`, a
64-bit memory BAR 4, with or without `disable-modern=on`). So the transport is virtio 1.x's:

- Capabilities (id 9) in configuration space name the common, notify, ISR and device structures
  inside the BAR; the driver maps the BAR with `mapDeviceMemoryWithRegister` and, if a BAR cannot
  be mapped, falls back to the `PCI_CFG` capability's window (not exercised: the mapping worked
  on 10.1 and 10.2).
- `VIRTIO_F_VERSION_1` is required; the status goes ACK, DRIVER, FEATURES_OK (read back), then
  DRIVER_OK after the queues are set up.
- Queues are enabled with the three parts of the ring (descriptors, available, used) at separate
  addresses; no MSI-X (INTx and the ISR byte, as the legacy drivers).
- The guest is big-endian and the device little-endian, so ring and packet fields are written a
  byte at a time, and BAR registers go through `lwbrx`/`stwbrx`. elliotnunn/classicvirtio does the
  same for Mac OS 9 (it has no vsock).

The protocol is the specification's "Socket Device": queue 0 receive, 1 transmit, 2 events; a
44-byte header per packet; REQUEST/RESPONSE/RST/SHUTDOWN/RW/CREDIT_UPDATE/CREDIT_REQUEST; credit
flow control with 32 KiB buffered per connection; a TRANSPORT_RESET event resets every connection.
Streams only: a SEQPACKET packet is answered with an RST.

## What it does and does not do

- Up to 32 sockets (listening and connected) over the whole system, 8 queued connections per
  listener, 64 receive and 32 transmit slots of 2 KiB (2000 bytes of payload a packet).
- No call sleeps in the kernel. The first version did, with `commandSleep(..., THREAD_ABORTSAFE)`
  inside the command gate, and on 10.2 a signal, SIGKILL included, neither woke the thread nor let
  the process die; the guest hung shortly after. Now the driver says `kIOReturnNotReady` and
  `libvsock` polls from user space (a few immediate retries, then 5 ms steps), where signals work.
  The cost is up to 5 ms of latency when a call has to wait.
- Reads and writes are limited to 4000 bytes a call (structures travel in-band, in 4096 bytes), and
  exchange a fixed-size structure: 10.0's and 10.1's `IOUserClient` take structure arguments of
  exactly the size the method declares, `kIOUCVariableStructureSize` being later. `libvsock` loops.
- Closing: the handle is gone at once; the driver sends SHUTDOWN and frees the connection when the
  peer's RST comes, or after 8 s.
- Unloads (`kextunload`) and reloads; a killed or interrupted process loses its sockets.
- Not done: SEQPACKET, the `VIRTIO_VSOCK_F_*` features beyond STREAM, MSI-X, access control,
  select/poll integration, a BSD socket family (impossible here).

## Intel

The same source, built against the kernel programming interfaces of 10.4 on, as VirtioNet's Intel
build is (`../virtionet/README.md`'s "Intel: one more build, against the kernel programming
interfaces"):

    make ABI=kpi intel  # on 10.6 with Xcode 3.2.6: build/intel/10.4, build/intel/10.6, vsockcat

| Slice | Compiler, SDK | For |
| --- | --- | --- |
| i386 | GCC 4.0, 10.4u SDK | the 32-bit kernels of 10.4 to 10.7, a relocatable object |
| x86_64 | GCC 4.2, 10.6 SDK | the 64-bit kernels of 10.6 on, a kext bundle |

`build/intel/10.6/VirtioVsock.kext` is the universal kext, which OpenCore injects from the ESP on
10.6 to 10.15; `build/intel/10.4/` its i386 slice alone, for /System/Library/Extensions on 10.4 and
10.5 (10.5 refuses a universal kext whose x86_64 slice is a kext bundle). The orchestrator's
legacyintel installer puts both on the installed system (`addonKexts`).
`../../ladder/intel-build.sh drivers/virtiovsock "ABI=kpi intel" build/intel/ dist/virtiovsock-intel`
builds them in the cluster's Snow Leopard guest.

What differs on Intel is chosen in the source by architecture, not by ABI:

- The BAR's registers are little-endian, the processor's own order: plain loads and stores, where
  PowerPC byte-reverses them (`lwbrx`/`stwbrx`) and orders stores with `eieio`. The rings were
  already written a byte at a time, which serves both.
- `mfence` for PowerPC's `sync`.
- A DMA address's high half: on the 64-bit kernels `IOPhysicalAddress` is 64 bits and memory above
  4 GB can be handed out, so the descriptors' and the queue registers' high words are written. On a
  32-bit kernel they stay zero.
- `IOProbeScore` 50000: AppleVirtIO's generic transport on 10.14 and 10.15 matches every virtio
  device, this one too, and puts nothing on it.

`libvsock` takes 10.0's `IOConnectMethod*` calls in a 32-bit process and 10.5's `IOConnectCall*Method`
in a 64-bit one, which has only those -- so it builds against a current SDK too, and the guest agent
links it on every platform. The driver's method table is the old kind, which IOUserClient
translates for 32- and 64-bit callers alike. In a 64-bit process `vsock_open` says ENXIO where
IOKit is not there at all: the agent's x86_64 slice links it weakly for Tiger, which runs that
slice and has no 64-bit IOKit.

On the host, the device goes at a fixed slot, 0x17 (`internal/machine`'s `q35VsockSlot`): numbered
with the others, between the virtio NIC and the 9p share, it moved them, and Snow Leopard's agent
on pci-serial lost most of its input -- 267 resyncs and 76 partial requests to 223 whole ones in
16 minutes; at 0x17, 1 resync and none partial in 185.

## Why it is built twice, and in the guests

As VirtioConsole (`../ppc/virtioconsole/README.md`, which also says where the toolchains come from):
the kernel's C++ ABI differs between GCC 2.95 (10.0, 10.1) and GCC 3 (10.2 on).

| Build | For | Built on | Headers and kmod libraries |
| --- | --- | --- | --- |
| `gcc2` | 10.0, 10.1 | a 10.1 guest, December 2001 tools | 10.0's, from the March 2001 tools |
| `gcc3` | 10.2, 10.3 | a 10.2 guest, December 2002 tools | the December 2002 tools' own |

    make ABI=gcc2 KERNEL_HEADERS=/tmp/k100/Headers KMOD_LIB_DIR=/tmp/k100/lib   # on 10.1
    make ABI=gcc3                                                                # on 10.2

Each leaves `build/<ABI>/VirtioVsock.kext` and `build/vsockcat` (the tools build on either).
Neither build declares `com.apple.kernel.bsd` (no one version satisfies both 10.0 and 10.1, see
`../virtio9p`): the driver uses no BSD symbol. Both build without a warning. The build and copy steps
are `../virtio9p/README.md`'s "Rebuilding, step by step", with `virtiovsock` for `virtio9p`; note
that a guest's `/tmp` is emptied at boot, so 10.0's headers (`k100`) must be copied in again.

## Testing

`test.sh` runs on the host, drives the guest over SSH and the host's end in the pod
(`host-peer.py`, which needs `AF_VSOCK`):

    sh test.sh POD SSH_PORT CID [megabytes]       # sh test.sh jaguar-guest 2202 302 4

It checks the CID the driver reports, a host-to-guest connection with random bytes echoed back and
compared, a guest-to-host connection whose bytes arrive with the same sha1, a refused connection
each way, and that no `vsockcat` is left behind.

## Evidence

QEMU's mac99 in the `cheetah-guest` (CID 303), `puma-guest` (301), `jaguar-guest` (302) and `panther-guest` (304) pods
with `-device vhost-vsock-pci,guest-cid=...`, 2026-10-02:

- `gcc2` on 10.0.4 and 10.1.5, `gcc3` on 10.2.8 and 10.3.9, loaded by hand, interrupt-driven, 64
  receive and 32 transmit slots: the checks of `test.sh` all pass (on 10.0 and 10.3 run through
  the guest agent, those guests having no SSH: CID, 4 MB echoed from the host, 4 MB sent to the
  host with the same sha1, a refused connection each way). 4 to 16 MB each way at about 4 MB/s
  under TCG. The CID was read correctly through the byte-swapped device configuration on all four.
- SIGINT and `kill -9` on a process blocked in `accept` end it (10.1, 10.2); the port can be
  listened on again at once, and `kextunload`/`kextload` still work.
- Polled (`Polled` set to `true`, 10.1.5): `test.sh` 7 of 7.
- Installed in `/System/Library/Extensions` and the guest restarted (10.2.8): the kext loaded at
  boot ("cid 302 ... interrupt-driven") and `vsockcat -i` worked.

Intel, 2026-10-12, `snowleopard-guest` (10.6.8, its 64-bit kernel) with `-device
vhost-vsock-pci,guest-cid=3000006006,addr=0x17`, the 10.6 bundle loaded by hand: "cid 3000006006,
64 receive and 32 transmit slots, interrupt-driven"; `vsockcat -i` read the CID; 8 MB echoed host to
guest to host intact in 2.7 s; 8 MB sent guest to host with the same sha1; a refused connection each
way. The guest agent 0.3.0 on vsock port 7710 from the pod (AF_VSOCK): sync, ping, osinfo, exec, and
1 MiB of `/mach_kernel` through `guest-file-read` at 3028 KB/s -- where the agent on pci-serial took
30 s for the same megabyte.

Intel 32-bit, 2026-10-12, `tiger-guest` (10.4.11): the firmware puts BAR 4 above 4 GB (0x800004000), which
Tiger's 32-bit kernel records as 0x4000; 0.2.0 mapped that and read zeros. 0.2.1 leaves such a BAR
unmapped and goes through the PCI_CFG window, polled ("BAR 4 is at 0000000800004000, above what this
kernel can map"): the echo (4 MB in 1.6 s), the guest-to-host sha1 and the refusals passed, and from
/System/Library/Extensions the guest's agent was on vsock from the host's first connection, 1 MB
through `macos exec` in 1.1 s. A 32-bit process on Tiger, since Tiger runs a universal binary's
x86_64 slice and has no 64-bit IOKit: `vsockcat` thinned to i386, as the installer thins the agent.

The same on 10.8.5 (`mountainlion-guest`, 64-bit kernel), for another reason: its IOPCIConfigurator moved
BAR 4's record below 4 GB (0x81004000) and the device kept decoding at 0x881004000. 0.2.2 compares
the recorded address with the BAR register and maps only where they agree; elsewhere the window,
polled. Loaded from the ESP: the agent on vsock from the host's first connection, 1 MB through
`macos exec` in 1.06 s. 10.6.8 and 10.7.5, whose BAR is not moved, map it and run interrupt-driven.

## Not yet

- The PCI configuration window fallback (the BARs were always mappable), and the watchdog's switch
  to polling when an interrupt never comes.
- Loading at boot on 10.0, 10.1 and 10.3.
- A BSD socket family on Intel (10.4 has the KPI for one): `libvsock` serves the agent as it is.

## References

- The virtio 1.x specification: "Virtio Over PCI Bus" (capabilities, common configuration) and
  "Socket Device" (queues, header, flow control, events).
- elliotnunn/classicvirtio (MIT): modern virtio on QEMU's mac99 with a big-endian guest, for Mac OS
  9; `transport-ndrv.c` for the capability walk. No vsock there.
- `VirtioNet` here, for the IOKit side (work loop, filter interrupt, contiguous slots).

10.0.4 (`cheetah-guest`, CID 303) and 10.3.9 (`panther-guest`, CID 304), 2026-10-04: the ladder's builds
(gcc2 built on 10.0.4, gcc3 on 10.3.9 with Xcode 1.1) loaded with `kextload`, and the native guest
agent (`software/qemu-guest-agent`) served its QGA protocol and the guest's log over them to the pod
(AF_VSOCK): 1 MiB at 1119 and 948 KB/s.
