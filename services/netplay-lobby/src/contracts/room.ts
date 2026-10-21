/**
 * contracts/room.ts — room admission and socket bookkeeping shapes.
 *
 * Mirrors src/domain/room.mjs (LobbyRoom.admit + the sockets map
 * entries). Type-only — the .mjs implementation stays the runtime
 * authority; drift is caught when a file opts into `// @ts-check`.
 */

import type { Reject } from "./errors.js";

/** Querystring role a socket asks for on /ws. "queue" is matchmaking:
 * the room resolves it to host|join before admission. */
export type RoomRole = "host" | "join" | "spec" | "queue";

/** Protocol dialects. v1 relays whole sessions; v2 is per-peer. */
export type ProtocolVersion = 1 | 2;

/** The duck-typed transport the domain is written against — both `ws`
 * (Node adapter) and the Workers server-side WebSocket satisfy it. The
 * domain never sees the real socket class. */
export interface SocketLike {
  send(data: string | ArrayBuffer | Uint8Array | ArrayBufferView): void;
  close(code?: number, reason?: string): void;
}

/** Relay endpoint a player publishes for hole punching (truncated by
 * the room to 64 chars each). "-" is the not-published sentinel. */
export interface PeerEndpoint {
  public: string;
  local: string;
}

/** Per-socket rate window (see RATE_WINDOW_MS in protocol.mjs). */
export interface RateWindow {
  win: number;
  text: number;
  bin: number;
}

/** Per-socket bookkeeping stored in LobbyRoom.sockets. `kind:"up"` is a
 * send-only uplink bound to its owner's control socket; the other kinds
 * are real participant/spectator connections. */
export interface SocketInfo {
  kind: "host" | "join" | "spec" | "up";
  version: number;
  slot: number;                   // 0..3 players, 4..7 spectators; uplinks carry the owner's slot
  endpoint: PeerEndpoint | null;  // published for hole punching (players only)
  relay: boolean;                 // this socket's path goes over the WebSocket
  upkey?: string;                 // per-connection uplink secret (v2)
  owner?: SocketLike | null;      // uplink only: the owner control socket
  rate?: RateWindow;
  endpoints?: number;             // endpoint-update count, capped per connection
  lastSeen?: number;
  ready?: boolean;                // v2 ready-check flag, rides the roster
}

/** What the adapter hands admit() — parsed from the /ws query. */
export interface AdmitParams {
  role: RoomRole | string | null;
  version: ProtocolVersion | number;
  isUplink: boolean;
  uplinkSlot: number;
  uplinkKey?: string | null;
}

/** Follow-up the adapter runs after a successful attach. */
export type AdmitAfter = "introduceV1" | "broadcastRoster";

/** admit() outcome: the adapter acts on `kind`, never inspects fields
 * blindly. */
export type AdmitResult =
  | Reject
  | {
      kind: "uplink";
      info: SocketInfo;
      replaced: SocketLike[];   // stale uplink sockets the adapter must close
    }
  | {
      kind: "attach";
      info: SocketInfo;
      welcome: Record<string, unknown>;  // sent as JSON right after attach
      after?: AdmitAfter;                // broadcast follow-up (absent on some paths)
      closeAll?: boolean;                // v1 host reclaim: close every socket first
    };

/** One row of the roster broadcast — full for players, redacted for
 * spectators (AUD-46: spec rosters carry no endpoints). */
export interface RosterEntry {
  slot: number;
  role: SocketInfo["kind"];
  public: string;
  local: string;
  ready: boolean;
}
