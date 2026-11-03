#!/usr/bin/env python3
"""End-to-end test of the 9p filesystem: runs on the host against a guest started
with tests/iris/boot-ip28.sh and P9_SHARE set, drives the guest over telnet, and
looks at the share from this side too.

    tests/iris/run_9p_tests.py SHARE [--mb N] [--quick] [--readonly]

SHARE is the host directory given as P9_SHARE; its contents are this test's to
create and remove.  Empty it rather than recreating it: iris holds the directory
open, and a new one by that name is not the share.  IRIS_SRC is an iris checkout, for tools/irix-sh.py (default
../iris).
"""
import argparse
import hashlib
import os
import shutil
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
IRIS_SRC = os.environ.get("IRIS_SRC", os.path.join(os.path.dirname(ROOT), "iris"))
IRIX_SH = os.path.join(IRIS_SRC, "tools", "irix-sh.py")
TELNET_PORT = "12323"
MODULE = "/tmp/vioser.IP28/mod/p9fs.o"
MOUNT_9P = "/tmp/vioser.IP28/mount_9p"
MNT = "/mnt9p"
TEST = "/tmp/p9test"

results = []


def guest(*cmds, timeout=300):
    """Run shell commands in the guest; returns (exit status, output).

    IRIX's tty takes at most 256 characters a line, so keep each one short.
    """
    args = [sys.executable, "-I", IRIX_SH, "--port", TELNET_PORT, "--timeout", str(timeout), "run", "-k"]
    r = subprocess.run(args + list(cmds), capture_output=True, text=True, errors="replace")
    return r.returncode, r.stdout + r.stderr


def check(name, ok, detail=""):
    results.append((name, ok))
    print(f"{'PASS' if ok else 'FAIL'}  {name}" + (f"\n      {detail}" if detail and not ok else ""))
    sys.stdout.flush()


def build():
    rc, out = guest(
        "/sbin/mount | grep -q ' on /share ' || /sbin/mount /share",
        "rm -rf /tmp/vioser.IP28 /tmp/p9test*",
        "cd /share/driver && smake > /tmp/9b.log 2>&1; tail -3 /tmp/9b.log",
        "cd /share/tests/guest; for a in n32 64 32; do cc -$a -o /tmp/p9test.$a p9test.c 2>&1; done",
        f"cp /tmp/p9test.n32 {TEST}",
        f"cp {MOUNT_9P} /usr/etc/mount_9p; mkdir -p {MNT}",
        f"ls -l {MODULE} {TEST}",
    )
    check("build filesystem and test programs", rc == 0 and "Cannot access" not in out, out)
    return rc == 0


def load():
    rc, out = guest(f"/sbin/ml ld -v -j {MODULE} -p p9fs_ -a 9p", "/sbin/ml list -l | grep p9fs")
    check("load module", rc == 0 and "File system" in out, out)
    return rc == 0


def unload():
    rc, out = guest("id=`/sbin/ml list -l | awk '/p9fs/ {print $2}'`", "/sbin/ml unld $id", "/sbin/ml list -l | grep -c p9fs")
    check("unload module", "0" in out.split(), out)


def mount(opts="rw"):
    rc, out = guest(f"/sbin/mount -t 9p -o {opts} share {MNT}", f"/sbin/mount | grep ' {MNT} '")
    check(f"mount -t 9p -o {opts}", rc == 0 and "type 9p" in out, out)
    return rc == 0


def umount():
    rc, out = guest(f"/sbin/umount {MNT}", f"/sbin/mount | grep -c ' {MNT} '")
    check("umount", "0" in out.split(), out)


def test_guest_program(abi="n32"):
    rc, out = guest(f"mkdir -p {MNT}/t", f"/tmp/p9test.{abi} all {MNT}/t", f"rm -rf {MNT}/t", timeout=600)
    bad = [l for l in out.splitlines() if l.startswith("FAIL")]
    check(f"file operations from a {abi} program", rc == 0 and not bad and "0 failed" in out, "\n      ".join(bad) or out)


