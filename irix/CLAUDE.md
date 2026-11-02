# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

IRIX 6.5.22 kernel drivers for virtio devices, built towards the IRIS emulator's device models (sibling checkout `../iris`, branch `feature/virtio-serial-guest-device-412469`; its `docs/virtio-serial.md` and `src/dev/virtio/` are the contract). `README.md` covers status and use.

## Commands

Host (no IRIX needed):

```bash
make -C tests/host                     # virtqueue unit test, with ASan and UBSan
```

Inside IRIX, from `driver/` (the guest's `/share` is this repository when booted as below). The Makefile needs `smake`, and IRIX has no `sed -i`, `grep -A`, or bash:

```sh
smake                     # /tmp/vioser.<board>/mod/vioser.o; board from uname -m
smake CPUBOARD=IP22       # compile-check another board: IP22 (n32), IP30 (n64)
smake load ; smake unload
```

End to end, against the installed system in `$HOME/Developer/irix-ip28`:

```bash
# Build iris from the branch, outside its checkout.
git -C ../iris archive origin/feature/virtio-serial-guest-device-412469 | tar -x -C "$SRC"
(cd "$SRC" && CARGO_TARGET_DIR="$TGT" cargo build --release --features lightning,idle-pause --bin iris)

IRIS="$TGT/release/iris" VIRTIO_SLOT=exp0 tests/iris/boot-ip28.sh "$WORK"   # blocks; run in the background
python3 ../iris/tools/irix-sh.py --port 12323 wait --timeout 480
tests/iris/run_tests.py "$WORK" --bytes 16777216                            # builds, loads, tests, unloads
python3 ../iris/tools/irix-sh.py --port 12323 run -k 'sync; shutdown -p -g0 -y'
```

The inst package, inside IRIX (`.forgejo/workflows/package.yml` runs both, then uploads the result with `packaging/publish-package.sh`):

```sh
sh /share/packaging/build-package.sh 2026110201 /var/tmp/out   # vioser-2026110201.tardist; /tmp empties at boot
sh /share/packaging/test-package.sh /var/tmp/out/vioser-2026110201.tardist   # inst install, check, remove; loads nothing
```

There is no way to run a single end-to-end test: call the functions in `run_tests.py`, or run `/tmp/vstest` by hand (`tests/guest/vstest.c` lists its modes). A change to the interrupt or polling path needs a run on both `VIRTIO_SLOT=exp0` and `exp1`, since they take different paths.

Writes go to `irix65.raw.overlay` beside the instance's image, never to the image. Delete the overlay after a crash or kill: a killed guest leaves XFS dirty.

## Architecture

`driver/` builds into one loadable module, `vioser.o`, by `ld -r`. There are three layers, each ignorant of the one above:

- `virtq.c`: single-descriptor split rings. No kernel dependencies, so `tests/host` runs it; cache maintenance arrives as `vq_sync_ops`.
- `vioser.c`: the virtio-serial core and character device (ports, control queue, read/write/poll). Touches no registers.
- `vioser_gio.c` (IP22, IP28: virtio-mmio) and `vioser_pci.c` (IP30: virtio-pci): each fills in `vioser_bus_ops` and is compiled out on other boards. Only the board's `#ifdef`ed transport supplies `vioser_bus_init`.

Port discovery polls the control queue during attach, before the interrupt is connected. After that the interrupt path only consumes control messages and never sends, so `vs_ctl_send` is process context only.

## Non-obvious constraints

- **GIO DMA is not cache coherent, and rings must stay cached.** IP28's ECC memory does not take uncached partial writes outside "slow mode", so K1 rings are out. Everything is `KM_CACHEALIGN`, written back before the device reads and invalidated both when handed over and when returned (R10000 speculation can reload a line in between). IP28 modules also need `-TARG:t5_no_spec_stores`, which `/var/sysgen/Makefile.kernloadio` supplies.
- **A level interrupt with no handler wedges the guest.** Its IP stays pending and nothing clears it, with no console message. So rings set `VRING_AVAIL_F_NO_INTERRUPT` until a handler is connected. A hung `ml ld` is probably this: read `regs` from the monitor (`IRIS_MON_PORT=18888 ../iris/tools/mon.py regs`) and look for a stuck IP bit in Cause.
- **Indigo2 slot 1 has no interrupt.** `ip22_gio1_intr` tests only the graphics slot and slot 0, so `vioser_gio.c` returns `VIOSER_NO_INTR` there and the core polls from `itimeout`. Use `slot = "exp0"`; IRIS's default is `exp1`.
- **Everything the device reads is little-endian**, on a big-endian CPU. `virtq.c` stores ring fields a byte at a time. virtio-mmio registers are the exception: they read as plain numbers with `lw`, and config space is read a byte at a time.
- **The MIPSpro in the guest is C89**: declarations at the top of blocks, no `<stdint.h>`, `__uint16_t` undefined in the kernel. DDI signatures are IRIX's, not the SVR4 ones in old docs: `SV_WAIT_SIG(sv, lock, s)`, `SV_BROADCAST(sv)`, `MUTEX_ALLOC(type, flags, name)`. Check `/usr/include/sys` rather than trusting memory.
- **`tools/irix-sh.py` hangs silently on a command line over 256 characters** (IRIX's tty limit). Push a script and run that. Its `wait` returns when telnet answers, before rc2 finishes: wait for `console login` in the console log if rc scripts matter. `reboot` over it does not take effect; power off with `shutdown -p` and start iris again.
- **`boot-ip28.sh`'s ports are fixed, and iris starts anyway if they are taken.** Port 12323 may then reach another session's guest, so check `lsof -iTCP:12323 -sTCP:LISTEN` before booting and `ls /share` before running anything.
- **The package's `vioser.o` must stay unstripped**: `ml` needs its symbols and relocations, so the idb marks it `nostrip`, and `mach(CPUBOARD=...)` picks each board's copy for one install path.
- **The guest's `/share` mounts late**, and is `noexec` for practical purposes: `mount /share` if it is empty, and build into `/tmp`.

## What has run

Only IP28 under IRIS has run. IP22, IP24 and IP30 are compile-tested, so say so rather than implying otherwise. O2 is out of scope until IRIS wires the device there.
