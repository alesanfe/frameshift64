#!/usr/bin/env python3
"""Dev server for the web-wasm build.

The wasm binary uses Emscripten pthreads (SharedArrayBuffer), which browsers
only expose to cross-origin-isolated pages. This wraps `http.server` with the
required COOP/COEP headers.

Usage:
    python platforms/web/serve.py [build-dir] [port]
    python platforms/web/serve.py out/build/web-wasm 8931
"""
import functools
import http.server
import os
import sys

BUILD_DIR = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else "out/build/web-wasm")
PORT = int(sys.argv[2]) if len(sys.argv) > 2 else 8931


class COIHandler(http.server.SimpleHTTPRequestHandler):
    def end_headers(self):
        self.send_header("Cross-Origin-Opener-Policy", "same-origin")
        self.send_header("Cross-Origin-Embedder-Policy", "require-corp")
        # wasm must not be served stale between rebuilds
        self.send_header("Cache-Control", "no-store")
        super().end_headers()

    def log_message(self, fmt, *args):
        if not (args and str(args[0]).endswith("404")):
            super().log_message(fmt, *args)


if __name__ == "__main__":
    handler = functools.partial(COIHandler, directory=BUILD_DIR)
    http.server.ThreadingHTTPServer.allow_reuse_address = True
    with http.server.ThreadingHTTPServer(("", PORT), handler) as srv:
        print(f"Serving {BUILD_DIR} at http://localhost:{PORT}/ (cross-origin isolated)")
        srv.serve_forever()
