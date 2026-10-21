// @ts-check
// LobbyRoom — the room domain, shared by both adapters.
//
// Transport-free: every socket is a duck-typed { send(data), close(code,
// reason) } — both `ws` (Node) and the Workers server-side WebSocket
// satisfy that. Adapter responsibilities that stay OUT of this class:
//
//   index.js (Durable Object)  — WebSocketPair/101 plumbing, reject(),
//                                addEventListener wiring, Blob→ArrayBuffer
//                                normalization + per-socket order queue,
//                                setAlarm idle sweeps.
//   node-lobby.mjs             — http/ws upgrade plumbing, the `rooms` Map
//                                + gc, ws.on wiring, setInterval sweeps.
//
// Two hooks exist for the adapters:
//   room.onSocketActivity() — fired on every inbound message (DO uses it
//                             to re-arm the alarm; Node leaves it a no-op).
//   room.maybeDestroy()     — fired at the end of onClose; Node deletes
//                             the room from `rooms` when empty, the DO
//                             relies on isolate eviction instead.
//
// `bytes` passed to handleBinary must be a Uint8Array-ish view (Buffer on
// Node works — same indexing/length/subarray surface).

import {
  MAX_BINARY_BYTES, MAX_TEXT_BYTES, MAX_PLAYERS_V2, MAX_SPECS,
  RATE_WINDOW_MS, MAX_TEXT_PER_SEC, MAX_BINARY_PER_SEC,
  MAX_ENDPOINT_UPDATES, JOIN_WINDOW_MS, MAX_JOINS_PER_WINDOW,
  SOCKET_IDLE_MS, SPEC_MAGIC, newUpkey,
} from "./protocol.mjs";

/** @typedef {import("../contracts/room.js").SocketLike} SocketLike */
/** @typedef {import("../contracts/room.js").SocketInfo} SocketInfo */
/** @typedef {import("../contracts/room.js").AdmitParams} AdmitParams */
/** @typedef {import("../contracts/room.js").AdmitResult} AdmitResult */
/** @typedef {import("../contracts/room.js").PeerEndpoint} PeerEndpoint */

export class LobbyRoom {
  /** Fired on every inbound message — the DO re-arms its alarm; Node no-ops. */
  /** @type {(() => void) | undefined} */ onSocketActivity;
  /** Fired at the end of onClose — Node deletes empty rooms; the DO no-ops. */
  /** @type {(() => void) | undefined} */ maybeDestroy;

  /** @param {string} code */
  constructor(code) {
    this.code = code;
    /** @type {Map<SocketLike, SocketInfo>} */
    this.sockets = new Map();
    this.version = 0;          // 0 = empty room, else 1|2 fixed by the host
    this.relayV1 = false;      // v1: whole session relayed
    /** @type {number[]} */
    this.recentJoins = [];     // timestamps, bounded join/uplink rate
  }

  /* ---- socket bookkeeping ---- */

  /** @param {SocketLike} ws @returns {SocketInfo | undefined} */
  info(ws) { return this.sockets.get(ws); }

  /** @param {string} role @returns {SocketLike | null} */
  socketV1(role) {
    for (const [ws, i] of this.sockets) {
      if (i.kind !== "up" && i.version === 1 && i.kind === role) return ws;
    }
    return null;
  }

  /** @param {number} slot @returns {SocketLike | null} */
  socketForSlot(slot) {
    for (const [ws, i] of this.sockets) {
      if (i.kind !== "up" && i.version === 2 && i.slot === slot) return ws;
    }
    return null;
  }

  freeSlot() {
    for (let s = 1; s < MAX_PLAYERS_V2; s++) {
      if (!this.socketForSlot(s)) return s;
    }
    return -1;
  }

  freeSpecSlot() {
    for (let s = MAX_PLAYERS_V2; s < MAX_PLAYERS_V2 + MAX_SPECS; s++) {
      if (!this.socketForSlot(s)) return s;
    }
    return -1;
  }

  /** @param {SocketLike} ws @param {Record<string, unknown>} obj */
  sendJson(ws, obj) {
    try { ws.send(JSON.stringify(obj)); } catch { /* socket already gone */ }
  }

