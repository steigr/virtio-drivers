# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

IRIX 6.5.22 kernel drivers for virtio devices, built towards the IRIS emulator's device models (sibling checkout `../iris`, branch `feature/virtio-9p`, on top of `feature/virtio-serial-guest-device-412469`; its `docs/virtio-serial.md`, `docs/virtio-9p.md` and `src/dev/virtio/` are the contract). `README.md` covers status and use.

## Commands

Host (no IRIX needed):

```bash
make -C tests/host                     # virtqueue and 9P unit tests, with ASan and UBSan
```

Inside IRIX, from `driver/` (the guest's `/share` is this repository when booted as below). The Makefile needs `smake`, and IRIX has no `sed -i`, `grep -A`, or bash:

```sh
smake                     # /tmp/vioser.<board>/mod/{vioser,p9fs}.o, mount_9p; board from uname -m
smake CPUBOARD=IP22       # compile-check another board: IP22 (n32), IP30 (n64)
smake load ; smake unload
smake load9p ; smake unload9p
```

End to end, against the installed system in `$HOME/Developer/irix-ip28`:

```bash
# Build iris from the branch, outside its checkout.
git -C ../iris archive origin/feature/virtio-9p | tar -x -C "$SRC"
(cd "$SRC" && CARGO_TARGET_DIR="$TGT" cargo build --release --features lightning,idle-pause --bin iris)

IRIS="$TGT/release/iris" VIRTIO_SLOT=exp0 tests/iris/boot-ip28.sh "$WORK"   # blocks; run in the background
python3 ../iris/tools/irix-sh.py --port 12323 wait --timeout 480
tests/iris/run_tests.py "$WORK" --bytes 16777216                            # builds, loads, tests, unloads
python3 ../iris/tools/irix-sh.py --port 12323 run -k 'sync; shutdown -p -g0 -y'
```

The filesystem's tests want a guest with a share, and an empty directory for it. `VIRTIO_SLOT` is vioser's, so the 9p device gets the other slot: `exp1` here gives 9p the slot with an interrupt.

```bash
IRIS="$TGT/release/iris" VIRTIO_SLOT=exp1 P9_SHARE="$DIR" tests/iris/boot-ip28.sh "$WORK"   # blocks
python3 ../iris/tools/irix-sh.py --port 12323 wait --timeout 480
tests/iris/run_9p_tests.py "$DIR" --mb 64                                   # builds, loads, tests, unloads; --quick skips the build on the share
# Read-only: boot with P9_READONLY=1 as well, then tests/iris/run_9p_tests.py "$DIR" --readonly
```

The inst packages, inside IRIX (`.forgejo/workflows/package.yml` runs both, then uploads the results with `packaging/publish-package.sh`):

```sh
sh /share/packaging/build-package.sh 2026110201 /var/tmp/out   # vioser- and p9fs-2026110201.tardist; /tmp empties at boot
sh /share/packaging/test-package.sh p9fs /var/tmp/out/p9fs-2026110201.tardist   # inst install, check, remove; loads nothing
```

There is no way to run a single end-to-end test: call the functions in `run_tests.py` and `run_9p_tests.py`, or run `/tmp/vstest` and `/tmp/p9test` by hand (`tests/guest/vstest.c` and `p9test.c` list their modes). A change to the interrupt or polling path needs a run on both `VIRTIO_SLOT=exp0` and `exp1`, since they take different paths. A leftover mount or loaded module from an earlier run spoils the next one: each device can have one driver attached, and loading a second resets it under the first.

Writes go to `irix65.raw.overlay` beside the instance's image, never to the image. Delete the overlay after a crash or kill: a killed guest leaves XFS dirty.

## Architecture

`driver/` builds into two loadable modules by `ld -r`, `vioser.o` and `p9fs.o`, which share the layers underneath. A driver knows its device, and each layer is ignorant of the one above:

- `virtq.c`: split rings. No kernel dependencies, so `tests/host` runs it; cache maintenance arrives as `vq_sync_ops`.
- `virtio_gio.c` (IP22, IP28: virtio-mmio) and `virtio_pci.c` (IP30: virtio-pci): each fills in `virtio_bus_ops` and is compiled out on other boards. Only the board's `#ifdef`ed transport supplies `virtio_bus_init`, which finds the device a `virtio_driver` names.
- `vioser.c`: the virtio-serial core and character device (ports, control queue, read/write/poll). Touches no registers.
- `p9.c`: the 9P2000.L client, messages only. No kernel dependencies, so `tests/host` runs it too; the round trip arrives as `p9_transport`.
- `p9fs_dev.c`: the virtio-9p device and that round trip: one queue, a pool of requests made at attach.
- `p9fs_vfs.c` (mount, statvfs, sync, the module) and `p9fs_vnops.c` (vnode operations): the filesystem.

Port discovery polls the control queue during attach, before the interrupt is connected. After that the interrupt path only consumes control messages and never sends, so `vs_ctl_send` is process context only.

File data goes through the buffer cache as the IRIX NFS client's does (`chunkread`, `getchunk`, `biomove`, `VOP_BMAP`, `VOP_STRATEGY`), which is what makes `mmap` and exec work with no code of their own: `VOP_MAP` is `fs_noerr`, as NFS's is. Strategy always finishes before returning. There is one node per qid, and a node's walked fid is a *name*: every lookup that reaches the file swaps in the fid that just worked.

## Non-obvious constraints

- **GIO DMA is not cache coherent, and rings must stay cached.** IP28's ECC memory does not take uncached partial writes outside "slow mode", so K1 rings are out. Everything is `KM_CACHEALIGN`, written back before the device reads and invalidated both when handed over and when returned (R10000 speculation can reload a line in between). IP28 modules also need `-TARG:t5_no_spec_stores`, which `/var/sysgen/Makefile.kernloadio` supplies.
- **A level interrupt with no handler wedges the guest.** Its IP stays pending and nothing clears it, with no console message. So rings set `VRING_AVAIL_F_NO_INTERRUPT` until a handler is connected. A hung `ml ld` is probably this: read `regs` from the monitor (`IRIS_MON_PORT=18888 ../iris/tools/mon.py regs`) and look for a stuck IP bit in Cause.
- **Indigo2 slot 1 has no interrupt.** `ip22_gio1_intr` tests only the graphics slot and slot 0, so `vioser_gio.c` returns `VIOSER_NO_INTR` there and the core polls from `itimeout`. Use `slot = "exp0"`; IRIS's default is `exp1`.
- **Everything the device reads is little-endian**, on a big-endian CPU. `virtq.c` stores ring fields a byte at a time. virtio-mmio registers are the exception: they read as plain numbers with `lw`, and config space is read a byte at a time.
- **The MIPSpro in the guest is C89**: declarations at the top of blocks, no `<stdint.h>`, `__uint16_t` undefined in the kernel. DDI signatures are IRIX's, not the SVR4 ones in old docs: `SV_WAIT_SIG(sv, lock, s)`, `SV_BROADCAST(sv)`, `MUTEX_ALLOC(type, flags, name)`. Check `/usr/include/sys` rather than trusting memory.
- **`tools/irix-sh.py` hangs silently on a command line over 256 characters** (IRIX's tty limit). Push a script and run that. Its `wait` returns when telnet answers, before rc2 finishes: wait for `console login` in the console log if rc scripts matter. `reboot` over it does not take effect; power off with `shutdown -p` and start iris again.
- **`boot-ip28.sh`'s ports are fixed, and iris starts anyway if they are taken.** Port 12323 may then reach another session's guest, so check `lsof -iTCP:12323 -sTCP:LISTEN` before booting and `ls /share` before running anything.
- **The package's modules must stay unstripped**: `ml` needs their symbols and relocations, so the idb marks them `nostrip`, and `mach(CPUBOARD=...)` picks each board's copy for one install path.
- **The filesystem's kernel interfaces are undocumented.** They were read from `/usr/include/sys` and from disassembling `/var/sysgen/boot/nfs3.a` with `dis` and `elfdump -r`: its `nfs3_vnodeops` table says which `fs_*` a third-party filesystem should use in each slot, and its `nfs3_bmap`, `nfs3_strategy` and `rw3vp` are what `p9fs_bmap`, `p9fs_strategy` and `p9fs_rw` follow. Do the same before guessing at a VFS call.
- **A vnode-page call wants the rwlock held exclusively and the range's last byte, inclusive**: `VOP_FLUSHINVAL_PAGES(vp, 0, FILE_LAST, FI_REMAPF_LOCKED)`, so it is never made from a VOP_READ holding it shared. `VOP_*` macros are brace blocks, so an `if` around one needs braces before an `else`.
- **`VOP_RECLAIM` must leave no page or buffer on the vnode.** `vn_reclaim` otherwise calls `VOP_FLUSHINVAL_PAGES` after the behavior has gone, and the kernel faults at address 0x10. Clean buffers count, and `VN_CACHED` does not cover them, so `p9fs_reclaim` always flushes.
- **`VOP_CREATE`'s `*vpp` comes in non-null when the kernel's lookup found the file**, with a hold the filesystem must drop, as `nfs3_create` does. Miss it and the vnode never goes inactive: the first sign is `umount` saying busy with nothing using it.
- **`pn_` names clash with `sys/pathname.h`**: `pn_free` is taken, so check before adding one.
- **The guest's `/share` mounts late**, and is `noexec` for practical purposes: `mount /share` if it is empty, and build into `/tmp`.

## What has run

Only IP28 under IRIS has run, both drivers. IP22, IP24 and IP30 are compile-tested, so say so rather than implying otherwise. O2 is out of scope until IRIS wires the device there.
