# Incident response

## Reporting

Security issues go through GitHub private vulnerability reporting on the
repository (`SECURITY.md`). Do not open public issues for exploitable
problems.

## Triage classes

| Class | Examples | Response |
|-------|----------|----------|
| Critical | Remote code exec, slot/auth bypass, updater compromise | Fix + hotfix release + advisory |
| High | Desync injection, deterministic crash via packet | Fix next release + advisory |
| Medium | DoS bounded by existing rate limits, log leakage | Normal release cycle |
| Low | Hardening gaps without demonstrated impact | Backlog |

## Process

1. Reproduce privately; assess blast radius (which versions, which
   platforms, does it need a malicious peer or a compromised lobby).
2. Fix on a private branch or in the decomp/port-patches submodule flow
   when the fix must land in a submodule.
3. Add a regression test (`tests/` or the netplay harnesses) — a security
   fix without a test is incomplete.
4. Write the post-mortem under `docs/bugs/` only after the fix ships
   publicly.
5. Rotate any credential that may have been exposed (Cloudflare API
   tokens, GH tokens, signing keys).

## What we will NOT do

- Ship a fix that changes wire behavior without bumping the affected
  protocol version.
- Publicly document an unpatched exploit path.