  /* Register a socket — adapter still owns event wiring (ws.on vs
   * addEventListener). Returns the info object for convenience. */
  /** @param {SocketLike} ws @param {SocketInfo} info @returns {SocketInfo} */
  attach(ws, info) {
    info.rate = { win: 0, text: 0, bin: 0 };
    info.endpoints = 0;
    info.lastSeen = Date.now();
    info.ready = false;
    this.sockets.set(ws, info);
    return info;
  }

  /* ---- rate limits ---- */

  /* Bounded join rate: the room code is the only room secret, so upgrade
   * attempts (control + uplink) cost real connections. */
  allowJoin() {
    const now = Date.now();
    this.recentJoins = this.recentJoins.filter((t) => now - t < JOIN_WINDOW_MS);
    this.recentJoins.push(now);
    return this.recentJoins.length <= MAX_JOINS_PER_WINDOW;
  }

  /* Failed uplink auth doesn't spend the join budget — an attacker
   * without the upkey shouldn't crowd out the owner's reconnect (AUD-24). */
  refundJoin() { this.recentJoins.pop(); }

  /* Per-socket token window; over the limit the socket is dropped. */
  /** @param {SocketInfo} info @param {boolean} binary */
  allowMessage(info, binary) {
    const now = Date.now();
    const r = info.rate ?? (info.rate = { win: 0, text: 0, bin: 0 });
    if (now - r.win >= RATE_WINDOW_MS) {
      r.win = now;
      r.text = 0;
      r.bin = 0;
    }
    return binary ? ++r.bin <= MAX_BINARY_PER_SEC : ++r.text <= MAX_TEXT_PER_SEC;
  }

  /* ---- admission ---- */

  /* Validate an incoming connection and return an action the adapter
   * executes. `p`: { role, version, isUplink, uplinkSlot, uplinkKey }.
   *
   * Returns:
   *   { kind:"reject", reason }                      — close with `reason`
   *   { kind:"uplink", info, replaced:[ws] }         — attach a send-only uplink
   *   { kind:"attach", info, welcome, after, closeAll }
   *     — attach, send `welcome` (JSON object), then run the follow-up:
   *       "introduceV1" | "broadcastRoster" | null; when closeAll is set,
   *       close(4002,"room-reset") every existing socket first.
   */
  /** @param {AdmitParams} p @returns {AdmitResult} */
  admit(p) {
    if (!this.allowJoin()) return { kind: "reject", reason: "rate-limited" };

    // Send-only uplink: tagged {kind:"up"} with the owner's slot, excluded
    // from the roster; inbound text applies to the owner. The &key= must
    // match the upkey the owner got in its welcome.
    if (p.isUplink) {
      const owner = (p.version === 2 && this.version === 2 &&
                     p.uplinkSlot >= 0 && p.uplinkSlot < MAX_PLAYERS_V2)
        ? this.socketForSlot(p.uplinkSlot) : null;
      const oi = owner ? this.info(owner) : null;
      if (!oi || !oi.upkey || p.uplinkKey !== oi.upkey) {
        this.refundJoin();
        return { kind: "reject", reason: "no-such-room" };
      }
      // One uplink per slot: a reconnect replaces the stale socket.
      /** @type {SocketLike[]} */
      const replaced = [];
      for (const [s, i] of this.sockets) {
        if (i.kind === "up" && i.slot === p.uplinkSlot) {
          this.sockets.delete(s);
          try { s.close(4003, "uplink-replaced"); } catch { /* gone */ }
          replaced.push(s);
        }
      }
      return { kind: "uplink", replaced,
               info: { kind: "up", version: 2, slot: p.uplinkSlot,
                       endpoint: null, relay: false, owner } };
    }

    // Matchmaking (v2): a queue connection hosts when the room has no host,
    // joins otherwise — two queued players always pair up. Quick-play rooms
    // are the well-known QUEUE<n> codes; everything downstream stays the
    // normal host/join semantics.
    if (p.role === "queue") {
      if (p.version !== 2) return { kind: "reject", reason: "version-mismatch" };
      p = { ...p, role: this.socketForSlot(0) ? "join" : "host" };
    }

    // Spectator (v2): slots 4..7, no endpoints, always relayed — the host
    // pushes every player's confirmed input stream to them.
    if (p.role === "spec") {
      if (p.version !== 2) return { kind: "reject", reason: "version-mismatch" };
      if (!this.socketForSlot(0)) return { kind: "reject", reason: "no-such-room" };
      const slot = this.freeSpecSlot();
      if (slot < 0) return { kind: "reject", reason: "room-full" };
      return { kind: "attach",
               info: { kind: "spec", version: 2, slot, endpoint: null, relay: true },
               welcome: { type: "welcome", room: this.code, role: "spec", slot },
               after: "broadcastRoster" };
    }

    if (p.role === "host") {
      const oldHost = p.version === 2 ? this.socketForSlot(0) : this.socketV1("host");
      if (oldHost) return { kind: "reject", reason: "room-in-use" };
      // Only reset a room that is empty or already on this protocol version
      // (hostless): a cross-version join on a live room must not evict it.
      let closeAll = false;
      if (this.sockets.size > 0) {
        if (this.version !== p.version) return { kind: "reject", reason: "version-mismatch" };
        closeAll = true;
      }
      this.version = p.version;
      this.relayV1 = false;

      if (p.version === 1) {
        return { kind: "attach", closeAll,
                 info: { kind: "host", version: 1, slot: 0, endpoint: null, relay: false },
                 welcome: { type: "welcome", room: this.code, role: "host" },
                 after: "introduceV1" };
      }
      const upkey = newUpkey();
      return { kind: "attach", closeAll,
               info: { kind: "host", version: 2, slot: 0, endpoint: null, relay: false, upkey },
               welcome: { type: "welcome", room: this.code, role: "host", slot: 0, upkey },
               after: "broadcastRoster" };
    }

    // role === "join"
    if (this.version !== p.version) return { kind: "reject", reason: "version-mismatch" };
    const host = p.version === 2 ? this.socketForSlot(0) : this.socketV1("host");
    if (!host) return { kind: "reject", reason: "no-such-room" };

    if (p.version === 1) {
      if (this.socketV1("join")) return { kind: "reject", reason: "room-full" };
      return { kind: "attach",
               info: { kind: "join", version: 1, slot: 1, endpoint: null, relay: false },
               welcome: { type: "welcome", room: this.code, role: "join" },
               after: "introduceV1" };
    }
    const slot = this.freeSlot();
    if (slot < 0) return { kind: "reject", reason: "room-full" };
    const upkey = newUpkey();
    return { kind: "attach",
             info: { kind: "join", version: 2, slot, endpoint: null, relay: false, upkey },
             welcome: { type: "welcome", room: this.code, role: "join", slot, upkey },
             after: "broadcastRoster" };
  }

