#!/usr/bin/env python3
"""Minimal RFC6455 server for ws_win32_leak_test — stdlib only.

Two listener ports:
  --hold-port  handshake, then drain frames until the peer closes
               (clean connect/send/close churn)
  --die-port   handshake, wait ~15ms for one frame, then close the TCP
               socket ungracefully (forces the client's next send to
               fail -> lazy uplink re-dial — the path that used to
               orphan the intermediate WinHTTP handles)

The test binary drives the loop counts; this script only needs to keep
accepting forever. Exits on SIGTERM (test teardown kills the process).
"""

import argparse
import base64
import hashlib
import socket
import ssl
import threading
import time

GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"


def read_headers(conn):
    buf = b""
    while b"\r\n\r\n" not in buf:
        chunk = conn.recv(4096)
        if not chunk:
            return None
        buf += chunk
        if len(buf) > 16384:
            return None
    head, _, rest = buf.partition(b"\r\n\r\n")
    return head, rest


def handshake(conn):
    got = read_headers(conn)
    if got is None:
        return None
    head, rest = got
    key = None
    for line in head.split(b"\r\n"):
        if line.lower().startswith(b"sec-websocket-key:"):
            key = line.split(b":", 1)[1].strip()
    if key is None:
        return None
    accept = base64.b64encode(
        hashlib.sha1(key + GUID.encode()).digest()).decode()
    conn.sendall(
        ("HTTP/1.1 101 Switching Protocols\r\n"
         "Upgrade: websocket\r\n"
         "Connection: Upgrade\r\n"
         f"Sec-WebSocket-Accept: {accept}\r\n\r\n").encode())
    return rest  # bytes past the header block = already-received frame data


def drain_frame(conn, buf):
    """Consume one masked client frame; returns (opcode, leftover) or None.

    A 0x8 close frame gets an RFC6455 close reply before the socket goes
    away — without it the client's graceful close blocks ~10s waiting
    for the peer's close answer. That stall is real client behavior
    (WinHttpWebSocketClose), just not what this test measures."""
    while len(buf) < 2:
        chunk = conn.recv(4096)
        if not chunk:
            return None
        buf += chunk
    opcode = buf[0] & 0x0F
    plen = buf[1] & 0x7F
    off = 2
    if plen == 126:
        off += 2
    elif plen == 127:
        off += 8
    masked = (buf[1] & 0x80) != 0
    off += 4 if masked else 0
    # don't bother decoding payload — just consume header + declared len
    need = off + plen
    while len(buf) < need:
        chunk = conn.recv(4096)
        if not chunk:
            return None
        buf += chunk
    if opcode == 0x8:
        try:
            conn.sendall(b"\x88\x80\x00\x00\x00\x00\x00\x00")
        except OSError:
            pass
        return 0x8, buf[need:]
    return opcode, buf[need:]


def serve_hold(conn):
    try:
        buf = handshake(conn)
        if buf is None:
            return
        conn.settimeout(30)
        while buf is not None:
            got = drain_frame(conn, buf)
            if got is None:
                break
            opcode, buf = got
            if opcode == 0x8:
                break
    except (OSError, socket.timeout):
        pass
    finally:
        try:
            conn.close()
        except OSError:
            pass


def serve_die(conn):
    try:
        buf = handshake(conn)
        if buf is None:
            return
        # Give the client a beat to push a frame, then die. A lingering
        # close makes the client's next send hit a dead link
        # deterministically instead of racing TCP teardown.
        conn.settimeout(0.015)
        try:
            if buf is not None:
                got = drain_frame(conn, buf)
                if got is not None:
                    buf = got[1]
        except socket.timeout:
            pass
    except OSError:
        pass
    finally:
        # Plain close, not SO_LINGER=0: an RST here can destroy the 101
        # still queued in the client's receive buffer (TLS record latency
        # makes wss:// hit this window; ws:// usually didn't). FIN keeps
        # the handshake deliverable while the kernel RSTs any post-close
        # frame the client sends — the dead-link behavior the test needs.
        try:
            conn.close()
        except OSError:
            pass


def listener(port, handler, ready, tls_ctx=None):
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", port))
    srv.listen(64)
    ready.set()
    while True:
        try:
            conn, _ = srv.accept()
        except OSError:
            return
        if tls_ctx is not None:
            try:
                conn = tls_ctx.wrap_socket(conn, server_side=True)
            except (ssl.SSLError, OSError):
                try:
                    conn.close()
                except OSError:
                    pass
                continue
        threading.Thread(target=handler, args=(conn,), daemon=True).start()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--hold-port", type=int, required=True)
    ap.add_argument("--die-port", type=int, required=True)
    ap.add_argument("--ready-file", required=True,
                    help="written once both listeners are bound")
    ap.add_argument("--cert", help="PEM cert+CA chain — enables wss://")
    ap.add_argument("--key", help="PEM private key for --cert")
    args = ap.parse_args()

    tls_ctx = None
    if args.cert:
        tls_ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        tls_ctx.load_cert_chain(args.cert, args.key)

    r1, r2 = threading.Event(), threading.Event()
    threading.Thread(target=listener, args=(args.hold_port, serve_hold, r1, tls_ctx),
                     daemon=True).start()
    threading.Thread(target=listener, args=(args.die_port, serve_die, r2, tls_ctx),
                     daemon=True).start()
    r1.wait(); r2.wait()
    with open(args.ready_file, "w") as f:
        f.write("ready")
    while True:
        time.sleep(3600)


if __name__ == "__main__":
    main()
