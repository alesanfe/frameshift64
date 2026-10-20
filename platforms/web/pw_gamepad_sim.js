/* E2E simulated-gamepad smoke for the web build.
 *
 * Simulates a physical controller without hardware: installs a synthetic
 * standard-mapping Gamepad behind navigator.getGamepads() before the page
 * loads, then fires the real gamepadconnected event. Everything upstream
 * is the production path — Emscripten-SDL's joystick backend polls
 * navigator.getGamepads() and reads .buttons/.axes exactly as it would a
 * real pad, so this exercises shell UX + SDL binding + game input.
 *
 * Asserts:
 *   1. touch overlay hides while a pad is connected (product behavior:
 *      a paired pad replaces the overlay, else it would double-bind)
 *   2. button presses reach the *game-consumed* N64 pad state — proven by
 *      window.__ssb64_p1_buttons (published each frame by
 *      port_web_touch_poll from gSYControllerDevices[0])
 *   3. gamepaddisconnected → overlay returns
 *
 * Usage: node pw_gamepad_sim.js
 *   (serve the build with: python platforms/web/serve.py out/build/web-wasm 8931)
 */
const { chromium, devices } = require('playwright');

const BASE = 'http://localhost:8931/BattleShip.html';
const BOOT_MS = 120000;
const logs = [];
const N64_START = 0x1000;

/* Synthetic standard Gamepad — same shape Emscripten-SDL reads:
 * buttons[9]=START, buttons[0]=A under the "standard" mapping. */
const PAD_INIT = () => {
  const mkbtn = () => ({ pressed: false, touched: false, value: 0 });
  const pad = {
    index: 0,
    id: 'SimPad (STANDARD GAMEPAD Vendor: 5353 Product: 564a)',
    connected: true,
    mapping: 'standard',
    timestamp: 0,
    buttons: Array.from({ length: 17 }, mkbtn),
    axes: [0, 0, 0, 0],
    vibrationActuator: null,
  };
  const pads = [pad, null, null, null];
  navigator.getGamepads = () => pads;
  navigator.webkitGetGamepads = navigator.getGamepads;
  window.__simpad = pad;
  window.__simpad_event = (name) => {
    let ev;
    try { ev = new GamepadEvent(name, { gamepad: pad }); }
    catch (_) {
      ev = new Event(name);
      Object.defineProperty(ev, 'gamepad', { value: pad });
    }
    window.dispatchEvent(ev);
  };
  window.__simpad_press = (i, down) => {
    pad.buttons[i].pressed = down;
    pad.buttons[i].value = down ? 1 : 0;
    pad.timestamp = performance.now();
  };
};

async function main() {
  const browser = await chromium.launch();
  /* Mobile context: touch overlay is wanted (maxTouchPoints/pointer:coarse),
   * so the hide-on-gamepad behavior is actually exercised. */
  const ctx = await browser.newContext({
    ...devices['Pixel 7'],
    hasTouch: true,
  });
  await ctx.route('**/sw.js', r => r.fulfill({ status: 404, body: '' }));
  await ctx.addInitScript(PAD_INIT);
  const page = await ctx.newPage();
  page.on('console', m => {
    const t = m.text();
    logs.push(t);
    if (/Frame \d+ complete|PAGEERROR|Aborted/i.test(t)) console.log(`[pg] ${t}`);
  });
  page.on('pageerror', e => console.log(`[pg] PAGEERROR ${e.message || e}`));

  const fail = (why) => {
    console.log(`\nGAMEPAD SIM: FAIL — ${why}`);
    console.log(logs.slice(-15).join('\n'));
    return page.screenshot({ path: 'pw_gamepad_fail.png' })
      .finally(() => browser.close()).then(() => process.exit(1));
  };

  /* Connect BEFORE navigation — the pad must exist when SDL enumerates. */
  await page.goto(`${BASE}?autostart=1`, { waitUntil: 'domcontentloaded' });
  await page.evaluate(() => window.__simpad_event('gamepadconnected'));

  /* Wait for the game to produce frames (console "Frame N complete"). */
  const t0 = Date.now();
  while (Date.now() - t0 < BOOT_MS) {
    if (logs.some(t => /Frame \d+ complete/.test(t))) break;
    await page.waitForTimeout(500);
  }
  if (!logs.some(t => /Frame \d+ complete/.test(t))) {
    return fail('no game frames within boot window');
  }
  console.log('[pg] game running — checking shell + input path');

  /* 1. Overlay must hide while the pad is connected (menu closed). */
  await page.waitForTimeout(600); /* shell refresh cadence is 250ms */
  const overlayOn = await page.evaluate(() =>
    document.getElementById('touchui').classList.contains('on'));
  if (overlayOn) return fail('touch overlay still visible with pad connected');
  console.log('[pg] overlay hidden with pad connected OK');

  /* 2. Hold START — the game-consumed N64 pad state must show it. */
  await page.evaluate(() => window.__simpad_press(9, true));
  let hold = 0;
  const t1 = Date.now();
  while (Date.now() - t1 < 10000) {
    hold = await page.evaluate(() => window.__ssb64_p1_buttons | 0);
    if ((hold & 0xFFFF) !== 0) break;
    await page.waitForTimeout(200);
  }
  if ((hold & 0xFFFF) === 0) {
    return fail('__ssb64_p1_buttons stayed 0 — input did not reach the game');
  }
  console.log(`[pg] game-consumed pad state = 0x${(hold & 0xFFFF).toString(16)}` +
    ((hold & N64_START) ? ' (START bit set)' : ` (START bit clear — LUS mapped it elsewhere)`));
  await page.evaluate(() => window.__simpad_press(9, false));

  /* Release should clear the bit. */
  await page.waitForTimeout(600);
  const released = await page.evaluate(() => window.__ssb64_p1_buttons & 0xFFFF);
  if (released !== 0) console.log(`[pg] WARN: hold still 0x${released.toString(16)} after release`);

  /* 3. Disconnect → touch overlay must come back (no menu open). */
  await page.evaluate(() => {
    window.__simpad.connected = false;
    window.__simpad_event('gamepaddisconnected');
  });
  await page.waitForTimeout(600);
  const overlayBack = await page.evaluate(() =>
    document.getElementById('touchui').classList.contains('on'));
  if (!overlayBack) return fail('touch overlay did not return after disconnect');
  console.log('[pg] overlay restored after disconnect OK');

  console.log('\nGAMEPAD SIM: PASS');
  await browser.close();
}

main().catch(e => { console.error(e); process.exit(1); });
