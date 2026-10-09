# Virtio9P

A Mac OS X kernel extension that mounts a QEMU virtio 9P share (VirtFS): a host directory as a
filesystem in the guest, for PowerPC Mac OS X 10.0 to 10.3. One kext holds two things:

- an IOKit driver (`Virtio9P.cpp`) for QEMU's legacy virtio 9P transport, `virtio-9p-pci`
  (1af4:1009), which reads the device's mount tag and carries 9P messages over its one virtqueue;
- a filesystem, `9p` (`p9fs_*.c`), a 9P2000.L client for the BSD VFS these releases have before
  10.4's KPI, mounted with `mount_9p`.

See `p9fs.h` for the layers and their locking, and `Virtio9P.h` for the device side.

## Host side

    -fsdev local,id=p9,path=/path/to/share,security_model=none
    -device virtio-9p-pci,disable-modern=on,fsdev=p9,mount_tag=host

`disable-modern=on` as for VirtioConsole; a transitional device (without it) has the same legacy
I/O BAR. Any `security_model` works; with `none` the guest sees the host's owners and modes and
QEMU's own permissions decide.

## In the guest

The IOKit personality matches the device, so the kext loads at boot when it is in
`/System/Library/Extensions`, and loading it adds the `9p` filesystem:

    mount_9p host /Volumes/host                  # or: mount -t 9p host /Volumes/host
    mount_9p -o ro,nosuid -u 501 -g 20 host /mnt # read-only, every file shown as 501:20

Options (`-o`): `ro`, `rw`, `nosuid`, `nodev`, `noexec`, `uid=`, `gid=`, `aname=` (a subtree of
the export), `uname=` (the 9P user, default `root`, numeric id the caller's). A tag is mounted
once at a time.

## What it does and does not do

- Files, directories, symbolic links and hard links: lookup, create, read, write, truncate,
  remove, rename, mkdir, rmdir, symlink, readlink, link, chmod, chown, utimes, statfs, fsync.
- `mmap` and `exec` work: mapped files have pages through the UBC (VOP_PAGEIN/PAGEOUT).
- `read()` and `write()` go to the server every time, so changes made on the host are seen at
  once; attributes are cached for up to two seconds, and a file's mapped pages are dropped when
  it is opened after the host changed it (close-to-open, as NFS).
- Advisory locks (`flock`, `fcntl`) are granted without locking anything, as on an NFS mount
  with `nolock`: these kernels have no lock manager a kext can use, and QEMU's `Tlock` is a stub.
- `kextunload` is refused while a share is mounted, by both halves: the driver will not be
  terminated and the module will not stop.
- Not supported: device nodes, FIFOs and sockets on the share (they are listed but cannot be
  opened, nor made), file flags (`chflags`), `getattrlist` (Carbon falls back), NFS export.
- Directory offsets are entry indices, not QEMU's 64-bit cookies, so `telldir`/`seekdir` work.
- Polled: a request is waited for with a short spin, then in 1 ms sleeps; no interrupt is used.

## Why it is built twice, and in the guests

As VirtioConsole (`../ppc/virtioconsole/README.md`, which also says where the toolchains come from):
the kernel's C++ ABI differs between GCC 2.95 (10.0, 10.1) and GCC 3 (10.2 on).

| Build | For | Built on | Headers and kmod libraries |
| --- | --- | --- | --- |
| `gcc2` | 10.0, 10.1 | a 10.1 guest, December 2001 tools | 10.0's, from the March 2001 tools |
| `gcc3` | 10.2, 10.3 | a 10.2 guest, December 2002 tools | the December 2002 tools' own |

    make ABI=gcc2 KERNEL_HEADERS=/tmp/k100/Headers KMOD_LIB_DIR=/tmp/k100/lib   # on 10.1
    make ABI=gcc3                                                                # on 10.2

Each leaves `build/<ABI>/Virtio9P.kext` and `build/<ABI>/mount_9p`. What differs from
VirtioConsole's build, and why:

- The kmod has its own start and stop routines (`Virtio9P_info.c`) instead of libkmodc++'s, so
  that unloading is refused while a share is mounted before the C++ runtime is torn down, not
  after.
- The `gcc2` build declares no `com.apple.kernel.bsd`: 10.0's is 1.0.0b1, and 10.1's is 5.5
  compatible back to 1.1 only, so no one version satisfies both ("Installed version 1.0.0b1 of
  dependency com.apple.kernel.bsd is not compatible", live, 10.1.5, 2026-10-01). Without it the
  kext links against the kernel's symbols all the same; the BSD structures it uses (`vnode`,
  `mount`, the VFS operation arguments) are laid out alike in xnu-123 and xnu-201. Every symbol
  it imports is in 10.0's `mach_kernel`.
- On 10.0 and 10.1 the page lists of VOP_PAGEIN/PAGEOUT are mapped and committed with
  `kernel_upl_*` (`P9FS_ABI_GCC2`), which 10.0 has and 10.1 kept; `ubc_upl_*` came with 10.1.
- `p9fs_vnops.c` declares what 10.2's Kernel.framework lacks headers for (`sys/unistd.h`,
  `sys/signalvar.h`, and so `vfs/vfs_support.h`).

