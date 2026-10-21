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
  a { color: #7ec8ff; text-decoration: none; }
  a:hover { text-decoration: underline; }
  a.tname { color: #d7dde8; }
  a.tname:hover { color: #7ec8ff; }
  /* Player card + bracket detail views (hash-routed: #player=, #bracket=) */
  #detail h2 { margin-bottom: 14px; }
  .pcard { display: flex; gap: 28px; flex-wrap: wrap; margin-bottom: 14px; }
  .pcard .stat b { font-size: 22px; }
  .bracket { display: flex; gap: 20px; overflow-x: auto; padding: 6px 0; }
  .bround { display: flex; flex-direction: column; gap: 8px;
            justify-content: space-around; min-width: 160px; flex-shrink: 0; }
  .bround h3 { margin: 0 0 2px; font-size: 11px; color: #6b7896;
               text-transform: uppercase; letter-spacing: .08em; }
  .pair { border: 1px solid #232a3d; border-radius: 6px; padding: 5px 9px;
          font-size: 12px; background: #0e1420; }
  .pair div { padding: 1px 0; white-space: nowrap;
              overflow: hidden; text-overflow: ellipsis; max-width: 220px; }
  .pair .pw { color: #7fd88f; font-weight: 600; }
  .pair .bye { color: #6b7896; font-style: italic; }
  .backline { margin-bottom: 10px; font-size: 12px; }
</style>
</head>
<body>
<header>
  <h1>BattleShip <small>netplay leaderboard</small></h1>
</header>
<main>
  <section id="detail" style="grid-column:1/-1; display:none">
    <div class="backline"><a href="#">← back</a></div>
    <h2 id="detailTitle"></h2>
    <div id="detailBody"></div>
  </section>
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

/* i18n — the portal strings are few enough for an inline table; the
 * browser's language picks the row, en is the fallback. Keys are fixed;
 * an untranslated locale still works because every lookup hits t. */
const LANGS = {
  en: { svc:"Service", board:"Leaderboard", recent:"Recent verified matches",
        tour:"Tournaments", tourSing:"Tournament", shared:"Shared replays",
        entries:"entries", verified:"verified matches", replays:"replays shared",
        crashes:"crash reports", banned:"banned", shadow:"shadow", muted:"muted",
        builds:"builds", player:"Player", rating:"Rating", winner:"Winner",
        loser:"Loser", when:"When", name:"Name", size:"Size", status:"Status",
        replay:"Replay", sharedBy:"Shared by", empty:"empty", noneYet:"none yet",
        none:"none", noneShared:"none shared yet", unreachable:"API unreachable",
        draw:"draw", loading:"loading…", rank:"rank", players:"players",
        round:"Round", bye:"bye", playerNF:"player not found" },
  es: { svc:"Servicio", board:"Clasificación", recent:"Partidas verificadas recientes",
        tour:"Torneos", tourSing:"Torneo", shared:"Replays compartidos",
        entries:"entradas", verified:"partidas verificadas", replays:"replays compartidos",
        crashes:"reportes de crash", banned:"baneados", shadow:"sombra", muted:"silenciados",
        builds:"builds", player:"Jugador", rating:"Rating", winner:"Ganador",
        loser:"Perdedor", when:"Cuándo", name:"Nombre", size:"Tamaño", status:"Estado",
        replay:"Replay", sharedBy:"Compartido por", empty:"vacío", noneYet:"aún ninguna",
        none:"ninguno", noneShared:"aún no hay", unreachable:"API inalcanzable",
        draw:"empate", loading:"cargando…", rank:"rango", players:"jugadores",
        round:"Ronda", bye:"bye", playerNF:"jugador no encontrado" },
  pt: { svc:"Serviço", board:"Classificação", recent:"Partidas verificadas recentes",
        tour:"Torneios", tourSing:"Torneio", shared:"Replays partilhados",
        entries:"entradas", verified:"partidas verificadas", replays:"replays partilhados",
        crashes:"relatórios de crash", banned:"banidos", shadow:"sombra", muted:"silenciados",
        builds:"builds", player:"Jogador", rating:"Rating", winner:"Vencedor",
        loser:"Perdedor", when:"Quando", name:"Nome", size:"Tamanho", status:"Estado",
        replay:"Replay", sharedBy:"Partilhado por", empty:"vazio", noneYet:"ainda nenhuma",
        none:"nenhum", noneShared:"ainda não há", unreachable:"API inacessível",
        draw:"empate", loading:"a carregar…", rank:"posto", players:"jogadores",
        round:"Ronda", bye:"bye", playerNF:"jogador não encontrado" },
  de: { svc:"Dienst", board:"Bestenliste", recent:"Letzte verifizierte Matches",
        tour:"Turniere", tourSing:"Turnier", shared:"Geteilte Replays",
        entries:"Einträge", verified:"verifizierte Matches", replays:"geteilte Replays",
        crashes:"Absturzberichte", banned:"gebannt", shadow:"Shadow", muted:"stumm",
        builds:"Builds", player:"Spieler", rating:"Wertung", winner:"Sieger",
        loser:"Verlierer", when:"Wann", name:"Name", size:"Größe", status:"Status",
        replay:"Replay", sharedBy:"Geteilt von", empty:"leer", noneYet:"noch keine",
        none:"keine", noneShared:"noch keine geteilt", unreachable:"API nicht erreichbar",
        draw:"Unentschieden", loading:"lädt…", rank:"Rang", players:"Spieler",
        round:"Runde", bye:"Freilos", playerNF:"Spieler nicht gefunden" },
  fr: { svc:"Service", board:"Classement", recent:"Matchs vérifiés récents",
        tour:"Tournois", tourSing:"Tournoi", shared:"Replays partagés",
        entries:"entrées", verified:"matchs vérifiés", replays:"replays partagés",
        crashes:"rapports de crash", banned:"bannis", shadow:"ombre", muted:"muets",
        builds:"builds", player:"Joueur", rating:"Cote", winner:"Vainqueur",
        loser:"Perdant", when:"Quand", name:"Nom", size:"Taille", status:"Statut",
        replay:"Replay", sharedBy:"Partagé par", empty:"vide", noneYet:"aucun pour l'instant",
        none:"aucun", noneShared:"aucun partagé", unreachable:"API inaccessible",
        draw:"match nul", loading:"chargement…", rank:"rang", players:"joueurs",
        round:"Manche", bye:"bye", playerNF:"joueur introuvable" },
};
const t = LANGS[(navigator.language || 'en').slice(0, 2).toLowerCase()] || LANGS.en;

const rel = (iso) => {
  const s = (Date.now() - Date.parse(iso)) / 1000;
  if (!(s >= 0)) return "";
  if (s < 60) return Math.floor(s) + "s";
  if (s < 3600) return Math.floor(s / 60) + "m";
  if (s < 86400) return Math.floor(s / 3600) + "h";
  return Math.floor(s / 86400) + "d";
};
const flags = (e) =>
  (e.banned ? '<span class="flag ban">' + t.banned + '</span>' : "") +
  (e.shadow ? '<span class="flag shd">' + t.shadow + '</span>' : "") +
  (e.muted ? '<span class="flag mut">' + t.muted + '</span>' : "") +
  (e.suspicious ? '<span class="flag sus">suspicious</span>' : "");
/* Player names are deep links into the card view — encodeURIComponent
 * keeps weird-but-legal names (sanitized server-side anyway) intact. */
const pname = (n) =>
  '<a class="tname" href="#player=' + encodeURIComponent(n) + '">' + esc(n) + '</a>';

/* Static chrome gets its strings once at boot — the tables are re-owned
 * by load(), but headings and header rows are fixed markup. */
const secs = document.querySelectorAll('main > section');
[t.svc, t.board, t.recent, t.tour, t.shared].forEach((s, i) => {
  const h = secs[i + 1] && secs[i + 1].querySelector('h2');
  if (h) h.textContent = s;
});
document.querySelector('#board thead tr').innerHTML =
  '<th>#</th><th>' + t.player + '</th><th class="num">' + t.rating +
  '</th><th class="num">W</th><th class="num">L</th><th></th>';
document.querySelector('#hist thead tr').innerHTML =
  '<th>' + t.winner + '</th><th>' + t.loser + '</th><th>' + t.when + '</th>';
document.querySelector('#tour thead tr').innerHTML =
  '<th>' + t.name + '</th><th class="num">' + t.size + '</th><th>' +
  t.status + '</th><th>' + t.winner + '</th>';
document.querySelector('#reps thead tr').innerHTML =
  '<th>' + t.replay + '</th><th>' + t.sharedBy + '</th><th>' + t.when + '</th>';

/* Keys the main stats row claims explicitly — anything else numeric the
 * deployment injects via statsExtra (rooms live, queue depth, …) still
 * shows up under its raw key name. */
const KNOWN_STATS = new Set(['leaderboard_entries', 'verified_matches',
  'shared_replays', 'crash_reports', 'pending_results', 'banned',
  'shadowed', 'muted', 'distinct_builds', 'mirror_reports']);

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
      [t.entries, stats.leaderboard_entries],
      [t.verified, stats.verified_matches],
      [t.replays, stats.shared_replays],
      [t.crashes, stats.crash_reports],
      [t.banned, stats.banned], [t.shadow, stats.shadowed],
      [t.muted, stats.muted], [t.builds, stats.distinct_builds],
      ...Object.keys(stats)
        .filter(k => !KNOWN_STATS.has(k) && typeof stats[k] === 'number')
        .map(k => [k.replace(/_/g, ' '), stats[k]]),
    ].map(([k, v]) =>
      '<div class="stat"><b>' + (v ?? 0) + '</b><span>' + esc(k) + '</span></div>'
    ).join("");
    document.querySelector("#board tbody").innerHTML =
      (board.entries || []).map((e, i) =>
        "<tr><td>" + (i + 1) + "</td><td>" + pname(e.name) + flags(e) +
        '</td><td class="num">' + e.rating +
        '</td><td class="num win">' + e.wins +
        '</td><td class="num">' + e.losses + "</td><td></td></tr>"
      ).join("") || '<tr><td colspan="6" class="muted">' + t.empty + '</td></tr>';
    document.querySelector("#hist tbody").innerHTML =
      (hist.matches || []).map(m =>
        m.draw
          ? "<tr><td>" + pname(m.a) + " ↔ " + pname(m.b) +
            '</td><td class="muted">' + t.draw + '</td><td>' + rel(m.at) + "</td></tr>"
          : '<tr><td class="win">' + pname(m.w) + "</td><td>" +
            pname(m.l) + "</td><td>" + rel(m.at) + "</td></tr>"
      ).join("") || '<tr><td colspan="3" class="muted">' + t.noneYet + '</td></tr>';
    document.querySelector("#tour tbody").innerHTML =
      (tour.brackets || []).map(b =>
        '<tr><td><a class="tname" href="#bracket=' + encodeURIComponent(b.id) +
        '">' + esc(b.name) + '</a></td><td class="num">' + b.size +
        "</td><td>" + esc(b.status) + "</td><td>" +
        (b.winner ? '<span class="win">' + esc(b.winner) + "</span>" : "—") +
        "</td></tr>"
      ).join("") || '<tr><td colspan="4" class="muted">' + t.none + '</td></tr>';
    document.querySelector("#reps tbody").innerHTML =
      (reps.replays || []).map(rp =>
        '<tr><td><a href="/replay/' + esc(rp.id) + '">' + esc(rp.id) +
        "</a></td><td>" + esc(rp.by || "—") + "</td><td>" + rel(rp.at) +
        "</td></tr>"
      ).join("") || '<tr><td colspan="3" class="muted">' + t.noneShared + '</td></tr>';
  } catch (e) {
    document.querySelector("#stats").innerHTML =
      '<span class="err">' + t.unreachable + '</span>';
  }
}

/* ── Detail views (hash routes) ────────────────────────────────────────
 * #player=<name> → /player card; #bracket=<id> → /tournament?id= tree.
 * The dashboard tables keep refreshing underneath; back = "#". */
const detail = document.querySelector('#detail');
const detailTitle = document.querySelector('#detailTitle');
const detailBody = document.querySelector('#detailBody');

function renderPlayer(c) {
  const rows = (c.matches || []).slice(0, 10).map(m =>
    m.draw
      ? '<tr><td>' + pname(m.a) + ' ↔ ' + pname(m.b) +
        '</td><td class="muted">' + t.draw + '</td><td>' + rel(m.at) + '</td></tr>'
      : '<tr><td class="win">' + pname(m.w) + '</td><td>' + pname(m.l) +
        '</td><td>' + rel(m.at) + '</td></tr>'
  ).join('') || '<tr><td colspan="3" class="muted">' + t.noneYet + '</td></tr>';
  return '<div class="pcard">' + [
      [t.rank, '#' + (c.rank || '—')], [t.rating, c.rating],
      ['W', c.wins], ['L', c.losses],
    ].map(([k, v]) =>
      '<div class="stat"><b>' + esc(v) + '</b><span>' + esc(k) + '</span></div>'
    ).join('') + '</div>' + flags(c) +
    '<table><thead><tr><th>' + t.winner + '</th><th>' + t.loser + '</th><th>' +
    t.when + '</th></tr></thead><tbody>' + rows + '</tbody></table>';
}

function renderBracket(b) {
  const head = '<div class="pcard">' + [
      [t.status, b.status], [t.size, b.size],
      [t.players, (b.players || []).length], [t.winner, b.winner || '—'],
    ].map(([k, v]) =>
      '<div class="stat"><b>' + esc(v) + '</b><span>' + esc(k) + '</span></div>'
    ).join('') + '</div>';
  const side = (p, name) => name == null
    ? '<div class="bye">' + t.bye + '</div>'
    : '<div class="' + (p.winner === name ? 'pw' : '') + '">' + esc(name) +
      (p.winner === name ? ' ✓' : '') + '</div>';
  const rounds = (b.rounds || []).map((round, i) =>
    '<div class="bround"><h3>' + t.round + ' ' + (i + 1) + '</h3>' +
    round.map(p => '<div class="pair">' + side(p, p.a) + side(p, p.b) + '</div>')
         .join('') + '</div>'
  ).join('');
  return head + '<div class="bracket">' +
    (rounds || '<span class="muted">' + t.noneYet + '</span>') + '</div>';
}

async function route() {
  const h = location.hash || '';
  try {
    if (h.startsWith('#player=')) {
      const name = decodeURIComponent(h.slice(8));
      detail.style.display = '';
      detailTitle.textContent = t.player + ' — ' + name;
      detailBody.innerHTML = '<span class="muted">' + t.loading + '</span>';
      const r = await fetch('/player?name=' + encodeURIComponent(name));
      detailBody.innerHTML = r.ok
        ? renderPlayer(await r.json())
        : '<span class="err">' + t.playerNF + '</span>';
    } else if (h.startsWith('#bracket=')) {
      const id = decodeURIComponent(h.slice(9));
      detail.style.display = '';
      detailTitle.textContent = t.tourSing;
      detailBody.innerHTML = '<span class="muted">' + t.loading + '</span>';
      const r = await fetch('/tournament?id=' + encodeURIComponent(id));
      detailBody.innerHTML = r.ok
        ? renderBracket(await r.json())
        : '<span class="err">' + t.unreachable + '</span>';
    } else {
      detail.style.display = 'none';
    }
  } catch (e) {
    detailBody.innerHTML = '<span class="err">' + t.unreachable + '</span>';
  }
}
window.addEventListener('hashchange', route);
route();
load(); setInterval(load, 30000);
</script>
</body>
</html>`;
