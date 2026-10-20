#!/usr/bin/env python3
"""E2E smoke test for the web asset picker (shell.html).

Serves out/build/web-wasm/ and drives the picker with a *fake* ROM —
the point is the wiring, not real extraction (no copyrighted ROM needed):

  picker visible -> drop/pick .z64 -> torch.js loads -> asset-recipes/
  manifest fetches + stages -> Extract() runs -> unknown-hash error ->
  status shows failure and the picker stays usable.

A real baserom would continue past Extract and boot the game; asserting
the *error path* proves every stage of the pipeline up to Companion::Init.

Usage: python platforms/web/pw_rom_picker.py [build_dir] [port]
"""

import functools
import http.server
import os
import sys
import threading

from playwright.sync_api import sync_playwright

BUILD = sys.argv[1] if len(sys.argv) > 1 else "out/build/web-wasm"
PORT = int(sys.argv[2]) if len(sys.argv) > 2 else 8937
BOOT_S = 60  # wasm boot + fetch attempts
EXTRACT_S = 120  # torch module load + recipe staging can be slow


class IsolatedHandler(http.server.SimpleHTTPRequestHandler):
    """pthread SharedArrayBuffer needs cross-origin isolation (serve.py).
    404 on BattleShip.o2r so the picker path is exercised even when the
    build dir contains a pre-extracted archive."""

    def send_head(self):
        if self.path.split("?")[0].endswith("BattleShip.o2r"):
            self.send_error(404)
            return None
        return super().send_head()

    def end_headers(self) -> None:
        self.send_header("Cross-Origin-Opener-Policy", "same-origin")
        self.send_header("Cross-Origin-Embedder-Policy", "require-corp")
        super().end_headers()


def main() -> int:
    handler = functools.partial(IsolatedHandler, directory=BUILD)
    srv = http.server.ThreadingHTTPServer(("127.0.0.1", PORT), handler)
    threading.Thread(target=srv.serve_forever, daemon=True).start()

    # Fake ROM: right size class (8 MiB), bogus content — Companion's
    # SHA1 lookup must reject it.
    fake_rom = os.path.join(os.environ.get("TEMP", "/tmp"), "fake.z64")
    with open(fake_rom, "wb") as fh:
        fh.write(os.urandom(8 * 1024 * 1024))

    logs = []
    try:
        with sync_playwright() as p:
            page = p.chromium.launch().new_page()
            page.on("console", lambda m: logs.append(m.text))
            page.on("pageerror", lambda e: logs.append(f"PAGEERROR {e}"))
            page.goto(f"http://127.0.0.1:{PORT}/BattleShip.html")

            # 1. With no o2r in IndexedDB/fetch, the picker must appear.
            try:
                page.wait_for_selector("#romButton:not(.hidden)", timeout=BOOT_S * 1000)
            except Exception:
                status = page.eval_on_selector("#status", "el => el.textContent")
                print(f"FAIL: picker never appeared; status={status!r}")
                print("\n".join(logs[-15:]))
                raise
            print("PASS: picker button appeared (no cached assets)")

            # 2. Pick the fake ROM -> extraction chain must run and reject it.
            page.set_input_files("#picker", fake_rom)
            try:
                page.wait_for_function(
                    "document.getElementById('status')"
                    ".textContent.match(/Failed|invalid|supported/)",
                    timeout=EXTRACT_S * 1000,
                )
            except Exception:
                status = page.eval_on_selector("#status", "el => el.textContent")
                print(f"FAIL: extraction never rejected; status={status!r}")
                print("\n".join(logs[-20:]))
                raise
            status = page.eval_on_selector("#status", "el => el.textContent")
            assert "Failed" in status, f"unexpected status: {status!r}"
            print(f"PASS: fake ROM rejected -> {status.splitlines()[0]!r}")

            # 3. Picker still usable after the failure.
            assert page.eval_on_selector(
                "#romButton", 'el => !el.classList.contains("hidden")'
            ), "picker hidden after failure"
            print("PASS: picker stays open after failure")

            # 4. torch.js actually loaded + Extract ran (console evidence).
            joined = "\n".join(logs)
            assert "[torch]" in joined or "extract" in joined.lower(), (
                "no torch activity in console"
            )
            print("PASS: torch module loaded and ran")
    finally:
        srv.shutdown()
    print("ALL CHECKS PASSED")
    return 0


if __name__ == "__main__":
    sys.exit(main())
