#!/usr/bin/env python3
"""Driver for ws_*_leak_test: spawn the loopback RFC6455 server, run the
test binary, tear the server down. Usage:

    ws_win32_leak_driver.py <test-binary> [clean-iters] [fail-iters]
                            [--cert PEM --key PEM | --tls]

Picks two free ports, waits for the server's ready file, forwards the
binary's exit code. Server output goes to stderr on failure only.

--cert/--key run the server over TLS; the caller decides client trust
(env such as CURL_CA_BUNDLE is a curl-TOOL knob — libcurl ignores it).

--tls is the self-contained wss:// mode: generates a self-signed cert
with openssl, serves TLS on both ports, sets WS_LEAK_SCHEME=wss, and
runs the binary inside a user+mount namespace that bind-mounts the cert
over the system CA bundle (Linux only — Schannel offers no equivalent
non-interactive trust override). Exits 77 (SKIP_RETURN_CODE) when
openssl, unshare, or a known CA-bundle path is missing."""

import os
import shutil
import socket
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
SERVER = os.path.join(HERE, "ws_win32_leak_server.py")
SKIP = 77
CA_BUNDLES = ("/etc/ssl/certs/ca-certificates.crt",   # Debian/Ubuntu
              "/etc/pki/tls/certs/ca-bundle.crt",      # Fedora/RHEL
              "/etc/ssl/cert.pem")                     # Alpine


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    p = s.getsockname()[1]
    s.close()
    return p


def tls_setup(tmp):
    """Generate a self-signed cert + pick the CA bundle to shadow.
    Returns (cert, key, cafile) or None when the toolchain is absent."""
    openssl = shutil.which("openssl")
    unshare = shutil.which("unshare")
    cafile = next((p for p in CA_BUNDLES if os.path.exists(p)), None)
    if not openssl or not unshare or not cafile:
        return None
    cert, key = os.path.join(tmp, "cert.pem"), os.path.join(tmp, "key.pem")
    r = subprocess.run(
        [openssl, "req", "-x509", "-newkey", "rsa:2048",
         "-keyout", key, "-out", cert, "-days", "1", "-nodes",
         "-subj", "/CN=127.0.0.1",
         "-addext", "subjectAltName=IP:127.0.0.1"],
        capture_output=True)
    if r.returncode != 0:
        return None
    # user namespaces can be kernel-disabled; probe before committing.
    if subprocess.run([unshare, "-rm", "true"]).returncode != 0:
        return None
    return cert, key, cafile


def main():
    argv = sys.argv[1:]
    if not argv:
        print("usage: ws_win32_leak_driver.py <binary> [clean] [fail] "
              "[--cert PEM --key PEM | --tls]", file=sys.stderr)
        return 2
    use_tls = "--tls" in argv
    if use_tls:
        argv.remove("--tls")
    cert = key = None
    if "--cert" in argv:
        i = argv.index("--cert")
        cert = argv[i + 1]
        key = argv[argv.index("--key") + 1] if "--key" in argv else None
        del argv[i:i + (4 if "--key" in argv else 2)]
    binary = argv[0]
    clean = argv[1] if len(argv) > 1 else "200"
    fail = argv[2] if len(argv) > 2 else "60"

    hold_port, die_port = free_port(), free_port()
    while die_port == hold_port:
        die_port = free_port()

    with tempfile.TemporaryDirectory() as tmp:
        cafile = None
        if use_tls:
            setup = tls_setup(tmp)
            if setup is None:
                print("ws-leak-driver: --tls needs openssl + unshare + a "
                      "known CA-bundle path; skipping", file=sys.stderr)
                return SKIP
            cert, key, cafile = setup

        ready = os.path.join(tmp, "ready")
        srv_cmd = [sys.executable, SERVER,
                   "--hold-port", str(hold_port),
                   "--die-port", str(die_port),
                   "--ready-file", ready]
        if cert:
            srv_cmd += ["--cert", cert]
            if key:
                srv_cmd += ["--key", key]
        srv = subprocess.Popen(
            srv_cmd,
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        try:
            deadline = time.time() + 10
            while time.time() < deadline and not os.path.exists(ready):
                if srv.poll() is not None:
                    print("leak-server exited early:\n" +
                          (srv.stdout.read() or ""), file=sys.stderr)
                    return 2
                time.sleep(0.05)
            if not os.path.exists(ready):
                print("leak-server never became ready", file=sys.stderr)
                return 2

            cmd = [binary, str(hold_port), str(die_port), clean, fail]
            env = None
            if cafile:
                # sh -c: $0=cert $1=cafile, then the real argv after
                # shift — bind the cert over the default trust store so
                # libcurl's compiled-in CA bundle verifies our cert.
                cmd = ["unshare", "-rm", "sh", "-c",
                       'mount --bind "$0" "$1" || exit 1; '
                       'shift 2; exec "$@"',
                       cert, cafile] + cmd
                env = dict(os.environ, WS_LEAK_SCHEME="wss")
            proc = subprocess.run(cmd, env=env)
            return proc.returncode
        finally:
            srv.kill()


if __name__ == "__main__":
    sys.exit(main())
