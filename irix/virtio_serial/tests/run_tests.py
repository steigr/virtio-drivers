#!/usr/bin/env python3
"""End-to-end test of the driver: runs on the host against a guest started with
tests/iris/boot-ip28.sh, drives the guest over telnet and plays the host ends of
the ports itself.

    tests/iris/run_tests.py WORKDIR [--bytes N]

WORKDIR is boot-ip28.sh's work directory, which holds agent.sock.
IRIS_SRC is an iris checkout, for tools/irix-sh.py (default ../iris).
"""
import argparse
import os
import socket
import subprocess
import sys
import threading
import time

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
IRIS_SRC = os.environ.get("IRIS_SRC", os.path.join(os.path.dirname(ROOT), "iris"))
IRIX_SH = os.path.join(IRIS_SRC, "tools", "irix-sh.py")
TELNET_PORT = "12323"
CONSOLE_TCP = 18890
AGENT_SOCK = "agent.sock"
MODULE = "/tmp/virtio_serial.IP28/mod/virtio_serial.o"
VSTEST = "/tmp/vstest"

AGENT = "/hw/virtio-ports/org.iris.agent.0"
UNWIRED = "/hw/virtio-ports/org.iris.unwired.0"
CONSOLE = "/hw/virtio-serial/port0"


def guest(*cmds, timeout=300):
    """Run shell commands in the guest; returns (exit status, output).

    IRIX's tty takes at most 256 characters a line, so keep each one short.
    """
    args = [sys.executable, "-I", IRIX_SH, "--port", TELNET_PORT, "--timeout", str(timeout), "run", "-k"]
    r = subprocess.run(args + list(cmds), capture_output=True, text=True, errors="replace")
    return r.returncode, r.stdout + r.stderr


class GuestJob(threading.Thread):
    """A guest command running while the host side does its part."""

    def __init__(self, *cmds, timeout=300):
        super().__init__(daemon=True)
        self.cmds, self.timeout, self.result = cmds, timeout, None
        self.start()

    def run(self):
        self.result = guest(*self.cmds, timeout=self.timeout)

    def finish(self):
        self.join(self.timeout + 30)
        return self.result if self.result else (124, "guest job never finished")


results = []


def check(name, ok, detail=""):
    results.append((name, ok))
    print(f"{'PASS' if ok else 'FAIL'}  {name}" + (f"\n      {detail}" if detail and not ok else ""))
    sys.stdout.flush()


def connect_tcp(port):
    s = socket.create_connection(("127.0.0.1", port), timeout=30)
    s.settimeout(120)
    return s


def connect_unix(path):
    s = socket.socket(socket.AF_UNIX)
    s.settimeout(120)
    s.connect(path)
    return s


def recv_exact(s, n):
    data = b""
    while len(data) < n:
        chunk = s.recv(min(65536, n - len(data)))
        if not chunk:
            break
        data += chunk
    return data


def build():
    rc, out = guest(
        # The image's rc script only retries the mount in the background.
        "/sbin/mount | grep -q ' on /share ' || /sbin/mount /share",
        "rm -rf /tmp/virtio_serial.IP28 /tmp/vstest",
        "cd /share/driver && smake 2>&1 | tail -20",
        "cd /share/tests/guest && cc -n32 -I/share/driver -o /tmp/vstest vstest.c 2>&1",
        f"ls -l {MODULE} {VSTEST}",
    )
    check("build driver and test program", rc == 0 and "Cannot access" not in out, out)
    return rc == 0


def load():
    rc, out = guest(f"/sbin/ml ld -v -c {MODULE} -p virtio_serial_", "ls /hw/virtio-serial /hw/virtio-ports")
    ok = rc == 0 and "org.iris.agent.0" in out and "port2" in out
    check("load module, ports appear in /hw", ok, out)
    return ok


def unload():
    rc, out = guest("id=`/sbin/ml list -l | awk '/virtio_serial/ {print $2}'`", "/sbin/ml unld $id", "ls /hw/virtio-serial")
    check("unload module, ports gone", rc != 0 or "port" not in out.replace("virtio-serial", ""), out)


