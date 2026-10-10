#!/usr/bin/env node
// BattleShip netplay rendezvous server: room codes, UDP hole-punch
// introduction and a relay fallback. One UDP port, no dependencies.
//
//   node server/lobby-server.js [port]        (default 7000)
//
// Clients talk to it from the same UDP socket they will play on, so the
// address the server sees is the player's public (NAT-mapped) endpoint.
//
// Protocol: ASCII datagrams starting with "SSBR " (control) or "SSBX"
// (relayed payload).
//   C->S  SSBR CREATE <nonce> <local ip:port>     host asks for a room
//   S->C  SSBR ROOM <code>
//   C->S  SSBR JOIN <code> <nonce> <local ip:port>
//   S->C  SSBR PEER <role> <public ip:port> <local ip:port>   (to both)
//   C->S  SSBR PING <code>                         keep the room alive
//   C->S  SSBR RELAY <code>                        direct path failed
//   S->C  SSBR RELAYING <code>
//   C->S  SSBX<code><payload>                      forwarded to the other
//   S->C  SSBX<code><payload>                      member as-is
//   S->C  SSBR ERR <message>
'use strict';

const dgram = require('dgram');

const PORT = Number(process.argv[2] || process.env.PORT || 7000);
const ROOM_TTL_MS = 10 * 60 * 1000;         // idle rooms expire
const CODE_ALPHABET = 'ABCDEFGHJKLMNPQRSTUVWXYZ23456789';  // no 0/O/1/I

const rooms = new Map();   // code -> { host, client, relay, touched }

function endpoint(rinfo) {
  return `${rinfo.address}:${rinfo.port}`;
}

function newCode() {
  for (;;) {
    let code = '';
    for (let i = 0; i < 6; i++) {
      code += CODE_ALPHABET[Math.floor(Math.random() * CODE_ALPHABET.length)];
    }
    if (!rooms.has(code)) return code;
  }
}

const sock = dgram.createSocket('udp4');

function send(text, addr) {
  const [host, port] = splitEndpoint(addr);
  sock.send(Buffer.from(text), port, host);
}

function splitEndpoint(addr) {
  const i = addr.lastIndexOf(':');
  return [addr.slice(0, i), Number(addr.slice(i + 1))];
}

function introduce(room) {
  const h = room.host, c = room.client;
  send(`SSBR PEER host ${c.public} ${c.local}`, h.public);
  send(`SSBR PEER client ${h.public} ${h.local}`, c.public);
}

sock.on('message', (msg, rinfo) => {
  const from = endpoint(rinfo);

  // Relayed game traffic: "SSBX" + 6-char code + payload.
  if (msg.length >= 10 && msg.toString('ascii', 0, 4) === 'SSBX') {
    const room = rooms.get(msg.toString('ascii', 4, 10));
    if (!room || !room.relay || !room.client) return;
    const to = (from === room.host.public) ? room.client.public
             : (from === room.client.public) ? room.host.public : null;
    if (to) {
      const [host, port] = splitEndpoint(to);
      sock.send(msg, port, host);
      room.touched = Date.now();
    }
    return;
  }

  const parts = msg.toString('utf8').trim().split(/\s+/);
  if (parts[0] !== 'SSBR') return;

  switch (parts[1]) {
    case 'CREATE': {
      // Retransmitted CREATE from the same endpoint returns the same room.
      for (const [code, room] of rooms) {
        if (room.host.public === from && room.host.nonce === parts[2]) {
          send(`SSBR ROOM ${code}`, from);
          return;
        }
      }
      const code = newCode();
      rooms.set(code, {
        host: { public: from, local: parts[3] || from, nonce: parts[2] },
        client: null, relay: false, touched: Date.now(),
      });
      send(`SSBR ROOM ${code}`, from);
      console.log(`room ${code} created by ${from}`);
      break;
    }
    case 'JOIN': {
      const code = (parts[2] || '').toUpperCase();
      const room = rooms.get(code);
      if (!room) { send('SSBR ERR no-such-room', from); return; }
      if (room.client && room.client.public !== from) { send('SSBR ERR room-full', from); return; }
      room.client = { public: from, local: parts[4] || from, nonce: parts[3] };
      room.touched = Date.now();
      introduce(room);
      console.log(`room ${code} joined by ${from}`);
      break;
    }
    case 'PING': {
      const room = rooms.get(parts[2]);
      if (room) {
        room.touched = Date.now();
        // A host still waiting re-learns the client if the PEER was lost.
        if (room.client && (from === room.host.public || from === room.client.public)) introduce(room);
      }
      break;
    }
    case 'RELAY': {
      const room = rooms.get(parts[2]);
      if (!room || !room.client) { send('SSBR ERR no-such-room', from); return; }
      room.relay = true;
      room.touched = Date.now();
      send(`SSBR RELAYING ${parts[2]}`, room.host.public);
      send(`SSBR RELAYING ${parts[2]}`, room.client.public);
      console.log(`room ${parts[2]} switched to relay`);
      break;
    }
    default:
      break;
  }
});

setInterval(() => {
  const now = Date.now();
  for (const [code, room] of rooms) {
    if (now - room.touched > ROOM_TTL_MS) rooms.delete(code);
  }
}, 30 * 1000).unref();

sock.on('listening', () => {
  const a = sock.address();
  console.log(`BattleShip lobby server listening on udp ${a.address}:${a.port}`);
});
sock.bind(PORT);
