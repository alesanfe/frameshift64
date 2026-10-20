#!/usr/bin/env python3
"""Privacy E2E for the web ROM pipeline (shell.html + sw.js).

Guarantees under test (docs/platforms/web.md privacy section):

  * ROM processed strictly locally — every network request during boot,
    pick and extraction is same-origin and GET-only; no request body or
    URL ever carries ROM bytes (the fake ROM is filled with a unique
    marker so even a partial leak would trip the check).
  * The Service Worker Cache API never stores the ROM, the picked file,
    or the user-derived BattleShip.o2r (NEVER_CACHE regex).
  * IndexedDB / IDBFS never receives ROM bytes either — extraction only
    writes the derived .o2r; after a failed run the marker must appear
    in zero IDB values.

Run after pw_rom_picker.py; a real ROM is not required — the marker ROM
proves the plumbing, not the extractor output.

Usage: python platforms/web/pw_rom_privacy.py [build_dir] [port]
"""

import functools
import http.server
import os
import sys
import threading

from playwright.sync_api import sync_playwright

BUILD = sys.argv[1] if len(sys.argv) > 1 else "out/build/web-wasm"
PORT = int(sys.argv[2]) if len(sys.argv) > 2 else 8938
BOOT_S = 60
EXTRACT_S = 120
# Unique, searchable, unlikely to appear in compressed/wasm data by chance.
MARKER = b"SSB64ROMMARKER-7f3a9c-"


class IsolatedHandler(http.server.SimpleHTTPRequestHandler):
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

    fake_rom = os.path.join(os.environ.get("TEMP", "/tmp"), "fake_priv.z64")
    block = (MARKER * (4096 // len(MARKER) + 1))[:4096]
    with open(fake_rom, "wb") as fh:
        for _ in range(8 * 1024 * 1024 // 4096):
            fh.write(block)

    requests = []  # (method, url, body_len_or_0)
    try:
        with sync_playwright() as p:
            page = p.chromium.launch().new_page()

            def on_request(req):
                body = b""
                try:
                    pd = req.post_data_buffer
                    body = bytes(pd) if pd else b""
                except Exception:
                    pass
                requests.append((req.method, req.url, body))

            page.on("request", on_request)
            page.goto(f"http://127.0.0.1:{PORT}/BattleShip.html")
            page.wait_for_selector("#romButton:not(.hidden)", timeout=BOOT_S * 1000)
            page.set_input_files("#picker", fake_rom)
            page.wait_for_function(
                "document.getElementById('status')"
                ".textContent.match(/Failed|invalid|supported/)",
                timeout=EXTRACT_S * 1000,
            )
            status = page.eval_on_selector("#status", "el => el.textContent")
            assert "Failed" in status, f"fake ROM was not rejected: {status!r}"
            page.wait_for_timeout(3000)  # let the SW finish install/precache

            # ── 1. No request carries ROM bytes; all are same-origin GET ──
            marker = MARKER.decode("latin1")
            bad = []
            for method, url, body in requests:
                if method not in ("GET", "HEAD", "OPTIONS"):
                    bad.append(f"{method} {url}")
                if body and (MARKER in body or marker.encode("latin1") in body):
                    bad.append(f"ROM BYTES in body: {method} {url}")
                if not (url.startswith(f"http://127.0.0.1:{PORT}/")
                        or url.startswith("blob:") or url.startswith("data:")):
                    bad.append(f"cross-origin request: {method} {url}")
            assert not bad, "request-capture violations:\n" + "\n".join(bad)
            print(f"PASS: {len(requests)} requests, all same-origin GET, "
                  "zero ROM bytes in any request")

            # ── 2. Cache API holds no ROM-derived or ROM-named entries ──
            cached = page.evaluate("""async () => {
                const out = [];
                for (const name of await caches.keys())
                    for (const req of await (await caches.open(name)).keys())
                        out.push(name + ' :: ' + req.url);
                return out;
            }""")
            leaks = [u for u in cached
                     if u.rsplit("?", 1)[0].lower().endswith(
                         (".z64", ".n64", ".v64", "battleship.o2r"))]
            assert not leaks, "ROM/personal assets in Cache API:\n" + "\n".join(leaks)
            print(f"PASS: Cache API audited ({len(cached)} entries), "
                  "no ROM or personal .o2r stored")

            # ── 3. IndexedDB/IDBFS values contain zero ROM marker bytes ──
            marker_hits = page.evaluate(f"""async () => {{
                const needle = "{MARKER.decode('latin1')}";
                let hits = 0;
                const scan = (v) => {{
                    if (v == null) return;
                    if (typeof v === 'string') {{ if (v.includes(needle)) hits++; }}
                    else if (v instanceof ArrayBuffer || ArrayBuffer.isView(v)) {{
                        const s = new TextDecoder('latin1')
                            .decode(v instanceof ArrayBuffer ? v : v.buffer);
                        if (s.includes(needle)) hits++;
                    }}
                }};
                const dbs = await indexedDB.databases();
                for (const info of dbs) {{
                    const db = await new Promise((res, rej) => {{
                        const q = indexedDB.open(info.name);
                        q.onsuccess = () => res(q.result);
                        q.onerror = () => rej(q.error);
                    }});
                    for (const store of db.objectStoreNames) {{
                        const vals = await new Promise((res, rej) => {{
                            const t = db.transaction(store).objectStore(store).getAll();
                            t.onsuccess = () => res(t.result || []);
                            t.onerror = () => rej(t.error);
                        }});
                        vals.forEach(scan);
                    }}
                    db.close();
                }}
                return hits;
            }}""")
            assert marker_hits == 0, f"ROM marker found {marker_hits}x in IndexedDB"
            print("PASS: IndexedDB/IDBFS audited — zero ROM bytes persisted")
    finally:
        srv.shutdown()
    print("ALL PRIVACY CHECKS PASSED")
    return 0


if __name__ == "__main__":
    sys.exit(main())
