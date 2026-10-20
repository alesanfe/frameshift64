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


# Content-Security-Policy for the game page. The Emscripten shell embeds its
# loader inline (unsafe-inline); 'unsafe-eval' is REQUIRED here — two runtime
# paths eval JS by design: EM_ASM blocks (kbproxy flag) and
# emscripten_run_script_string (the URL→env bridge in port_web_stubs.cpp).
CSP = (
    "default-src 'self'; "
    "script-src 'self' 'unsafe-inline' 'unsafe-eval' blob:; "
    "worker-src 'self' blob:; "
    "connect-src 'self' ws: wss:; "
    "img-src 'self' data: blob:; "
    "style-src 'self' 'unsafe-inline'; "
    "font-src 'self'; "
    "manifest-src 'self'; "
    "media-src 'self'; "
    "object-src 'none'; "
    "base-uri 'none'; "
    "form-action 'none'; "
    "frame-ancestors 'self'"
)


class COIHandler(http.server.SimpleHTTPRequestHandler):
    def _redirect_root(self):
        # land users on the game page instead of a directory listing
        if self.path in ("/", "/index.html"):
            self.send_response(302)
            self.send_header("Location", "/BattleShip.html")
            self.end_headers()
            return True
        return False

    def do_GET(self):
        if not self._redirect_root():
            super().do_GET()

    def do_HEAD(self):
        if not self._redirect_root():
            super().do_HEAD()

    def end_headers(self):
        self.send_header("Cross-Origin-Opener-Policy", "same-origin")
        self.send_header("Cross-Origin-Embedder-Policy", "require-corp")
        self.send_header("Cross-Origin-Resource-Policy", "same-origin")
        self.send_header("X-Content-Type-Options", "nosniff")
        self.send_header("Referrer-Policy", "no-referrer")
        self.send_header(
            "Permissions-Policy",
            "camera=(), microphone=(), geolocation=(), payment=(), usb=()",
        )
        self.send_header("Content-Security-Policy", CSP)
        # wasm must not be served stale between rebuilds
        self.send_header("Cache-Control", "no-store")
        super().end_headers()

    def log_message(self, fmt, *args):
        if not (args and str(args[0]).endswith("404")):
            super().log_message(fmt, *args)


if __name__ == "__main__":
    handler = functools.partial(COIHandler, directory=BUILD_DIR)
    # On Windows SO_REUSEADDR lets a second server silently co-bind an
    # already-LISTENING port — stacked dead listeners then answer with
    # empty replies. Bind exclusively so a duplicate instance fails fast.
    http.server.ThreadingHTTPServer.allow_reuse_address = False
    with http.server.ThreadingHTTPServer(("", PORT), handler) as srv:
        print(f"Serving {BUILD_DIR} at http://localhost:{PORT}/ (cross-origin isolated)")
        srv.serve_forever()