  /* ---- v1: two players, endpoint exchange + symmetric relay ---- */

  /** @param {SocketLike} ws @returns {SocketLike | null} */
  otherV1(ws) {
    const i = this.info(ws);
    if (!i) return null;
    return this.socketV1(i.kind === "host" ? "join" : "host");
  }

  introduceV1() {
    const host = this.socketV1("host");
    const join = this.socketV1("join");
    if (!host || !join) return;
    const he = this.info(host)?.endpoint;
    const je = this.info(join)?.endpoint;
    if (!he || !je) return;
    this.sendJson(host, { type: "peer", role: "join", public: je.public, local: je.local });
    this.sendJson(join, { type: "peer", role: "host", public: he.public, local: he.local });
  }

  /* ---- v2: roster broadcast + routed relay ---- */

  /** @param {SocketLike} [exclude] */
  broadcastRoster(exclude) {
    /* Spectators receive the roster with every endpoint redacted: they are
     * relay-only by definition, so player IPs are information they don't
     * need (AUD-46). Players keep the full roster for hole punching. */
    const full = [];
    const redacted = [];
    for (const [ws, i] of this.sockets) {
      if (ws === exclude || i.kind === "up") continue;
      full.push({
        slot: i.slot,
        role: i.kind,
        public: i.endpoint?.public || "-",
        local: i.endpoint?.local || "-",
        ready: i.ready === true,
      });
      /* Spectators may see who is ready — it leaks nothing the redacted
       * roster is meant to hide (no endpoints). */
      redacted.push({ slot: i.slot, role: i.kind, public: "-", local: "-",
                      ready: i.ready === true });
    }
    const fullText = JSON.stringify({ type: "peers", players: full });
    const redactedText = JSON.stringify({ type: "peers", players: redacted });
    for (const [ws, i] of this.sockets) {
      if (ws === exclude || i.kind === "up") continue;
      try { ws.send(i.kind === "spec" ? redactedText : fullText); } catch { /* gone */ }
    }
  }

