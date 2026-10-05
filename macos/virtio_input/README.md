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

QEMU sends the pointer to its *current* pointing device, the one the guest activated last
(`query-mice`, "current"), and VNC is absolute only while that one is. On the PMU machines (10.2,
10.3) mac99 always builds a USB mouse, which the guest activates at its first poll, and at boot it
can come after the tablet: then VNC is relative and the tablet hears nothing, whether or not a
`usb-tablet` is there too. The driver therefore re-asserts the device (a reset, then DRIVER_OK,
which QEMU answers by making it current) 5, 10, 20, 40, 80 and 160 seconds after it starts, until
the first event arrives ("no event yet; making the device QEMU's current pointer again" in the
log, `Reactivations` in the registry). By hand, the monitor's `mouse_set <index>` does the same. A
`usb-tablet` beside the virtio one is a second absolute device to race with and is better left out.

### Leaving the tablet no rival (10.2, 10.3)

mac99 with `via=pmu` has no ADB, and with USB on it builds `usb-kbd` and `usb-mouse` itself
(`hw/ppc/mac_newworld.c`), with no id and no switch for the mouse alone. The way to drop the mouse
is to turn the machine's USB off and add the controller and keyboard back:

    -machine mac99,via=pmu,usb=off
    -device pci-ohci,id=ohci -device usb-kbd,bus=ohci.0
    -device virtio-tablet-pci

The virtio tablet is then QEMU's only pointing device, and no race is possible. On 10.2.8 and
10.3.9 (2026-10-05) the guest booted, the USB keyboard typed the login and `tablet_test.py --login`
all passed. What else was tried, on the 10.3 command line, before boot (`query-mice`):

| Variant | Pointers | Race gone |
| --- | --- | --- |
| `via=pmu` (the orchestrator's 10.3 preset, with its `usb-tablet`) | virtio tablet, USB tablet, USB mouse | no |
| `usb=off` + own OHCI + `usb-kbd` + `usb-tablet` | virtio tablet, USB tablet | no: two absolute devices |
| **`usb=off` + own OHCI + `usb-kbd`** | **virtio tablet** | **yes** |
| `via=pmu-adb` | virtio tablet, USB tablet, ADB mouse | yes for the mouse (QEMU's ADB mouse never activates itself), but see below |
| `via=pmu`, the USB mouse `device_del`ed at runtime | virtio tablet, USB tablet | no, and it is a runtime step |

The cost is a relative pointer. A relative QMP `input-send-event` (no device named), what the
orchestrator's keep-awake sends every minute, goes to QEMU's first device that takes relative
motion: the USB mouse with `via=pmu` (it moved 10.3's cursor), the ADB mouse on 10.0 and 10.1 (it
moved theirs, the tablet current). With `usb=off` there is none and the nudge is dropped; with
`via=pmu-adb` 10.3 logged in with the ADB keyboard but did not move its cursor for the ADB mouse
either. A key press is activity too: on 10.0 a QMP `send-key ctrl` took IOHIDSystem's
`HIDIdleTime` from 23 s to 56 ms (10.1 and 10.2 publish no `HIDIdleTime`, and 10.3's did not
change at all, so it was not measurable there). 10.0 and 10.1 (CUDA) need none of this: ADB has no
race and keeps its mouse for the nudges.

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
- Re-asserts the device until its first event, so that QEMU sends the pointer here (see "Host
  side").
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
  alert's OK) took; `kextunload` and reload. The ladder's 10.3 guest could not log in then ("You
  cannot continue logging in at this time": its only NIC, not built in, was `en1`, and MCX found no
  machine address); renamed `en0` in `NetworkInterfaces.plist`, it logs in.
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

A later boot of 10.3.9 lost the race to the USB mouse: the driver had 0 reports, QMP's own
absolute events reached it (the cursor landed on them), the VNC client's moved nothing, and
`mouse_set` to the tablet made `tablet_test.py` pass. With the re-assertion, all four rebuilt by the
ladder, installed from their packages and booted with the orchestrator's device sets (on 10.3 its
`usb-tablet` too): `query-mice` had the virtio tablet current on each, the driver re-asserted it
four or five times before the first event, and `tablet_test.py` all passed on all four, logging in
through the tablet (10.0 `--login-fields`, the others `--login`). On 10.3.9 the wheel then scrolled
the Finder's Applications window.

## References

- The virtio 1.x specification: "Virtio Over PCI Bus" and "Input Device".
- QEMU's `hw/input/virtio-input-hid.c` (what the tablet, mouse and keyboard send) and
  `hw/input/virtio-input.c` (a report is held until slots for all of it are queued).
- Apple's xnu-123.5 `iokit/Families/IOHIDSystem` (10.0), IOHIDFamily-34.12 (10.2) and -86.26
  (10.3): `IOHIPointing::dispatchAbsolutePointerEvent` and `IOHIDSystem::_scaleLocationToCurrentScreen`.
- `../virtiovsock` here, whose modern virtio-pci transport this reuses.
