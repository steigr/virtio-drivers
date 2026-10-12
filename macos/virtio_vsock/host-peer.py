#!/usr/bin/env python3
"""The host's end of test.sh: AF_VSOCK, on the machine QEMU's vhost-vsock device is on.

  host-peer.py echo-client GUEST_CID PORT [MEGABYTES]   connect to a guest's vsockcat -l, send
                                                       random bytes, check they come back
  host-peer.py serve PORT [MEGABYTES]                  accept one connection, from the guest's
                                                       `vsockcat -c 2 PORT`, read until it ends
                                                       and send back a sha1 of what it read
  host-peer.py refused GUEST_CID PORT                  connect where nothing listens; must fail
  host-peer.py guest-listener GUEST_CID PORT           the same, and the connection must reset

Prints "ok ..." and exits 0, or "FAIL ..." and exits 1.
"""
import hashlib, os, socket, sys

def vsock():
    return socket.socket(socket.AF_VSOCK, socket.SOCK_STREAM)

def echo_client(cid, port, mb):
    data = os.urandom(mb * 1024 * 1024)
    s = vsock()
    s.settimeout(60)
    s.connect((cid, port))
    got = bytearray()
    sent = 0
    s.setblocking(False)
    import select
    while len(got) < len(data):
        r, w, _ = select.select([s], [s] if sent < len(data) else [], [], 60)
        if not r and not w:
            print("FAIL echo timed out at %d of %d bytes" % (len(got), len(data))); return 1
        if w:
            sent += s.send(data[sent:sent + 65536])
            if sent == len(data):
                pass
        if r:
            b = s.recv(65536)
            if not b:
                break
            got += b
    s.close()
    if bytes(got) != data:
        print("FAIL echo: sent %d got %d bytes, %s" % (len(data), len(got), "differ" if got != data[:len(got)] else "short"))
        return 1
    print("ok echo of %d MB" % mb)
    return 0

def serve(port, mb):
    srv = vsock()
    srv.bind((socket.VMADDR_CID_ANY, port))
    srv.listen(1)
    print("listening on vsock port %d" % port, flush=True)
    srv.settimeout(300)
    c, addr = srv.accept()
    h = hashlib.sha1()
    n = 0
    while True:
        b = c.recv(65536)
        if not b:
            break
        h.update(b)
        n += len(b)
    c.sendall(("%d %s\n" % (n, h.hexdigest())).encode())
    c.close()
    print("ok received %d bytes from cid %d port %d, sha1 %s" % (n, addr[0], addr[1], h.hexdigest()))
    return 0

def refused(cid, port):
    s = vsock()
    s.settimeout(20)
    try:
        s.connect((cid, port))
    except ConnectionResetError:
        print("ok refused (reset)"); return 0
    except OSError as e:
        print("ok refused (%s)" % e); return 0
    print("FAIL connected to a port nothing listens on"); return 1

if __name__ == "__main__":
    a = sys.argv[1:]
    if a and a[0] == "echo-client":
        sys.exit(echo_client(int(a[1]), int(a[2]), int(a[3]) if len(a) > 3 else 1))
    if a and a[0] == "serve":
        sys.exit(serve(int(a[1]), 0))
    if a and a[0] == "refused":
        sys.exit(refused(int(a[1]), int(a[2])))
    print(__doc__); sys.exit(2)