  /* ---- messages ---- */

  /* Text frame (already decoded to a string by the adapter). */
  /** @param {SocketLike} ws @param {string} text */
  handleText(ws, text) {
    const info = this.info(ws);
    if (!info) return;
    info.lastSeen = Date.now();
    this.onSocketActivity?.();

    if (!this.allowMessage(info, false)) {
      try { ws.close(4029, "rate-limit"); } catch { /* gone */ }
      return;
    }
    // Counted above even when oversized: >cap floods still spend budget.
    if (text.length > MAX_TEXT_BYTES) return;

    // Uplink sockets are send-only: their text applies to the owner socket
    // the uplink was authenticated against.
    if (info.kind === "up") {
      if (!info.owner || !this.info(info.owner)) return;
      ws = info.owner;
    }
    const wi = this.info(ws);
    if (!wi || wi.kind === "up") return;

    let msg;
    try { msg = JSON.parse(text); } catch { return; }
    switch (msg.type) {
      case "ping":
      case "keepalive":
        this.sendJson(ws, { type: "keepalive_ack" });
        break;
      case "endpoint":
        wi.endpoints = (wi.endpoints ?? 0) + 1;
        if (wi.endpoints > MAX_ENDPOINT_UPDATES) break;
        wi.endpoint = {
          public: String(msg.public || "-").slice(0, 64),
          local: String(msg.local || "-").slice(0, 64),
        };
        if (wi.version === 2) this.broadcastRoster(); else this.introduceV1();
        break;
      case "ready":
        /* Ready-check (v2): a player toggles {type:"ready",ready:0|1} and the
         * flag rides the next roster broadcast. v1 rooms have no roster, so
         * the flag is ignored there — v1 keeps its PICK handshake. The flag
         * dies with the socket; clients re-send after reconnect. */
        if (wi.version === 2) {
          wi.ready = msg.ready === true || msg.ready === 1;
          this.broadcastRoster();
        }
        break;
      case "relay":
        if (process.env.LOBBY_DEBUG) console.log(`[room ${this.code}] slot ${wi.slot} requests relay (v${wi.version})`);
        if (wi.version === 2) {
          // Per-link relay: this client's path to the host goes over the
          // WebSocket. Mark the socket and tell both ends.
          wi.relay = true;
          this.sendJson(ws, { type: "relaying" });
          const host = this.socketForSlot(0);
          if (host && host !== ws) this.sendJson(host, { type: "peer_relay", slot: wi.slot });
        } else {
          this.relayV1 = true;
          const relayText = JSON.stringify({ type: "relaying" });
          for (const [s] of this.sockets) {
            try { s.send(relayText); } catch { /* gone */ }
          }
        }
        break;
    }
  }