def test_readdir(abi, n):
    rc, out = guest(f"/tmp/p9test.{abi} readdir {MNT} {n}", timeout=600)
    check(f"directory of {n} entries, {abi} getdents", rc == 0 and "FAIL" not in out, out)


def test_host_to_guest(share):
    with open(os.path.join(share, "fromhost"), "w") as f:
        f.write("first\n")
    os.makedirs(os.path.join(share, "hostdir"), exist_ok=True)
    rc, out = guest(f"cat {MNT}/fromhost; ls {MNT}/hostdir | wc -l")
    check("guest sees a file the host made", "first" in out, out)

    # Reads are cached: a change on the host has to invalidate them.
    with open(os.path.join(share, "fromhost"), "w") as f:
        f.write("second, and longer\n")
    rc, out = guest(f"cat {MNT}/fromhost")
    check("guest sees the host's rewrite", "second, and longer" in out and "first" not in out, out)

    # Same size, so only the time says it moved.
    time.sleep(1.1)
    with open(os.path.join(share, "fromhost"), "w") as f:
        f.write("SECOND, AND LONGER\n")
    rc, out = guest(f"cat {MNT}/fromhost")
    check("guest sees a rewrite of the same size", "SECOND, AND LONGER" in out, out)

    # Under a long-lived reader, between reads.
    rc, out = guest(f"ls -la {MNT}/hostdir | wc -l")
    with open(os.path.join(share, "hostdir", "new"), "w") as f:
        f.write("late")
    rc, out = guest(f"ls {MNT}/hostdir")
    check("a new file in a known directory", "new" in out, out)
    os.remove(os.path.join(share, "hostdir", "new"))
    rc, out = guest(f"ls {MNT}/hostdir | wc -l; cat {MNT}/hostdir/new")
    check("a file the host removed is gone", "No such file" in out or "cannot open" in out, out)

    os.rename(os.path.join(share, "fromhost"), os.path.join(share, "renamed"))
    rc, out = guest(f"cat {MNT}/renamed; ls {MNT}/fromhost")
    check("a file the host renamed", "SECOND" in out and ("No such file" in out or "Cannot access" in out), out)
    os.remove(os.path.join(share, "renamed"))
    shutil.rmtree(os.path.join(share, "hostdir"))


def test_guest_to_host(share):
    rc, out = guest(f"echo from-the-guest > {MNT}/g; chmod 640 {MNT}/g")
    p = os.path.join(share, "g")
    ok = rc == 0 and os.path.exists(p) and open(p).read() == "from-the-guest\n"
    check("host sees what the guest wrote, once closed", ok, out)
    check("host sees the guest's chmod", ok and (os.stat(p).st_mode & 0o777) == 0o640)
    rc, out = guest(f"ln -s g {MNT}/gl; mkdir {MNT}/gd; mv {MNT}/g {MNT}/gd/g2")
    check("host sees symlink, mkdir and rename", os.path.islink(os.path.join(share, "gl")) and
          os.path.exists(os.path.join(share, "gd", "g2")), out)
    rc, out = guest(f"rm -rf {MNT}/gd {MNT}/gl")
    check("rm -rf", not os.path.exists(os.path.join(share, "gd")) and not os.path.lexists(os.path.join(share, "gl")), out)


def test_special_names(share):
    # Names only a host would make, and a symlink that points out of the share.
    weird = "a name with spaces & #marks"
    open(os.path.join(share, weird), "w").close()
    outside = os.path.join(os.path.dirname(os.path.abspath(share)), "9p-outside-the-share")
    with open(outside, "w") as f:
        f.write("HOST SECRET\n")
    os.symlink(outside, os.path.join(share, "outward"))
    os.symlink("../9p-outside-the-share", os.path.join(share, "outward2"))
    os.mkfifo(os.path.join(share, "fifo"))
    rc, out = guest(f"ls -l {MNT}/ | grep 'spaces'", f"cat {MNT}/outward; cat {MNT}/outward2",
                    f"ls -ld {MNT}/fifo", f"cat {MNT}/fifo")
    check("names with spaces", "#marks" in out, out)
    check("a host symlink out of the share does not leave it", "HOST SECRET" not in out, out)
    check("a host FIFO shows up", "prw" in out, out)
    os.remove(os.path.join(share, weird))
    os.remove(os.path.join(share, "outward"))
    os.remove(os.path.join(share, "outward2"))
    os.remove(os.path.join(share, "fifo"))
    os.remove(outside)


