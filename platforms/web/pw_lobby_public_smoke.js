/* E2E public-lobby match smoke for the web build.
 *
 * Opens two browser tabs against the dev server:
 *   A hosts a room via  ?lobby_host=wss://smash-netplay.jumpcircuit.workers.dev
 *                       &lobby_autostart=1 (host starts the match itself)
 *   B joins it via      ?lobby_join=<same>&lobby_code=<code>
 *
 * The room code is read from window.__ssb64_lobby_code — published by
 * auto_code_write on Emscripten once the host reaches WAITING (browser
 * tabs can't share the lobby_code.txt file).
 *
 * Success = both tabs reach CONNECTED through the public Cloudflare lobby
 * AND log match evidence (STARTM/handshake → running scene), i.e. the
 * whole wss:// signaling + relay path works end-to-end.
 *
 * Usage: node pw_lobby_public_smoke.js
 *   (run from a directory where playwright resolves; serve the build with
 *    python platforms/web/serve.py out/build/web-wasm 8931)
 */
const { chromium } = require('playwright');

const BASE = 'http://localhost:8931/BattleShip.html';
const LOBBY = 'wss://smash-netplay.jumpcircuit.workers.dev';
const BOOT_MS = 120000; // wasm boot + o2r restore can be slow
const LOBBY_MS = 60000;
const MATCH_MS = 90000;

function watch(page, tag, logs) {
  page.on('console', m => {
    const t = m.text();
    logs.push(t);
    if (/Lobby|lobby|SSB64 web env|netplay|match|STARTM|PAGEERROR|Aborted/i.test(t)) console.log(`[${tag}] ${t}`);
  });
  page.on('pageerror', e => console.log(`[${tag}] PAGEERROR ${e.message || e}`));
}

async function newTab(browser, tag, logs, url) {
  const ctx = await browser.newContext();
  // the app's own service worker would cache-first stale build artifacts
  // mid-test — keep runs deterministic by serving it a 404
  await ctx.route('**/sw.js', r => r.fulfill({ status: 404, body: '' }));
  const page = await ctx.newPage();
  watch(page, tag, logs);
  await page.goto(url);
  return page;
}

const waitLog = async (logs, re, ms) => {
  const t = Date.now();
  while (Date.now() - t < ms) {
    const hit = logs.find(l => re.test(l));
    if (hit) return hit;
    await new Promise(r => setTimeout(r, 500));
  }
  return null;
};

(async () => {
  const browser = await chromium.launch({ args: ['--enable-unsafe-swiftshader'] });
  const logsA = [], logsB = [];

  // ---- Tab A: host ----
  console.log('[A] open host tab');
  const pageA = await newTab(browser, 'A', logsA,
    `${BASE}?autostart=1&lobby_host=${encodeURIComponent(LOBBY)}&lobby_autostart=1&lobby_stage=5`);

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
  console.log(`[B] open join tab code=${code}`);
  const pageB = await newTab(browser, 'B', logsB,
    `${BASE}?autostart=1&lobby_join=${encodeURIComponent(LOBBY)}&lobby_code=${code}&lobby_fkind=3`);

  // both tabs must reach the connected roster state
  const connA = await waitLog(logsA, /reachable|players connected|Connected/i, LOBBY_MS);
  const connB = await waitLog(logsB, /reachable|players connected|Connected/i, LOBBY_MS);
  console.log(`[result] host connected=${!!connA} joiner connected=${!!connB}`);
  if (!connA || !connB) {
    console.log('LOBBY SMOKE: FAIL — last logs:');
    console.log('--- A ---'); console.log(logsA.slice(-30).join('\n'));
    console.log('--- B ---'); console.log(logsB.slice(-30).join('\n'));
    await browser.close();
    process.exit(1);
  }

  // autostart host sends STARTM once the roster is ready — both sides
  // must show match evidence (scene transition into the battle)
  const mA = await waitLog(logsA, /Match running|IN_MATCH|syNetPeerStart|STARTM|battle|scene/i, MATCH_MS);
  const mB = await waitLog(logsB, /Match running|IN_MATCH|syNetPeerStart|STARTM|battle|scene/i, MATCH_MS);
  console.log(`[result] host match=${!!mA}${mA ? ' (' + mA.slice(0, 90) + ')' : ''}`);
  console.log(`[result] joiner match=${!!mB}${mB ? ' (' + mB.slice(0, 90) + ')' : ''}`);

  // let the match run — frames must keep progressing on both tabs
  await pageA.waitForTimeout(15000);
  const frameA1 = logsA.filter(l => /Frame \d+ complete/.test(l)).slice(-1)[0];
  const frameB1 = logsB.filter(l => /Frame \d+ complete/.test(l)).slice(-1)[0];
  await pageA.waitForTimeout(15000);
  const frameA2 = logsA.filter(l => /Frame \d+ complete/.test(l)).slice(-1)[0];
  const frameB2 = logsB.filter(l => /Frame \d+ complete/.test(l)).slice(-1)[0];
  console.log(`[A] frames ${frameA1 || '?'} -> ${frameA2 || '?'}`);
  console.log(`[B] frames ${frameB1 || '?'} -> ${frameB2 || '?'}`);

  // real desyncs only — the periodic rollback stats end in "desync=0"
  const desync = [...logsA, ...logsB].filter(l => /desync=[1-9]|DESYNC DETECTED|divergen/i.test(l));
  console.log(`[result] desync lines: ${desync.length}`);

  await pageA.screenshot({ path: 'public_match_host.png' }).catch(() => {});
  await pageB.screenshot({ path: 'public_match_joiner.png' }).catch(() => {});

  const framesProgress = (a, b) => a && b && a !== b;
  const pass = mA && mB && desync.length === 0;
  if (!pass) {
    console.log('--- A tail ---'); console.log(logsA.slice(-25).join('\n'));
    console.log('--- B tail ---'); console.log(logsB.slice(-25).join('\n'));
  }
  console.log(`PUBLIC LOBBY MATCH: ${pass ? 'PASS' : 'FAIL'}` +
    (pass && !framesProgress(frameA1, frameA2) ? ' (frames not sampled)' : ''));
  await browser.close();
  process.exit(pass ? 0 : 1);
})().catch(e => { console.log('FATAL', e); process.exit(1); });
