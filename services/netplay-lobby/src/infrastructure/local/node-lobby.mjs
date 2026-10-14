// node-lobby — local mirror of the Cloudflare Worker lobby, for tests
// and LAN rigs. This file is ONLY the Node adapter: all room semantics
// (admission, routing, rate limits, roster, teardown) live in
// src/domain/ so they cannot drift from the deployed worker.
//
// Usage: node src/node-lobby.mjs [port]

import http from "node:http";
import { WebSocketServer } from "ws";
import {
  CODE_RE, VALID_ROLES, IDLE_SWEEP_MS, makeJoinLimiter,
} from "../../domain/protocol.mjs";
import { LobbyRoom } from "../../domain/room.mjs";

const rooms = new Map();    // code -> LobbyRoom
const allowIpJoin = makeJoinLimiter();  // cross-code scan throttle (AUD-25)

const port = parseInt(process.argv[2] || "8799", 10);
const server = http.createServer((req, res) => {
  if (req.url === "/health") {
    res.writeHead(200, { "content-type": "application/json" });
    res.end(JSON.stringify({ ok: true, service: "smash-netplay" }));
    return;
  }
  res.writeHead(404);
  res.end("not found");
});
const wss = new WebSocketServer({ noServer: true });
server.on("upgrade", (req, socket, head) => {
  wss.handleUpgrade(req, socket, head, (ws) => wss.emit("connection", ws, req));
});
server.listen(port);

/* Wire a socket into a room: registration in domain + event plumbing here. */
function attach(room, ws, info) {
  room.attach(ws, info);
  ws.on("message", (data, isBinary) => {
    if (isBinary) {
      // Buffer is Uint8Array-compatible — domain works on the same surface.
      room.handleBinary(ws, data);
    } else {
      room.handleText(ws, data.toString());
    }
  });
  ws.on("close", () => room.onClose(ws));
  ws.on("error", () => room.onClose(ws));
}

/* Delete the room once it holds no sockets — failed joins and closed
 * sessions must not accumulate empty LobbyRoom objects. */
function gc(room) {
  if (room.sockets.size === 0) rooms.delete(room.code);
}

wss.on("connection", (ws, req) => {
  const url = new URL(req.url, "http://localhost");
  if (url.pathname !== "/ws") { ws.close(); return; }
  const code = (url.searchParams.get("room") || "").toUpperCase();
  const role = url.searchParams.get("role");
  const version = url.searchParams.get("v") === "2" ? 2 : 1;
  const send = (obj) => { try { ws.send(JSON.stringify(obj)); } catch { /* gone */ } };
  const fail = (closeCode, reason) => {
    send({ type: "error", reason });
    ws.close(closeCode, reason);
  };

  if (!CODE_RE.test(code) || !VALID_ROLES.has(role)) {
    fail(4000, "bad room or role");
    return;
  }
  // The no-such-room / room-full distinction is a room-existence oracle —
  // bound attempts per client IP.
  if (!allowIpJoin(req.socket.remoteAddress || "unknown")) {
    fail(4029, "rate-limited");
    return;
  }

  let room = rooms.get(code);
  // Only a host mints a room: joins and uplinks to a nonexistent room fail
  // fast instead of accumulating empty objects in the map.
  const isUplink = url.searchParams.get("up") === "1";
  if (!room && (role !== "host" || isUplink)) {
    fail(4000, "no-such-room");
    return;
  }
  if (!room) {
    room = new LobbyRoom(code);
    room.maybeDestroy = () => gc(room);
    rooms.set(code, room);
  }

  const action = room.admit({
    role, version, isUplink,
    uplinkSlot: parseInt(url.searchParams.get("slot") || "-1", 10),
    uplinkKey: url.searchParams.get("key") || "",
  });

  if (action.kind === "reject") {
    fail(action.reason === "rate-limited" ? 4029 : 4000, action.reason);
    gc(room);
    return;
  }
  if (action.kind === "uplink") {
    attach(room, ws, action.info);
    return;
  }

  // kind === "attach": optional room-reset, then welcome + follow-up.
  if (action.closeAll) {
    for (const [s] of room.sockets) {
      try { s.close(4002, "room-reset"); } catch { /* gone */ }
    }
    room.sockets.clear();
  }
  attach(room, ws, action.info);
  send(action.welcome);
  if (action.after === "introduceV1") room.introduceV1();
  else if (action.after === "broadcastRoster") room.broadcastRoster();
});

// Idle sweep across all rooms (client keepalive cadence is 20 s; 90 s of
// silence means a dead peer that will never emit "close").
setInterval(() => {
  for (const r of rooms.values()) r.sweepIdle();
}, IDLE_SWEEP_MS).unref();

console.log(`node-lobby ws listening on :${port}`);
