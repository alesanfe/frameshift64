// @ts-check
// board_page.mjs — self-contained public portal page served at GET / by
// both the worker and node-lobby. Pure template: no storage access — the
// page fetches the service's own JSON endpoints (/leaderboard, /history,
// /tournament, /stats) from the browser, so it can never leak server
// internals and the same file works on any adapter that exposes the API.
//
// Kept as a single template literal: no build step, no assets — the lobby
// is a tiny service and a CDN-framework page would be heavier than the
// whole rest of the deployment.

export const BOARD_PAGE_HTML = `<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>BattleShip Netplay</title>
<style>
  :root { color-scheme: dark; }
  * { box-sizing: border-box; }
  body { margin: 0; font: 14px/1.5 system-ui, sans-serif;
         background: #0b0e14; color: #d7dde8; }
  header { padding: 28px 32px 12px; }
  h1 { margin: 0; font-size: 22px; letter-spacing: .04em; }
  h1 small { color: #7f8ba3; font-weight: 400; font-size: 13px; }
  main { padding: 0 32px 40px; display: grid; gap: 24px;
         grid-template-columns: repeat(auto-fit, minmax(340px, 1fr)); }
  section { background: #131826; border: 1px solid #232a3d;
            border-radius: 10px; padding: 16px 18px; }
  h2 { margin: 0 0 10px; font-size: 13px; text-transform: uppercase;
       letter-spacing: .12em; color: #8fa1c7; }
  table { width: 100%; border-collapse: collapse; font-variant-numeric:
          tabular-nums; }
  td, th { padding: 5px 8px; text-align: left; white-space: nowrap; }
  th { color: #6b7896; font-size: 11px; text-transform: uppercase; }
  tbody tr { border-top: 1px solid #1d2436; }
  .flag { font-size: 11px; padding: 1px 6px; border-radius: 4px;
          margin-left: 6px; }
  .flag.ban { background: #4c1d24; color: #ff8496; }
  .flag.shd { background: #3d3320; color: #e8c766; }
  .flag.sus { background: #1d3a4c; color: #7ec8ff; }
  .flag.mut { background: #2a2440; color: #b9a8ff; }
  .num { text-align: right; }
  .stats { display: flex; gap: 22px; flex-wrap: wrap; }
  .stat b { font-size: 20px; display: block; }
  .stat span { color: #6b7896; font-size: 12px; }
  .muted { color: #6b7896; }
  .win { color: #7fd88f; }
  .err { color: #ff8496; padding: 12px 0; }
</style>
</head>
<body>
<header>
  <h1>BattleShip <small>netplay leaderboard</small></h1>
</header>
<main>
  <section style="grid-column:1/-1">
    <h2>Service</h2>
    <div class="stats" id="stats"><span class="muted">loading…</span></div>
  </section>
  <section>
    <h2>Leaderboard</h2>
    <table id="board"><thead><tr>
      <th>#</th><th>Player</th><th class="num">Rating</th>
      <th class="num">W</th><th class="num">L</th><th></th>
    </tr></thead><tbody></tbody></table>
  </section>
  <section>
    <h2>Recent verified matches</h2>
    <table id="hist"><thead><tr>
      <th>Winner</th><th>Loser</th><th>When</th>
    </tr></thead><tbody></tbody></table>
  </section>
  <section style="grid-column:1/-1">
    <h2>Tournaments</h2>
    <table id="tour"><thead><tr>
      <th>Name</th><th class="num">Size</th><th>Status</th><th>Winner</th>
    </tr></thead><tbody></tbody></table>
  </section>
  <section>
    <h2>Shared replays</h2>
    <table id="reps"><thead><tr>
      <th>Replay</th><th>Shared by</th><th>When</th>
    </tr></thead><tbody></tbody></table>
  </section>
</main>
<script>
const esc = (s) => String(s).replace(/[&<>"']/g,
  c => ({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));
const rel = (iso) => {
  const s = (Date.now() - Date.parse(iso)) / 1000;
  if (!(s >= 0)) return "";
  if (s < 60) return Math.floor(s) + "s ago";
  if (s < 3600) return Math.floor(s / 60) + "m ago";
  if (s < 86400) return Math.floor(s / 3600) + "h ago";
  return Math.floor(s / 86400) + "d ago";
};
const flags = (e) =>
  (e.banned ? '<span class="flag ban">banned</span>' : "") +
  (e.shadow ? '<span class="flag shd">shadow</span>' : "") +
  (e.muted ? '<span class="flag mut">muted</span>' : "") +
  (e.suspicious ? '<span class="flag sus">suspicious</span>' : "");
async function load() {
  try {
    const [board, hist, tour, stats, reps] = await Promise.all([
      fetch("/leaderboard").then(r => r.json()),
      fetch("/history").then(r => r.json()),
      fetch("/tournament").then(r => r.json()),
      fetch("/stats").then(r => r.json()),
      fetch("/replays").then(r => r.json()),
    ]);
    document.querySelector("#stats").innerHTML = [
      ["entries", stats.leaderboard_entries],
      ["verified matches", stats.verified_matches],
      ["replays shared", stats.shared_replays],
      ["banned", stats.banned], ["shadow", stats.shadowed],
      ["muted", stats.muted], ["builds", stats.distinct_builds],
    ].map(([k, v]) =>
      '<div class="stat"><b>' + (v ?? 0) + '</b><span>' + k + '</span></div>'
    ).join("");
    document.querySelector("#board tbody").innerHTML =
      (board.entries || []).map((e, i) =>
        "<tr><td>" + (i + 1) + "</td><td>" + esc(e.name) + flags(e) +
        '</td><td class="num">' + e.rating +
        '</td><td class="num win">' + e.wins +
        '</td><td class="num">' + e.losses + "</td><td></td></tr>"
      ).join("") || '<tr><td colspan="6" class="muted">empty</td></tr>';
    document.querySelector("#hist tbody").innerHTML =
      (hist.matches || []).map(m =>
        m.draw
          ? "<tr><td>" + esc(m.a) + " ↔ " + esc(m.b) +
            '</td><td class="muted">draw</td><td>' + rel(m.at) + "</td></tr>"
          : "<tr><td class=\\"win\\">" + esc(m.w) + "</td><td>" +
            esc(m.l) + "</td><td>" + rel(m.at) + "</td></tr>"
      ).join("") || '<tr><td colspan="3" class="muted">none yet</td></tr>';
    document.querySelector("#tour tbody").innerHTML =
      (tour.brackets || []).map(b =>
        "<tr><td>" + esc(b.name) + '</td><td class="num">' + b.size +
        "</td><td>" + esc(b.status) + "</td><td>" +
        (b.winner ? '<span class="win">' + esc(b.winner) + "</span>" : "—") +
        "</td></tr>"
      ).join("") || '<tr><td colspan="4" class="muted">none</td></tr>';
    document.querySelector("#reps tbody").innerHTML =
      (reps.replays || []).map(rp =>
        '<tr><td><a href="/replay/' + esc(rp.id) + '">' + esc(rp.id) +
        "</a></td><td>" + esc(rp.by || "—") + "</td><td>" + rel(rp.at) +
        "</td></tr>"
      ).join("") || '<tr><td colspan="3" class="muted">none shared yet</td></tr>';
  } catch (e) {
    document.querySelector("#stats").innerHTML =
      '<span class="err">API unreachable</span>';
  }
}
load(); setInterval(load, 30000);
</script>
</body>
</html>`;
