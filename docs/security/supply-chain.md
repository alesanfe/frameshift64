# Supply-chain security

## In place

- Dependabot: GitHub Actions, npm (`services/netplay-lobby`),
  Gradle (`platforms/android`), git submodules (manual-bump policy —
  `open-pull-requests-limit: 0` because the forks carry port branches).
- CodeQL workflow (`codeql.yml`).
- `actions/attest` available in release flow.
- Release hashes: release workflow already has attestation plumbing.
- Actions pinned to full-length commit SHAs (see `.github/workflows/`).
- Submodules pin exact commits; `libultraship`/`torch`/`decomp` track
  fork branches (`ssb64`, `port-patches`) so upstream drift is opt-in.
- `.gitleaksignore` + secret-scanning hooks (pre-commit).

## Gap list (tracked)

| Gap | Tracking |
|-----|----------|
| OpenSSF Scorecard workflow | TD-012 |
| Dependency-review action on PRs | TD-013 |
| SBOM (SPDX) per release artifact | TD-014 |
| Signed releases / minisign or sigstore | TD-009 |
| Branch protection ruleset codified | repo settings — see below |

## Branch protection (apply on GitHub)

Recommended ruleset for `main`:

- Require PR before merge; 1 approval; stale-approval dismiss on push.
- Require status checks: build, lint, determinism-smoke (when the ROM
  secret is present), dependency-review.
- Require conversation resolution; block force-push and deletion.
- Restrict who can modify `.github/workflows/` (separate ruleset or
  CODEOWNERS review).

Apply via repo settings → Rules → Rulesets; this file is the record of
intent since rulesets are not versioned in-repo.
