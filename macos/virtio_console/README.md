# VirtioConsole

A Mac OS X kernel extension that presents QEMU's legacy virtio console as a serial port,
`/dev/cu.virtio-0`, so that the PowerPC releases' guest agent has a channel that needs neither
the guest's network nor USB. 10.0 has no USB QEMU can give it, and FTDI's driver, the other serial
channel, cannot load on 10.0 or 10.1. See `VirtioConsole.h` for the design.

The host side is `machine.AgentVirtioConsole`: `-device virtio-serial-pci,disable-modern=on,max_ports=1
-device virtconsole,chardev=qga0,nr=0`. The installer puts the kext on the target when the
deployment supplies it (`macos run --virtio-console`, chart value `virtioConsole.image`), and the
agent's startup item serves it with `--tty auto`.

## Why it is built in the guests

A kext links against the kernel's C++ classes, and the compiler that built the kernel decides how
they are laid out and named. 10.0 and 10.1 were built with GCC 2.95, 10.2 and later with GCC 3, and
the two do not agree -- FTDI's driver, built with GCC 3, imports 370 symbols 10.0 and 10.1 do not
have. So there are two builds of this one source, each made with Apple's own Developer Tools for
its generation, which run only on PowerPC Mac OS X:

| Build | For | Built on | Compiler | Headers and kmod libraries |
| --- | --- | --- | --- | --- |
| `gcc2` | 10.0, 10.1 | a 10.1 guest | December 2001 tools (GCC 2.95.2, gcc-934.3) | 10.0's, from the March 2001 tools |
| `gcc3` | 10.2 and later | a 10.2 guest | December 2002 tools (GCC 3.1, gcc-1175) | the December 2002 tools' own |

The `gcc2` build must see 10.0's Kernel.framework, not the 10.1 guest's own: against 10.1's headers
the kext imports eight IOService and IORegistryEntry virtuals 10.1 added (`copyProperty` three
times, `copyChildEntry`, `copyParentEntry`, `requestTerminate`, `willTerminate`, `didTerminate`),
and 10.0 refuses to link it. Built against 10.0's, all 376 of its imports are in 10.0's kernel and
IOSerialFamily, and the one binary loads on both.

The toolchains, from the Internet Archive:

- `691-3451-A,Mac OS X Developer Tools. v2001-12 (CD).toast`, 313,602,048 bytes, MD5
  `06cc2c1a15a9c181618f2c964e8dc4a1` (item `691-3451-AMac_OS_X_Developer_Tools._v2001-12_CD`).
  `DevTools.pkg` and `DevSDK.pkg`; 10.1 has no `installer` command, so their `.pax.gz` go in with
  `cd / && /System/Library/PrivateFrameworks/Installation.framework/Versions/A/Resources/pax -r -z
  -p e -f <package>.pax.gz`.
- `Mac OS X Developer Tools 691-2963-A.cdr`, 246,304,768 bytes, MD5
  `b148afc86e50a5ce4015851a8ca5a416` (item `cdrom_Mac_OS_X_Developer_Tools_691-2963-A`): the 10.0
  (4K78) tools, one `Developer.pkg`. Only its `Kernel.framework/Versions/A/Headers`,
  `/usr/lib/libkmod.a`, `/usr/lib/libkmodc++.a` and `/usr/lib/gcc/darwin/2.95.2/libcc_kext.a` are
  used; copied into a guest, the archives need `ranlib` again.
- `devtooldec02.zip`, 314,755,810 bytes, MD5 `badfcc5a65ca6043b6953c8f9a3c20a3` (item
  `macosx-devtools`). `BSDSDK.pkg`, `DevSDK.pkg` and `DevTools.pkg`, with 10.2's `installer -pkg`.

## Building

In a guest with its generation's Developer Tools installed, with this directory copied in:

    make ABI=gcc2 KERNEL_HEADERS=/tmp/k100/Headers KMOD_LIB_DIR=/tmp/k100/lib   # on 10.1
    make ABI=gcc3                                                                # on 10.2

Each leaves `build/<ABI>/VirtioConsole.kext`. On the host, with both builds copied back into
`build/`:

    (cd build && COPYFILE_DISABLE=1 tar --format ustar --no-xattrs -czf VirtioConsole.tar.gz gcc2/VirtioConsole.kext gcc3/VirtioConsole.kext)
    make virtio-console    # at the repository's top: the image the chart mounts

The flags are those of Project Builder's kernel-extension rules
(`/Developer/Makefiles/pbx_jamfiles/ProjectBuilderJambase`), and each departure from them was a
failure first; the Makefile's comments carry the evidence:

- GCC 2.95 needs `-fvtable-thunks -findirect-virtual-calls`, GCC 3 needs `-fapple-kext` (C++ only;
  GCC 3.1's C compiler refuses it). Without it 10.2 refuses the GCC 3 build: "is not compatible
  with its superclass, IOSerialDriverSync superclass changed?".
- The objects go before `-lkmodc++ -lkmod -lcc_kext`. With `-lkmodc++` first the linker takes
  libkmod's `__start`, which runs no constructors, and IOKit matches the device but cannot make the
  driver: "Couldn't alloc class io_virtio_VirtioConsole".
- The GCC 3 build asks for the kernel's libraries at 6.0 (`libraries-gcc3.xml`), so that 10.0 and
  10.1 refuse it outright instead of failing to link it.

Proven on 2026-09-29, each by loading the kext, serving the agent on `/dev/cu.virtio-0` and
round-tripping 8 KiB of every byte value from the host: `gcc2` on 10.0 (4K78) and 10.1 (5G64),
`gcc3` on 10.2 (6C115) and 10.3 (7B85).

## Loading it by hand

    cp -R build/gcc2/VirtioConsole.kext /tmp/ && chown -R root:wheel /tmp/VirtioConsole.kext
    kextload -v /tmp/VirtioConsole.kext
    ls /dev/cu.virtio-*

10.1's `kextunload` has no `-b`; `kextunload -m io.virtio.driver.VirtioConsole`.

## References

- Apple's IOSerialFamily-6 and -7 (github.com/apple-oss-distributions/IOSerialFamily), the 10.0 and
  10.1 generation: `IOSerialDriverSync.h`, the interface this driver implements, and
  `IOSerialBSDClient.cpp`, which makes `/dev/cu.*` of it.
- Apple16X50Serial-10.5 (github.com/apple-oss-distributions/Apple16X50Serial), Apple sample-code
  licence: the structure of the port's gated entry points.
- The virtio 0.9.5 specification (legacy PCI) and Linux's `drivers/char/virtio_console.c`.
- elliotnunn/classicvirtio (MIT): virtio on QEMU's mac99 with a big-endian guest, for Mac OS 9.
