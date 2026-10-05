# VirtioTablet

A Mac OS X kernel extension that drives QEMU's virtio input device as a pointer, for PowerPC Mac OS X
10.0 to 10.3: `virtio-tablet-pci` as an absolute pointer, so the guest's cursor is wherever the
host's pointer is over the VNC viewer or QEMU window (no grab, no second cursor lagging behind),
and `virtio-mouse-pci` as a relative one. See `VirtioTablet.h` for the design.

QEMU's mac99 has only relative pointers for these releases: the ADB mouse (CUDA machines, 10.0 and
10.1) and the USB mouse (PMU machines, 10.2 and 10.3; `usb-tablet` needs a USB stack the CUDA
machines do not have). Over VNC a relative pointer means two cursors that drift apart.

## Host side

    -device virtio-tablet-pci

QEMU's VNC server, and its own windows, then send the pointer to the tablet as an absolute position
(`info mice` / `query-mice` lists it as absolute). The device is modern-only (1af4:1052): no
`disable-modern`. `virtio-mouse-pci` works the same way, relative; `virtio-keyboard-pci` has the
same ID and is left alone (logged as "is no pointing device").

QEMU 11.0.2 aborts (`Property 'qemu-fixed-text-console.device' not found`) on a QMP
`input-send-event` that names a `device`, at least with `-display none`; select the device with
the monitor's `mouse_set <index>` and send events without `device` instead.

## In the guest

The IOKit personality matches the device, so the kext loads at boot when it is in
`/System/Library/Extensions` (owned root:wheel; `touch /System/Library/Extensions` after copying it
there, and on 10.1 remove `/System/Library/Extensions.mkext`, which BootX prefers), or by hand with
`kextload`. It logs

    VirtioTablet: "QEMU Virtio Tablet", absolute (0..32767 x 0..32767), with a wheel
    VirtioTablet: 64 event slots, interrupt-driven

IOHIDSystem picks it up like any other pointing device: the cursor follows from the login window on.

## What it does and does not do

- Absolute X/Y go to IOHIDSystem with the axes' range as bounds, and IOHIDSystem scales them to the
  screen (the cursor's tip lands within 1 to 2 pixels of the VNC client's pointer at 1024x768: the
  arrow's tip is two pixels off its hot spot). Ranges wider than 16 bits lose their low bits.
- Buttons: left (and `BTN_TOUCH`) is the primary, right the secondary, middle the third, side and
  extra the fourth and fifth. 10.0 to 10.2 make every button past the first the secondary (their
  IOHIPointing does); 10.3 passes the middle on. The primary is also reported as full pressure, as
  a tablet's tip would be.
- The wheel (`REL_WHEEL`, `REL_HWHEEL`, and the `BTN_GEAR_UP`/`DOWN` that QEMU before 4.0 sent)
  goes to `dispatchScrollWheelEvent`; scroll-wheel acceleration is the system's.
- Relative motion (virtio-mouse-pci) goes through IOHIPointing's acceleration, at 100 dpi, as an
  ADB mouse.
- Events are taken a report at a time (up to `EV_SYN`); `SYN_DROPPED` starts the next report afresh.
  Every event slot is given straight back, so QEMU, which holds a report until it has slots for all
  of it, never waits.
- Interrupt-driven where the platform routes the device's interrupt (all four releases did); polls
  every 10 ms where it does not, or when events wait two watchdog seconds with no interrupt.
  `Polled` set to `true` in the personality forces polling.
- Unloads (`kextunload`) and reloads; the cursor stops with it and follows again after.
- Not done: the status queue (LEDs; a pointer has none), multitouch (`virtio-multitouch-pci`),
  keyboards.

## Where IOHIPointing lives

The driver derives from `IOHIPointing`, and that class is somewhere else in every release (`nm` of
the binaries): 10.0's kernel, 10.1's `com.apple.iokit.IOHIDSystem` kext, 10.2's and 10.3's
`com.apple.iokit.IOHIDFamily`. The kext declares `com.apple.iokit.IOHIDFamily` 1.0.0b1, the one
bundle all four have, which leads to IOHIPointing where it is not its home: 10.1's IOHIDFamily
depends on IOHIDSystem, 10.0's on the kernel. `com.apple.iokit.IOHIDSystem` itself cannot be
declared: 10.0 has no such bundle.

10.1's IOHIPointing carries a `_reserved` pointer at its end that 10.0's header does not show; the
`gcc2` build, compiled against 10.0's headers, keeps a pad there so the driver's own fields stay
clear of it. 10.2 and 10.3 keep the class's size and grow through that pointer.

## Why it is built twice, and in the guests

As VirtioConsole (`../virtioconsole/README.md`, which also says where the toolchains come from):
the kernel's C++ ABI differs between GCC 2.95 (10.0, 10.1) and GCC 3 (10.2 on).

| Build | For | Built on | Headers and kmod libraries |
| --- | --- | --- | --- |
| `gcc2` | 10.0, 10.1 | 10.0 (or 10.1, with 10.0's headers for both) | 10.0's; 10.1's own for 10.1 alone |
| `gcc3` | 10.2, 10.3 | 10.2 or 10.3 | the guest's own |

    make ABI=gcc2      # on 10.0; on 10.1 add KERNEL_HEADERS=/tmp/k100/Headers KMOD_LIB_DIR=/tmp/k100/lib
    make ABI=gcc3      # on 10.2 or 10.3

Each leaves `build/<ABI>/VirtioTablet.kext`. Both build without a warning. The ladder
(`ladder/ladder.sh <release> build`) builds it in each release's guest and packages it
(`packaging/components/virtiotablet.json`).

## Testing

`ladder/tablet_test.py` runs on the host against the guest's QEMU VNC server (no authentication),
with the kext loaded:

    QEMU_EXTRA="-device virtio-tablet-pci" ladder/ladder.sh 10.2 start
    python3 ladder/tablet_test.py --login 520,432,PASSWORD127.0.0.1:5912

It moves the pointer to five places and finds the cursor in the framebuffer (the pixels that
changed around the target), and requires its tip within 3 pixels; then clicks the Apple menu open
and closes it. `--login X,Y,PASSWORD` first clicks the user in the login window's list (10.1 to
10.3) and logs in; `--login-fields NX,NY,PX,PY,USER,PASSWORD` does it with 10.0's name and password
fields; `--no-menu` stops after the moves. Without the driver the cursor does not move at all
("nothing changed there"). The ladder's guests' users are at 534,403/534,428 (10.0), 540,431
(10.1), 520,432 (10.2) and 547,425 (10.3), the guests' own password.

## Evidence

The local ladder's guests (QEMU 11.0.2, TCG, `mac99`, 1024x768) with `-device virtio-tablet-pci`,
2026-10-05. Loaded by hand first:

- 10.2.8 (`gcc3`, GCC 3.1): `tablet_test.py` all passed (five moves within 2 pixels, the Apple menu
  opened and closed); a click on the login window's user opened the password prompt; the wheel
  scrolled the Finder's Applications window down; after `kextunload` the cursor was dead (all five
  moves "nothing changed"), and after `kextload` all passed again.