## Rebuilding, step by step

Guests: a 10.1 one with the December 2001 tools and a 10.2 one with the December 2002 tools
installed (see `../ppc/virtioconsole/README.md` for the images and how they go in), reachable over
SSH as root (`software/dropbear`). With the test pods that is
`kubectl -n macos port-forward pod/puma-guest-0 2201:22` and `pod/jaguar-guest-0 2202:22`; root's shell
is csh there, so commands go through `/bin/sh`.

1. On the host, once: 10.0's kernel headers and kmod libraries, from the 10.0 (4K78) tools CD
   `Mac OS X Developer Tools 691-2963-A.cdr` (MD5 `b148afc86e50a5ce4015851a8ca5a416`):

       hdiutil attach -readonly -nobrowse -mountpoint /tmp/dt100 "Mac OS X Developer Tools 691-2963-A.cdr"
       mkdir -p /tmp/x && (cd /tmp/x &&
         gzip -dc /tmp/dt100/Developer.pkg/Contents/Resources/Developer.pax.gz |
           pax -r -s ',^\./,,' './System/Library/Frameworks/Kernel.framework*' './usr/lib/libkmod*' \
             './usr/lib/gcc/darwin/2.95.2/libcc_kext.a' &&
         mkdir -p k100/lib && cp -R System/Library/Frameworks/Kernel.framework/Versions/A/Headers k100/Headers &&
         cp usr/lib/libkmod.a usr/lib/libkmodc++.a usr/lib/gcc/darwin/2.95.2/libcc_kext.a k100/lib/ &&
         tar cf - k100 | gzip -c > k100.tgz)
       hdiutil detach /tmp/dt100

2. Into the 10.1 guest (the archives need `ranlib` again once copied), and the source into both,
   from this directory:

       ssh -p 2201 root@127.0.0.1 /bin/sh -c "'cd /tmp && gzip -dc | tar xf - && ranlib k100/lib/*.a'" < /tmp/x/k100.tgz
       for p in 2201 2202; do
         (cd .. && COPYFILE_DISABLE=1 tar --format ustar --exclude build -cf - virtio9p) | gzip -c |
           ssh -p $p root@127.0.0.1 /bin/sh -c "'cd /tmp && rm -rf virtio9p && gzip -dc | tar xf -'"
       done

3. Build (about a minute each under TCG):

       echo 'cd /tmp/virtio9p && make ABI=gcc2 KERNEL_HEADERS=/tmp/k100/Headers KMOD_LIB_DIR=/tmp/k100/lib' |
         ssh -p 2201 root@127.0.0.1 /bin/sh
       echo 'cd /tmp/virtio9p && make ABI=gcc3' | ssh -p 2202 root@127.0.0.1 /bin/sh