def test_info(hold):
    _, out = guest(f"{VSTEST} info {CONSOLE}", f"{VSTEST} info {AGENT}", f"{VSTEST} info {UNWIRED}")
    check("console port: number and flag", "port=0 console=1" in out, out)
    check("named port: number and name", "port=1 console=0" in out and "name=org.iris.agent.0" in out, out)
    check("connected host end reported", "port=1 console=0 host_open=1" in out, out)
    check("unwired port reports no host", "port=2 console=0 host_open=0 name=org.iris.unwired.0" in out, out)


def test_guest_to_host(sock):
    rc, out = guest(f"{VSTEST} send {AGENT} hello-from-irix")
    data = recv_exact(sock, len("hello-from-irix"))
    check("guest to host", rc == 0 and data == b"hello-from-irix", f"{out!r} {data!r}")


def test_host_to_guest(sock):
    job = GuestJob(f"{VSTEST} recv {AGENT} 14")
    time.sleep(3)  # bytes are held until the guest opens the port
    sock.sendall(b"hello-from-mac")
    rc, out = job.finish()
    check("host to guest", rc == 0 and "hello-from-mac" in out, out)


def test_console_port():
    s = connect_tcp(CONSOLE_TCP)
    time.sleep(0.5)
    rc, out = guest(f"{VSTEST} send {CONSOLE} on-port-zero")
    data = recv_exact(s, len("on-port-zero"))
    s.close()
    check("port 0 over TCP", rc == 0 and data == b"on-port-zero", f"{out!r} {data!r}")


def test_errors():
    rc, out = guest(f"{VSTEST} busy {AGENT}")
    check("second open is EBUSY", rc == 0 and "EBUSY" in out, out)
    rc, out = guest(f"{VSTEST} signal {AGENT}", timeout=60)
    check("a signal interrupts a blocked read", rc == 0 and "EINTR" in out, out)
    rc, out = guest(f"{VSTEST} nonblock {AGENT}")
    check("empty O_NONBLOCK read is EAGAIN", rc == 0 and "EAGAIN" in out, out)
    rc, out = guest(f"{VSTEST} eof {UNWIRED}", timeout=60)
    check("read on an unconnected host end is end of file", rc == 0 and "eof after 0" in out, out)


def test_eof_on_disconnect():
    s = connect_unix(AGENT_SOCK)
    time.sleep(1)
    job = GuestJob(f"{VSTEST} eof {AGENT}")
    time.sleep(3)
    s.sendall(b"abcde")
    time.sleep(1)
    s.close()
    rc, out = job.finish()
    check("host disconnect ends a blocked read", rc == 0 and "eof after 5 bytes" in out, out)


def test_echo(sock, nbytes):
    stop = threading.Event()
    total = [0]

    def echo():
        sock.settimeout(1)
        while not stop.is_set():
            try:
                d = sock.recv(65536)
            except socket.timeout:
                continue
            if not d:
                break
            total[0] += len(d)
            sock.sendall(d)

    t = threading.Thread(target=echo, daemon=True)
    t.start()
    started = time.time()
    rc, out = guest(f"{VSTEST} sum {AGENT} {nbytes}", timeout=900)
    took = time.time() - started
    stop.set()
    t.join(5)
    sock.settimeout(120)
    check(f"echo {nbytes} bytes both ways, verified, with poll()", rc == 0 and f"ok {nbytes} bytes" in out, out)
    print(f"      {nbytes} bytes each way in {took:.1f}s, {nbytes / took / 1024:.0f} KiB/s", flush=True)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("workdir")
    ap.add_argument("--bytes", type=int, default=1 << 20)
    a = ap.parse_args()
    os.chdir(a.workdir)  # a relative path keeps the socket's under SUN_LEN

    if not build() or not load():
        return 1
    sock = connect_unix(AGENT_SOCK)
    time.sleep(1)
    test_info(sock)
    test_guest_to_host(sock)
    test_host_to_guest(sock)
    test_console_port()
    test_errors()
    test_echo(sock, a.bytes)
    sock.close()
    time.sleep(1)
    test_eof_on_disconnect()
    unload()
    if load():
        s = connect_unix(AGENT_SOCK)
        time.sleep(1)
        test_guest_to_host(s)
        s.close()
        unload()

    failed = [n for n, ok in results if not ok]
    print(f"\n{len(results) - len(failed)} of {len(results)} passed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