- 10.3.9 (the same `gcc3` binary): the five moves; clicks on the login window (the user, then the
  alert's OK) took; `kextunload` and reload. The ladder's 10.3 guest cannot log in ("You cannot
  continue logging in at this time": its NIC is `en1`, and MCX finds no machine address), so there
  is no menu bar to test there.
- 10.0.4 (`gcc2`, GCC 2.95.2 gcc-926): the five moves; clicks into the login window's fields and the
  login; the Apple menu; a double-click opened TextEdit; dragging the grow box resized a window; the
  wheel scrolled TextEdit (10.0's Finder does not scroll by wheel at all); `kextunload` and reload.
- 10.1.5 (the `gcc2` binary built on 10.0.4): `tablet_test.py --login` all passed; the wheel
  scrolled the Finder's Applications window; `kextunload` and reload. With `virtio-keyboard-pci`
  attached too, the keyboard was declined; with `virtio-mouse-pci` (made current with `mouse_set`)
  QMP's relative motion moved the cursor (at 400 dpi then; now 100).

Then from the ladder (`ladder.sh <release> build`, each release building its own: gcc-926 on 10.0,
gcc-934.3 with 10.1's own headers on 10.1, GCC 3.1 on 10.2, GCC 3.3 on 10.3; no warnings), the
package installed (`installer -pkg` on 10.2 and 10.3, `packaging/installer/install-pkg.sh` on 10.0
and 10.1) and the guest restarted with the tablet: the kext loaded at boot on all four, and
`tablet_test.py` all passed on 10.0.4 (`--login-fields`), 10.1.5 and 10.2.8 (`--login`), and on
10.3.9 with `--no-menu`.

## References

- The virtio 1.x specification: "Virtio Over PCI Bus" and "Input Device".
- QEMU's `hw/input/virtio-input-hid.c` (what the tablet, mouse and keyboard send) and
  `hw/input/virtio-input.c` (a report is held until slots for all of it are queued).
- Apple's xnu-123.5 `iokit/Families/IOHIDSystem` (10.0), IOHIDFamily-34.12 (10.2) and -86.26
  (10.3): `IOHIPointing::dispatchAbsolutePointerEvent` and `IOHIDSystem::_scaleLocationToCurrentScreen`.
- `../virtiovsock` here, whose modern virtio-pci transport this reuses.