4. Back to the host, into `build/` here, as one archive for whatever installs it:

       mkdir -p build && for b in gcc2:2201 gcc3:2202; do
         echo "cd /tmp/virtio9p/build && tar cf - ${b%:*}/Virtio9P.kext ${b%:*}/mount_9p" |
           ssh -p ${b#*:} root@127.0.0.1 /bin/sh | (cd build && tar xf -)
       done
       (cd build && COPYFILE_DISABLE=1 tar --format ustar --no-xattrs -czf Virtio9P.tar.gz gcc2 gcc3)

   `build/Virtio9P.tar.gz` holds `gcc2/` (10.0, 10.1) and `gcc3/` (10.2, 10.3), each with
   `Virtio9P.kext` and `mount_9p`. Installing is copying the kext to `/System/Library/Extensions`
   (owned root:wheel; it then loads at boot when the device is there) and `mount_9p` to `/sbin`.

`/tmp` in the guests is emptied at boot; step 2 is repeated after a restart.

## Loading it by hand, and testing

    cp -R build/gcc2/Virtio9P.kext /tmp/ && chown -R root:wheel /tmp/Virtio9P.kext
    kextload -v /tmp/Virtio9P.kext
    sh test.sh build/gcc2/mount_9p host /tmp/m9    # mounts, runs 32 checks, unmounts
    kextunload -m io.virtio.driver.Virtio9P          # 10.1; -b on 10.2 on

A tag is mounted once at a time, and `test.sh` mounts `host`: if it is mounted already (`mount` lists it by tag, as `host on /work/share`, not by type, so `mount | grep 9p` finds nothing), `mount_9p` fails, and the checks that follow run on a plain directory and mostly pass. Unmount it first, and `kextunload` the module of an earlier build before loading a new one (live, 10.2.8, 2026-10-01).

`test.sh` writes, reads back and compares 2.5 MB, renames, links (symbolic and hard), changes
modes and times, makes and lists 300 files, runs `/bin/echo` and `/bin/ls` copied onto the share
(exec, so page-in), and removes it all again.

## Not yet

- 10.4 and later: their VFS is the KPI (`vnode_t`, `vfs_fsadd`), a port of the filesystem half.
- Interrupts instead of polling; more than one message with the device at a time.
- 10.0 and 10.3 live: the `gcc2` build's imports are all in 10.0's kernel and the `gcc3` build
  is 10.2's, but neither has been run there yet.

## Evidence

- Built without warnings with both toolchains (10.1.5, GCC 2.95.2; 10.2.8, GCC 3.1), 2026-10-01.
- Loaded, registered `9p`, answered a mount with "no device" and unloaded on 10.1.5 and 10.2.8
  without a 9P device, 2026-10-01.
- With `-fsdev local,...,security_model=none -device virtio-9p-pci,disable-modern=on,...,
  mount_tag=host` (QEMU in the `puma-guest`/`jaguar-guest` pods, 2026-10-01): the driver attached ("mount
  tag "host", I/O at 0x1080, 128-entry queue, 65536-byte messages"), `test.sh` passed all 32
  checks with `gcc2` on 10.1.5 (5S60b) and `gcc3` on 10.2.8 (6R73); a 400 KB file written in
  the guest and a 300 KB one written on the host had the same `cksum` on both sides; a file
  changed and one created on the host were seen by the guest at once; `kextunload` while
  mounted was refused and the mount kept working, and after `umount` it unloaded.
- Every symbol the `gcc2` build imports is exported by 10.0's (4K78) `mach_kernel`.

- The same builds rebuilt from this tree in the guests and run again, 2026-10-01: `gcc2` on 10.1.5 and
  `gcc3` on 10.2.8, no compiler warnings, `test.sh` 32 of 32 on each.

## References

- elliotnunn/classicvirtio (MIT): `9p.c` and `device-9p.c`, 9P2000.L for the classic Mac OS
  File Manager over the same QEMU device on a big-endian guest; the set of messages used here.
- The 9P2000 RFC (ericvh.github.io/9p-rfc) and the 9P2000.L messages of QEMU's `hw/9pfs/9p.c`.
- The virtio specification's "9P Transport" device, and virtio 0.9.5 (legacy PCI).
- Apple's xnu-123.5 to xnu-517 (`bsd/nfs`, `bsd/vfs`, `bsd/kern/ubc_subr.c`): the VFS and UBC
  contracts, followed as NFS keeps them; smb-115 (`smbfs_vfsops.c`) for adding a filesystem from
  a kext on these releases.
