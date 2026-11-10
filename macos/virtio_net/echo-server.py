#!/usr/bin/env python3
"""A TCP echo server for test.sh, run on the QEMU side of a user (slirp) netdev:

    python3 echo-server.py [port]     # default 9000, on 127.0.0.1

The guest reaches it as 10.0.2.2:<port>. Every connection gets back what it sends."""

import socket
import sys
import threading

def serve(conn):
    with conn:
        while True:
            data = conn.recv(65536)
            if not data:
                return
            conn.sendall(data)

port = int(sys.argv[1]) if len(sys.argv) > 1 else 9000
s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(("127.0.0.1", port))
s.listen(8)
while True:
    conn, _ = s.accept()
    threading.Thread(target=serve, args=(conn,), daemon=True).start()
