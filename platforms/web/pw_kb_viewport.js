/* E2E soft-keyboard viewport simulation for the web build.
 *
 * A physical phone's OS keyboard can't be simulated — but what it DOES
 * to the page can: with `interactive-widget=resizes-content` in the
 * viewport meta, Chrome shrinks the layout viewport when the keyboard
 * opens. We reproduce exactly that with CDP device-metrics override,
 * then assert the bottom-fixed #kbproxy stays fully visible and keeps
 * typing into the ImGui field — i.e. the user can still see what they
 * type on a real phone.
 *
 * Usage: node pw_kb_viewport.js
 *   (serve the build with: python platforms/web/serve.py out/build/web-wasm 8931)
 */
const { chromium, devices } = require('playwright');

const BASE = 'http://localhost:8931/BattleShip.html';
const BOOT_MS = 120000;
const logs = [];

async function main() {
  const browser = await chromium.launch();
  const ctx = await browser.newContext({
    ...devices['Pixel 7'],
    hasTouch: true,
  });
  await ctx.route('**/sw.js', r => r.fulfill({ status: 404, body: '' }));
  const page = await ctx.newPage();
  const cdp = await ctx.newCDPSession(page);
  page.on('console', m => {
    const t = m.text();
    logs.push(t);
    if (/Frame \d+ complete|PAGEERROR|Aborted/i.test(t)) console.log(`[pg] ${t}`);
  });
  page.on('pageerror', e => console.log(`[pg] PAGEERROR ${e.message || e}`));

  const fail = (why) => {
    console.log(`\nKB VIEWPORT: FAIL — ${why}`);
    console.log(logs.slice(-15).join('\n'));
    return page.screenshot({ path: 'pw_kbviewport_fail.png' })
      .finally(() => browser.close()).then(() => process.exit(1));
  };

  await page.goto(`${BASE}?autostart=1`, { waitUntil: 'domcontentloaded' });

  const t0 = Date.now();
  while (Date.now() - t0 < BOOT_MS) {
    if (logs.some(t => /Frame \d+ complete/.test(t))) break;
    await page.waitForTimeout(500);
  }
  if (!logs.some(t => /Frame \d+ complete/.test(t))) {
    return fail('no game frames within boot window');
  }

  /* Activate the kbproxy exactly like the product does: flag the want. */
  await page.evaluate(() => { window.__ssb64_wants_keyboard = 1; });
  await page.waitForTimeout(400); /* watcher cadence 120ms */
  const proxy = page.locator('#kbproxy');
  if (!(await proxy.isVisible())) return fail('kbproxy did not appear');
  if ((await page.evaluate(() => document.activeElement.id)) !== 'kbproxy') {
    return fail('kbproxy not focused');
  }
  console.log('[pg] kbproxy visible + focused');

  /* Emulate the OS keyboard: shrink the layout viewport the way
   * interactive-widget=resizes-content does (~45% of screen on phones). */
  const fullH = await page.evaluate(() => window.innerHeight);
  const kbH = Math.round(fullH * 0.55); /* phone keyboards eat ~40-50% */
  await cdp.send('Emulation.setDeviceMetricsOverride', {
    width: await page.evaluate(() => window.innerWidth),
    height: fullH - kbH,
    deviceScaleFactor: 3,
    mobile: true,
  });
  await page.waitForTimeout(300);

  /* The bottom-fixed proxy must still be entirely inside the (shrunken)
   * viewport — otherwise the OS keyboard would cover it on a real phone. */
  const rect = await proxy.boundingBox();
  const innerH = await page.evaluate(() => window.innerHeight);
  const bottom = rect ? rect.y + rect.height : NaN;
  if (!rect || bottom > innerH + 1) {
    return fail(`kbproxy covered: bottom=${bottom} innerH=${innerH}`);
  }
  console.log(`[pg] kbproxy fully visible above shrunken viewport ` +
              `(bottom=${Math.round(bottom)} <= ${innerH})`);

  /* Typing through the proxy must still reach SDL/game. */
  await proxy.pressSequentially('NET42', { delay: 40 });
  await page.waitForTimeout(400);
  await page.screenshot({ path: 'pw_kbviewport.png' });

  /* Restore viewport, deactivate, confirm cleanup. */
  await cdp.send('Emulation.clearDeviceMetricsOverride', {});
  await page.evaluate(() => { window.__ssb64_wants_keyboard = 0; });
  await page.waitForTimeout(400);
  if (await proxy.isVisible()) return fail('kbproxy stayed visible after flag cleared');

  console.log('\nKB VIEWPORT: PASS');
  await browser.close();
}

main().catch(e => { console.error(e); process.exit(1); });
