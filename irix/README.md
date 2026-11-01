# virtio drivers for IRIX

Drivers that let IRIX 6.5.22 use virtio devices under the
[IRIS](irix/iris) emulator. The first is **vioser**, for
virtio-serial: named, 8-bit-clean byte channels between the guest and the host,
with no UART, network or disk involved.

The device side is IRIS's `feature/virtio-serial-guest-device-412469` branch;
its `docs/virtio-serial.md` describes what the driver talks to.

## Status

| Machine | Transport | Status |
|---|---|---|
| Indigo2 IMPACT (IP28) | virtio-mmio on GIO | Runs: tested end to end under IRIS |
| Indigo2 (IP22), Indy (IP24) | virtio-mmio on GIO | Compiles; never run |
| Octane (IP30) | virtio-pci on the Bridge | Compiles; never run |
| O2 (IP32) | | Not supported: IRIS does not wire the device there |

## Using it

Inside IRIX, with MIPSpro and the kernel headers installed:

```sh
cd driver
smake            # builds /tmp/vioser.<board>/mod/vioser.o
smake load       # ml ld
smake unload     # ml unld
smake install    # load at every boot; smake uninstall undoes it
```

Each port is a character device:

```
/hw/virtio-serial/port0            every port, by number
/hw/virtio-ports/org.iris.agent.0  named ports, by name
/dev/virtio-ports                  a link to the above, after smake install
```

`read`, `write`, `poll`, `FIONREAD` and `VIOSER_GETINFO` (see
[driver/vioser.h](driver/vioser.h)) work as you would expect of a pipe. One
program can have a port open at a time. `read` returns end of file while no
program is connected to the host end, and what the guest writes then is dropped
by the device. A port is not a tty, so `getty` cannot run on one.

In `iris.toml`:

```toml
[virtio_serial]
enabled = true
slot    = "exp0"                  # see below, on Indigo2

[[virtio_serial.port]]
console  = true
tcp_port = 8890

[[virtio_serial.port]]
name   = "org.iris.agent.0"
socket = "/tmp/iris-agent.sock"
```

### Use `slot = "exp0"` on an Indigo2

IRIS defaults to `exp1`, but an Indigo2 kernel dispatches no interrupts for
that slot: IP28's `ip22_gio1_intr` only tests the graphics slot and slot 0. The
driver detects this and polls every 10 ms, so `exp1` works, but `exp0` gets real
interrupts.

## The MIPSpro wrapper

[tools/mipspro-suppress](tools/mipspro-suppress) removes the "requires a license
password" banner from compiler output, and keeps everything else, including real
errors and the exit status. `sh tools/install-wrapper` installs it under
`/usr/local/mipspro/bin`, and `driver/Makefile` uses it from there.

It is a Bourne shell port of
[LaurentChardon/mipspro](https://github.com/LaurentChardon/mipspro), whose
script needs `/usr/freeware/bin/bash`. A stock IRIX install has no bash.

## Tests

See [CLAUDE.md](CLAUDE.md) for how to run them.

## Limits

- Ports are found once, when the driver loads. A port added or removed later is
  ignored with a warning.
- A restored IRIS snapshot leaves the device idle; reload the driver.
- `vioser` loads with `ml`. It has not been linked statically into a kernel.
