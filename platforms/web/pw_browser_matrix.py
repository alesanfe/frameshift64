#!/usr/bin/env python3
"""Browser-matrix boot smoke for BattleShip Web (docs §20/§22).

For each Playwright engine present (chromium, firefox, webkit) drives the
real boot path against a COOP/COEP-serving local server:

  capability gate -> 'Ready.' -> Start click -> canvas sized -> no
  console errors (404s ignored) -> service worker state reported.

WebKit on Windows has no service-worker support in Playwright's bundled
build and Firefox needs dom.serviceWorkers.enabled, so SW assertions are
report-only. The hard gate is: page reaches Ready and a real canvas with
non-trivial resolution appears after Start.

Usage: python platforms/web/pw_browser_matrix.py [build_dir] [port]
       BUILD_DIR must already contain BattleShip.{html,js,wasm,o2r}.
"""

import functools
import http.server
import sys
import threading

from playwright.sync_api import sync_playwright

BUILD = sys.argv[1] if len(sys.argv) > 1 else "out/build/web-wasm"
PORT = int(sys.argv[2]) if len(sys.argv) > 2 else 8939
BOOT_S = 90  # wasm compile on slower engines


class IsolatedHandler(http.server.SimpleHTTPRequestHandler):
    def end_headers(self) -> None:
        self.send_header("Cross-Origin-Opener-Policy", "same-origin")
        self.send_header("Cross-Origin-Embedder-Policy", "require-corp")
        super().end_headers()

    def log_message(self, *a):  # quiet
        pass


# Benign per-engine console noise that must not fail the matrix:
# 'interactive-widget' is a Chromium-only viewport meta key — WebKit
# reports ignoring it, which is the intended graceful-degradation path.
BENIGN_ERRORS = ("interactive-widget",)


def check(engine_name, page):
    errors = []
    page.on("pageerror", lambda e: errors.append(f"PAGEERROR {e}"))
    page.on("console", lambda m: errors.append(m.text[:200])
            if m.type == "error" and "404" not in m.text
            and "favicon" not in m.text
            and not any(b in m.text for b in BENIGN_ERRORS) else None)
    page.goto(f"http://127.0.0.1:{PORT}/BattleShip.html")
    page.wait_for_selector("#startButton:not(.hidden)", timeout=BOOT_S * 1000)
    # DOM click, not page.click: pointer actionability polling times out
    # while the engine is still busy compiling wasm (the un-hidden button
    # IS the readiness signal — hit-testing isn't under test here).
    page.eval_on_selector("#startButton", "b => b.click()")
    page.wait_for_timeout(8000)
    size = page.eval_on_selector("#canvas", "e => [e.width, e.height]")
    assert size[0] >= 320 and size[1] >= 240, f"canvas never sized: {size}"
    sw = page.evaluate("!!(navigator.serviceWorker && navigator.serviceWorker.controller)")
    return {"canvas": f"{size[0]}x{size[1]}", "sw": sw, "errors": errors}


def main() -> int:
    handler = functools.partial(IsolatedHandler, directory=BUILD)
    http.server.ThreadingHTTPServer.allow_reuse_address = True
    srv = http.server.ThreadingHTTPServer(("127.0.0.1", PORT), handler)
    threading.Thread(target=srv.serve_forever, daemon=True).start()

    results = {}
    try:
        with sync_playwright() as p:
            for name in ("chromium", "firefox", "webkit"):
                engine = getattr(p, name)
                try:
                    browser = engine.launch()
                except Exception as e:
                    results[name] = {"skipped": str(e).splitlines()[0][:120]}
                    continue
                try:
                    results[name] = check(name, browser.new_page())
                except Exception as e:
                    results[name] = {"failed": str(e).splitlines()[0][:200]}
                browser.close()
    finally:
        srv.shutdown()

    ok = True
    for name, r in results.items():
        if "skipped" in r:
            print(f"{name}: SKIP ({r['skipped']})")
        elif "failed" in r:
            print(f"{name}: FAIL {r['failed']}")
            ok = False
        else:
            tag = "PASS" if not r["errors"] else "PASS*"
            print(f"{name}: {tag} canvas={r['canvas']} sw={r['sw']}"
                  + (f" errors={r['errors'][:3]}" if r["errors"] else ""))
            if r["errors"]:
                ok = False
    print("MATRIX:", "ALL PASS" if ok else "FAILURES PRESENT")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