  /* Binary frame normalized to a Uint8Array-ish view by the adapter. */
  /** @param {SocketLike} ws @param {Uint8Array} bytes */
  handleBinary(ws, bytes) {
    const info = this.info(ws);
    if (!info) return;
    info.lastSeen = Date.now();
    this.onSocketActivity?.();

    // Count first so oversized-frame floods still spend rate budget.
    if (!this.allowMessage(info, true)) {
      try { ws.close(4029, "rate-limit"); } catch { /* gone */ }
      return;
    }
    if (bytes.length > MAX_BINARY_BYTES) return;
    // Uplinks speak with their owner control socket's identity.
    const src = (info.kind === "up" && info.owner) ? this.info(info.owner) : info;
    if (!src) return;
    if (info.version === 2) {
      // Routed: [to_slot][payload] -> [from_slot][payload]. The star is
      // enforced: clients only reach slot 0, the host only client slots,
      // and the hop must be a link the room marked as relayed.
      if (bytes.length < 2) return;
      const toSlot = bytes[0] ?? -1;
      // Spectators may only address the host, and only with an "SSBL "
      // lobby datagram (see SPEC_MAGIC).
      if (info.kind === "spec") {
        if (toSlot !== 0) return;
        if (bytes.length < 6 ||
            bytes[1] !== SPEC_MAGIC[0] || bytes[2] !== SPEC_MAGIC[1] ||
            bytes[3] !== SPEC_MAGIC[2] || bytes[4] !== SPEC_MAGIC[3] ||
            bytes[5] !== SPEC_MAGIC[4]) return;
      }
      if (toSlot === src.slot || (src.slot !== 0 && toSlot !== 0)) return;
      if (src.slot !== 0 && !src.relay) return;
      const target = this.socketForSlot(toSlot);
      if (!target || target === ws || target === info.owner) {
        if (process.env.LOBBY_DEBUG) console.log(`[room ${this.code}] drop binary: no target for slot ${toSlot} from slot ${src.slot}`);
        return;
      }
      if (src.slot === 0) {
        const ti = this.info(target);
        if (!ti || !ti.relay) {
          if (process.env.LOBBY_DEBUG) console.log(`[room ${this.code}] drop binary: target slot ${toSlot} not relay-marked`);
          return;
        }
      }
      if (process.env.LOBBY_DEBUG) console.log(`[room ${this.code}] relay ${bytes.length}B slot ${src.slot} -> ${toSlot}`);
      const framed = new Uint8Array(bytes.length);
      framed[0] = src.slot;
      framed.set(bytes.subarray(1), 1);
      try { target.send(framed); } catch { /* gone */ }
      return;
    }
    if (this.relayV1) {
      const other = this.otherV1(ws);
      if (other) { try { other.send(bytes); } catch { /* gone */ } }
    }
  }

  /* ---- lifecycle ---- */

  /* Idle reaper (AUD-23): a dead TCP peer never fires "close" — sweep
   * silent sockets so zombies stop holding slots. Uplinks inherit their
   * owner's freshness (they are legitimately send-only). */
  sweepIdle() {
    const now = Date.now();
    for (const [ws, i] of this.sockets) {
      const seen = (i.kind === "up" && i.owner)
        ? (this.info(i.owner)?.lastSeen ?? i.lastSeen)
        : i.lastSeen;
      /* undefined lastSeen -> NaN comparison in the original semantics:
       * never reaped (a socket without attach() is a bug, not idle). */
      if (typeof seen === "number" && now - seen > SOCKET_IDLE_MS) {
        try { ws.close(4004, "idle-timeout"); } catch { /* gone */ }
        /* onClose does the real bookkeeping. */
        this.onClose(ws);
      }
    }
  }

  /** @param {SocketLike} ws */
  onClose(ws) {
    const info = this.info(ws);
    if (!info) return;
    const otherV1 = this.otherV1(ws);   // resolve before removing ws
    this.sockets.delete(ws);
    try { ws.close(1000, "closed"); } catch { /* already closed */ }

    if (info.kind === "up") {
      // Uplinks carry no roster state; never let one end the room. (A stale
      // uplink can still be the last socket standing — GC anyway.)
      this.maybeDestroy?.();
      return;
    }
    // A control socket's uplinks die with it.
    for (const [s, si] of this.sockets) {
      if (si.kind === "up" && si.owner === ws) {
        this.sockets.delete(s);
        try { s.close(1000, "owner-gone"); } catch { /* gone */ }
      }
    }
    if (info.version === 2) {
      if (info.slot === 0) {
        // Host left: the room is over for everyone.
        for (const [other, oi] of this.sockets) {
          if (oi.kind !== "up") this.sendJson(other, { type: "peer_left", slot: 0 });
          try { other.close(4001, "host-left"); } catch { /* gone */ }
        }
        this.sockets.clear();
        this.version = 0;
      } else {
        for (const [other, oi] of this.sockets) {
          if (oi.kind !== "up") this.sendJson(other, { type: "peer_left", slot: info.slot });
        }
        this.broadcastRoster();
      }
      this.maybeDestroy?.();
      return;
    }
    if (otherV1) {
      this.sendJson(otherV1, { type: "peer_left" });
      if (info.kind === "host") {
        try { otherV1.close(4001, "host-left"); } catch { /* gone */ }
      }
    }
    if (this.sockets.size === 0) {
      this.version = 0;
      this.relayV1 = false;
    }
    this.maybeDestroy?.();
  }
}
