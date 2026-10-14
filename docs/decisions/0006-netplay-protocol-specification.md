# ADR-0006: Netplay protocol documented as normative spec

## Status

Accepted (2026-10)

## Context

The SSNP/lobby protocol evolved in code; knowledge lived in comments,
audits, and bug writeups. Changes risked silently breaking compat because
there was no single source of truth for "what is on the wire".

## Decision

`docs/netplay/protocol/` is the normative specification:

- `specification.md` — wire framing, header, sizes, checksum
- `messages.md` — fixed message-ID catalog (IDs never reused)
- `lifecycle.md` — session state machine + per-state valid traffic
- `topology.md` — direct/star-relay/spectator shapes
- `security-model.md` — trust boundaries and what is enforced
- `error-codes.md` — lobby reject reasons (additive-only)
- `compatibility.md` — change rules + test mapping
- `examples/` — worked packet decode

`decomp/src/sys/netpeer.c` + `services/netplay-lobby/` are the reference
implementations; on disagreement the code wins and the doc is fixed in the
same PR.

## Consequences

- A PR touching netplay packet format must update these docs or justify
  exemption (enforced socially; CI lint pending).
- The spec enables a future independent implementation and gives the
  fuzz/mutation harnesses an oracle.
