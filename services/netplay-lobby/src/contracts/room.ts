/**
 * contracts/room.ts — room admission and socket bookkeeping shapes.
 *
 * Mirrors src/domain/room.mjs (LobbyRoom.admit + the sockets map
 * entries). Type-only — the .mjs implementation stays the runtime
 * authority; drift is caught when a file opts into `// @ts-check`.
 */

import type { Reject } from "./errors.js";

/** Querystring role a socket asks for on /ws. */
export type RoomRole = "host" | "join" | "spec" | "queue";

/** Protocol dialects. v1 relays whole sessions; v2 is per-peer. */
export type ProtocolVersion = 1 | 2;

/** What the adapter hands admit() — parsed from the /ws query. */
export interface AdmitParams {
  role: RoomRole | string | null;
  version: ProtocolVersion | number;
  isUplink: boolean;
  uplinkSlot: number;
  uplinkKey: string;
}

/** Per-socket bookkeeping stored in LobbyRoom.sockets. `kind:"up"` is a
 * send-only uplink owned by a player slot; the other kinds are real
 * participant/spectator connections. */
export interface SocketInfo {
  kind: "host" | "join" | "spec" | "up";
  version: number;
  slot: number;                 // 0..3 players, 4..7 spectators, -1 uplink pre-bind
  endpoint: unknown | null;     // relay target endpoint (adapter-owned shape)
  relay: boolean;
  upkey?: string;               // per-connection uplink secret (v2)
  owner?: number;               // uplink only: owning player slot
  rate?: { win: number; text: number; bin: number };
  endpoints?: number;           // endpoint-update count, capped per connection
  lastSeen?: number;
}

/** admit() outcome: the adapter acts on `kind`, never inspects fields
 * blindly. */
export type AdmitResult =
  | Reject
  | {
      kind: "uplink";
      info: SocketInfo;
      replaced: unknown[];      // sockets the adapter must close
    }
  | {
      kind: "attach";
      info: SocketInfo;
      welcome: Record<string, unknown>;
      after: Record<string, unknown> | null;  // broadcast after attach
      closeAll: boolean;        // v1 host reclaim: close every socket first
    };
