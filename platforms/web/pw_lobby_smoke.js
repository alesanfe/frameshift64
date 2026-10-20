/* E2E lobby smoke test for the web build.
 *
 * Opens two browser tabs against http://localhost:8931/:
 *   A hosts a room via  ?lobby_host=ws://localhost:8799
 *   B joins it via      ?lobby_join=ws://localhost:8799&lobby_code=<code>
 *
 * The room code is read from window.__ssb64_lobby_code (published by
 * auto_code_write — browser tabs can't share the code file).
 * Success = both tabs report a CONNECTED lobby state
 * in their console logs, i.e. the ws:// handshake, welcome, roster and
 * pick exchange all completed through the browser WebSocket transport.
 *
 * Usage: node pw_lobby_smoke.js
 */
const { chromium } = require('playwright');

const BASE = 'http://localhost:8931/BattleShip.html';
const LOBBY = 'ws://localhost:8799';
const BOOT_MS = 120000; // wasm boot + o2r restore can be slow
const LOBBY_MS = 45000;

function watch(page, tag, logs) {
  page.on('console', m => {
    const t = m.text();
    logs.push(t);
    if (/Lobby|lobby|SSB64 web env|netplay/i.test(t)) console.log(`[${tag}] ${t}`);
  });
  page.on('pageerror', e => console.log(`[${tag}] PAGEERROR ${e}`));
}

(async () => {
  const browser = await chromium.launch({ args: ['--enable-unsafe-swiftshader'] });
  const logsA = [], logsB = [];

  // ---- Tab A: host ----
  const ctxA = await browser.newContext();
  await ctxA.route('**/sw.js', r => r.fulfill({ status: 404, body: '' }));
  const pageA = await ctxA.newPage();
  watch(pageA, 'A', logsA);
  console.log('[A] open host tab');
  await pageA.goto(`${BASE}?autostart=1&lobby_host=${encodeURIComponent(LOBBY)}`);

  // wait for the room code — published to window.__ssb64_lobby_code by
  // auto_code_write on Emscripten once the host reaches WAITING
  let code = null;
  const t0 = Date.now();
  while (Date.now() - t0 < BOOT_MS + LOBBY_MS) {
    code = await pageA.evaluate(() => window.__ssb64_lobby_code || null);
    if (code) break;
    await pageA.waitForTimeout(1000);
  }
  if (!code) {
    console.log('[A] FAIL: no __ssb64_lobby_code after timeout');
    console.log(logsA.slice(-40).join('\n'));
    await browser.close();
    process.exit(1);
  }
  console.log(`[A] room code minted: ${code}`);

  // ---- Tab B: join ----
  const ctxB = await browser.newContext();
  await ctxB.route('**/sw.js', r => r.fulfill({ status: 404, body: '' }));
  const pageB = await ctxB.newPage();
  watch(pageB, 'B', logsB);
  console.log(`[B] open join tab code=${code}`);
  await pageB.goto(`${BASE}?autostart=1&lobby_join=${encodeURIComponent(LOBBY)}&lobby_code=${code}`);

  // wait until both tabs log a connected-state lobby line
  const waitConnected = async (logs, tag) => {
    const t = Date.now();
    while (Date.now() - t < LOBBY_MS) {
      if (logs.some(l => /connected|CONNECTED|roster|START/i.test(l) && /lobby|Lobby/.test(l))) return true;
      await new Promise(r => setTimeout(r, 500));
    }
    return false;
  };
  const okA = await waitConnected(logsA, 'A');
  const okB = await waitConnected(logsB, 'B');
  console.log(`[result] host connected=${okA} joiner connected=${okB}`);

  if (okA && okB) {
    console.log('LOBBY SMOKE: PASS');
  } else {
    console.log('LOBBY SMOKE: FAIL — last logs:');
    console.log('--- A ---'); console.log(logsA.slice(-30).join('\n'));
    console.log('--- B ---'); console.log(logsB.slice(-30).join('\n'));
  }
  await browser.close();
  process.exit(okA && okB ? 0 : 1);
})();
