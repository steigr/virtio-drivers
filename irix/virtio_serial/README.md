# virtio_serial for IRIX

`virtio_serial.o`, a loadable driver for virtio-serial (virtio console device, multiport): named,
8-bit-clean byte channels between the guest and the host, with no UART, network or disk involved.
It carries the IRIX guest agent's channel. Formerly `vioser`.

**Maturity:** **Beta.** Runs end to end on the IP28 and the IP35 under IRIS; compiles for IP22, IP24
and IP30, which have never run it.

## Build

Inside IRIX 6.5 with MIPSpro, `smake` and the kernel build environment
(`/var/sysgen/Makefile.kernloadio`):

```sh
smake            # /tmp/virtio_serial.<board>/mod/virtio_serial.o; CPUBOARD=IP30 for another board
smake load       # ml ld; smake unload undoes it
smake install    # load it at every boot; smake uninstall undoes it
```

Or install the package: `inst -f virtio_serial-latest.tardist`, then `/etc/init.d/virtio_serial start`.
Its `dev` image, not selected by default, adds `sys/virtio_serial.h`.

## Use

Each port is a character device:

```
/hw/virtio-serial/port0            every port, by number
/hw/virtio-ports/org.iris.agent.0  named ports, by name
/dev/virtio-ports                  a link to the above, made by the init script
```

`read`, `write`, `poll`, `FIONREAD` and `VIRTIO_SERIAL_GETINFO` ([virtio_serial.h](virtio_serial.h))
work as on a pipe. One program can have a port open at a time. `read` returns end of file while
nothing is connected to the host end, and what the guest writes then is dropped. A port is not a
tty, so `getty` cannot run on one.

## Host side (IRIS)

```toml
[virtio_serial]
enabled = true
slot    = "exp0"                  # on an Indigo2, see ../README.md

[[virtio_serial.port]]
name   = "org.iris.agent.0"
socket = "/tmp/iris-agent.sock"
```

## Testing

`tests/run_tests.py` drives a guest booted by `../tests/boot-ip28.sh` and plays the host ends of
the ports ([../CLAUDE.md](../CLAUDE.md)). Ports are found once, when the driver loads: one added or
removed later is ignored with a warning.

## CI

[`.forgejo/workflows/irix-virtio_serial.yml`](../../.forgejo/workflows/irix-virtio_serial.yml) builds the
package for IP22, IP28, IP30 and IP35 in the `irix` runner's guest, installs it with inst, and
publishes it on each push to `main` that touches this driver or `../virtio`.