def pattern_sha(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        while True:
            d = f.read(1 << 20)
            if not d:
                return h.hexdigest()
            h.update(d)


def test_bulk(share, mb):
    # Written by the guest in odd sizes, read back through a cold cache.
    started = time.time()
    rc, out = guest(f"/tmp/p9test mkbig {MNT}/big {mb}", timeout=1800)
    took = time.time() - started
    check(f"guest writes {mb} MiB", rc == 0 and "written" in out, out)
    print(f"      {mb / took:.1f} MiB/s", flush=True)
    check("the file on the host is the right size", os.path.getsize(os.path.join(share, "big")) == mb << 20)
    host = pattern_sha(os.path.join(share, "big"))
    started = time.time()
    rc, out = guest(f"/tmp/p9test ckbig {MNT}/big {mb}", timeout=1800)
    took = time.time() - started
    check("guest reads it back, verified", rc == 0 and "verified" in out, out)
    print(f"      {mb / took:.1f} MiB/s", flush=True)

    # And a file only the host wrote, with the guest's sum to compare.
    data = os.urandom(3 << 20)
    with open(os.path.join(share, "rnd"), "wb") as f:
        f.write(data)
    rc, out = guest(f"sum {MNT}/rnd")
    check("guest's sum of a file from the host", rc == 0 and out.split()[0] == str(_sysv_sum(data)), out)
    os.remove(os.path.join(share, "rnd"))
    rc, out = guest(f"cp {MNT}/big {MNT}/big2; cmp {MNT}/big {MNT}/big2 && echo same", timeout=1800)
    check("cp within the share", "same" in out, out)
    check("cp on the host agrees", pattern_sha(os.path.join(share, "big2")) == host)
    os.remove(os.path.join(share, "big"))
    os.remove(os.path.join(share, "big2"))


def _sysv_sum(data):
    """What IRIX's sum(1) prints first: the System V checksum."""
    s = sum(data) & 0xffffffff
    r = (s & 0xffff) + (s >> 16)
    return (r & 0xffff) + (r >> 16)


def test_exec_and_build(share):
    # Programs run from the share, and a real build happens on it: this is what
    # the NFS /share cannot do.
    dest = os.path.join(share, "src")
    shutil.rmtree(dest, ignore_errors=True)
    shutil.copytree(os.path.join(ROOT, "driver"), dest)
    with open(os.path.join(dest, "hello.c"), "w") as f:
        f.write('#include <stdio.h>\nint main(void){printf("ran from 9p\\n");return 0;}\n')
    rc, out = guest(f"cd {MNT}/src && cc -n32 -o hello hello.c 2>&1 && ./hello", timeout=300)
    check("compile on the share and exec the result", "ran from 9p" in out, out)
    # Compiling is what stresses it: many small files, seeks, and ld's mmap.
    rc, out = guest(f"cd {MNT}/src && smake OBJDIR={MNT}/src/obj > /tmp/9sb.log 2>&1; echo rc=$?; tail -3 /tmp/9sb.log",
                    f"ls -l {MNT}/src/obj/mod/p9fs.o", timeout=900)
    check("smake of this driver on the share", "rc=0" in out and "p9fs.o" in out, out)
    if os.path.exists(os.path.join(dest, "obj", "mod", "p9fs.o")):
        check("the object is on the host", os.path.getsize(os.path.join(dest, "obj", "mod", "p9fs.o")) > 10000)
    rc, out = guest(f"cd {MNT} && tar cf /tmp/s.tar src && mkdir tx && cd tx && tar xf /tmp/s.tar && cd .. && diff -r src tx/src && echo identical",
                    timeout=600)
    check("tar out and back", "identical" in out, out)
    rc, out = guest(f"rm -rf {MNT}/src {MNT}/tx", timeout=300)
    check("clean up the tree", not os.path.exists(dest), out)


def test_busy():
    guest(f"echo x > {MNT}/held")
    # All in one session: the holder goes when the telnet session does.
    rc, out = guest(f"{TEST} hold {MNT}/held > /dev/null 2>&1 & sleep 2; cd /; /sbin/umount {MNT}; "
                    f"/sbin/mount | grep -c ' {MNT} '; kill $!")
    check("umount of a busy filesystem refuses", "busy" in out.lower() and "1" in out.split(), out)
    time.sleep(1)
    guest(f"rm -f {MNT}/held")


def test_readonly_mount(share):
    with open(os.path.join(share, "ro"), "w") as f:
        f.write("readable\n")
    if not mount("ro"):
        return
    rc, out = guest(f"cat {MNT}/ro", f"echo x > {MNT}/ro", f"touch {MNT}/new", f"rm {MNT}/ro", f"mkdir {MNT}/d")
    check("a read-only mount reads", "readable" in out, out)
    check("a read-only mount refuses writes", open(os.path.join(share, "ro")).read() == "readable\n"
          and not os.path.exists(os.path.join(share, "new")) and not os.path.exists(os.path.join(share, "d")), out)
    check("and says why", "Read-only" in out or "read-only" in out, out)
    umount()
    os.remove(os.path.join(share, "ro"))


def test_aname(share):
    os.makedirs(os.path.join(share, "sub"), exist_ok=True)
    with open(os.path.join(share, "sub", "under"), "w") as f:
        f.write("under the aname\n")
    rc, out = guest(f"/sbin/mount -t 9p -o aname=sub share {MNT}", f"cat {MNT}/under", f"ls -a {MNT}", f"/sbin/umount {MNT}")
    check("mount a directory of the share with aname", "under the aname" in out, out)
    rc, out = guest(f"/sbin/mount -t 9p -o aname=nonesuch share {MNT}", f"/sbin/mount | grep -c ' {MNT} '")
    check("an aname that is not there fails", "0" in out.split(), out)
    rc, out = guest(f"/sbin/mount -t 9p -o rw notthetag {MNT}", f"/sbin/mount | grep -c ' {MNT} '")
    check("a tag that is not there fails", "0" in out.split(), out)
    shutil.rmtree(os.path.join(share, "sub"))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("share")
    ap.add_argument("--mb", type=int, default=64)
    ap.add_argument("--quick", action="store_true", help="skip the slow build on the share")
    ap.add_argument("--readonly", action="store_true", help="the device itself is read-only")
    a = ap.parse_args()
    share = os.path.abspath(a.share)

    if os.listdir(share):
        print(f"{share} is not empty")
        return 2
    if not build() or not load():
        return 1
    if a.readonly:
        with open(os.path.join(share, "ro"), "w") as f:
            f.write("readable\n")
        mount("rw")
        rc, out = guest(f"cat {MNT}/ro", f"echo x > {MNT}/ro", f"mkdir {MNT}/d")
        check("a read-only device reads", "readable" in out, out)
        check("a read-only device refuses writes", open(os.path.join(share, "ro")).read() == "readable\n"
              and not os.path.exists(os.path.join(share, "d")), out)
        umount()
        os.remove(os.path.join(share, "ro"))
        unload()
    else:
        if not mount():
            return 1
        for abi in ("n32", "64", "32"):
            test_guest_program(abi)
        for abi in ("n32", "64", "32"):
            test_readdir(abi, 1500)
        test_guest_to_host(share)
        test_host_to_guest(share)
        test_special_names(share)
        test_bulk(share, a.mb)
        if not a.quick:
            test_exec_and_build(share)
        test_busy()
        umount()
        test_readonly_mount(share)
        test_aname(share)
        unload()
        # Twice round: nothing the first load left behind may be in the way.
        if load() and mount():
            rc, out = guest(f"echo again > {MNT}/again; cat {MNT}/again; rm {MNT}/again")
            check("after reloading the module", "again" in out, out)
            umount()
            unload()

    failed = [n for n, ok in results if not ok]
    print(f"\n{len(results) - len(failed)} of {len(results)} passed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
